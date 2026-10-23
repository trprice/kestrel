// kestrel_app: small command-line driver that exercises the data and trading
// interfaces without knowing which provider or mode is configured.

#include <pthread.h>
#include <signal.h>
#include <spdlog/spdlog.h>

#include <CLI/CLI.hpp>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include "kestrel/backtest/backtest.hpp"
#include "kestrel/backtest/replay_data_source.hpp"
#include "kestrel/config/configuration.hpp"
#include "kestrel/db/bar_store.hpp"
#include "kestrel/db/quote_store.hpp"
#include "kestrel/factory.hpp"
#include "kestrel/strategy/runner.hpp"

namespace {

using namespace kestrel;

std::string Opt(const std::optional<types::Decimal>& d) {
    return d ? d->ToString() : "-";
}

// "2024-03-01" or a full "2024-03-01T12:00:00Z".
types::Result<types::Timestamp> ParseWhen(const std::string& text) {
    if (text.size() == 10) {
        return types::ParseIso8601(text + "T00:00:00Z");
    }
    return types::ParseIso8601(text);
}

// Ctrl-C / SIGTERM stop the runner. The signals are blocked in every thread
// (call BlockStopSignals before any thread starts, so they inherit the mask)
// and a watch thread collects them with sigtimedwait, because
// Runner::Stop() takes a mutex and so cannot be called from a signal handler.
// A second signal gives up on a clean stop.
sigset_t StopSignals() {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    return set;
}

void BlockStopSignals() {
    const sigset_t set = StopSignals();
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
}

class StopWatcher {
   public:
    explicit StopWatcher(strategy::Runner& runner)
        : mThread([this, &runner] {
              const sigset_t set = StopSignals();
              bool stopping = false;
              while (!mDone) {
                  const timespec timeout{0, 200'000'000};
                  if (sigtimedwait(&set, nullptr, &timeout) < 0) {
                      continue;
                  }
                  if (stopping) {
                      spdlog::error("second signal: exiting immediately");
                      std::_Exit(130);
                  }
                  stopping = true;
                  spdlog::info("stopping (signal again to force exit)");
                  runner.Stop();
              }
          }) {}
    ~StopWatcher() {
        mDone = true;
        mThread.join();
    }

   private:
    std::atomic<bool> mDone{false};
    std::thread mThread;
};

int RunStrategy(const config::Configuration& cfg, const std::string& name) {
    BlockStopSignals();
    auto limits = ResolveRiskLimits(name, cfg);
    if (!limits) {
        spdlog::error("risk: {}", limits.error().ToString());
        return 1;
    }
    if (auto ok = ValidateForRun(cfg, *limits); !ok) {
        spdlog::error("run: {}", ok.error().ToString());
        return 1;
    }
    auto strategy = CreateStrategy(name, cfg);
    if (!strategy) {
        spdlog::error("strategy: {}", strategy.error().ToString());
        return 1;
    }
    auto components = CreateComponents(cfg);
    if (!components) {
        spdlog::error("startup: {}", components.error().ToString());
        return 1;
    }
    std::shared_ptr<trading::TradingVenue> venue = std::move(components->venue);
    spdlog::info("running {} on {} mode={} venue={}", name,
                 components->data_source->Name(),
                 config::ToString(cfg.GetMode()), venue->Name());

    strategy::RunnerOptions options;
    options.risk = *limits;
    strategy::Runner runner(components->data_source, venue,
                            std::move(*strategy), options);
    if (const auto gate = runner.RiskGate()) {
        // The loss count starts here. If it cannot, trading without it would
        // be trading without the limit.
        if (auto baseline = gate->ResetDailyBaseline(); !baseline) {
            spdlog::error("risk: cannot start the daily loss count: {}",
                          baseline.error().ToString());
            return 1;
        }
    } else {
        spdlog::warn("no [risk] limits: the strategy trades unchecked");
    }

    StopWatcher watcher(runner);
    if (auto ran = runner.Run(); !ran) {
        spdlog::error("run: {}", ran.error().ToString());
        return 1;
    }
    spdlog::info("stopped");
    return 0;
}

int RunBacktestCommand(const config::Configuration& cfg,
                       const std::string& name, const std::string& from_text,
                       const std::string& to_text) {
    auto from = ParseWhen(from_text);
    auto to = ParseWhen(to_text);
    if (!from || !to) {
        std::cerr << "--from/--to: "
                  << (!from ? from.error() : to.error()).ToString() << '\n';
        return 2;
    }
    if (*to <= *from) {
        std::cerr << "--to must be after --from\n";
        return 2;
    }
    auto limits = ResolveRiskLimits(name, cfg);
    if (!limits) {
        spdlog::error("risk: {}", limits.error().ToString());
        return 1;
    }
    auto strategy = CreateStrategy(name, cfg);
    if (!strategy) {
        spdlog::error("strategy: {}", strategy.error().ToString());
        return 1;
    }
    auto source = CreateDataSource(cfg);
    if (!source) {
        spdlog::error("startup: {}", source.error().ToString());
        return 1;
    }

    // Everything the strategy trades, from the provider, plus enough bars
    // before --from to fill its warmup window.
    const auto needs = (*strategy)->Needs();
    auto replay = std::make_shared<backtest::ReplayDataSource>();
    for (const auto& symbol : needs.symbols) {
        auto info = (*source)->GetSymbolInfo(symbol);
        if (!info) {
            spdlog::error("{}: {}", symbol, info.error().ToString());
            return 1;
        }
        replay->SetSymbolInfo(*info);
        for (const auto timeframe : needs.bar_timeframes) {
            const auto length = types::FixedDuration(timeframe);
            if (!length) {
                spdlog::error("backtests cannot replay timeframe {}",
                              types::ToString(timeframe));
                return 1;
            }
            const auto first = *from - *length * (needs.warmup_bars + 1);
            auto bars = (*source)->GetBars(symbol, timeframe, first, *to);
            if (!bars) {
                spdlog::error("{} {}: {}", symbol, types::ToString(timeframe),
                              bars.error().ToString());
                return 1;
            }
            spdlog::info("{} {}: {} bars loaded", symbol,
                         types::ToString(timeframe), bars->size());
            replay->AddBars(symbol, timeframe, std::move(*bars));
        }
    }

    backtest::BacktestOptions options;
    options.paper = cfg.GetPaperOptions();
    options.risk = *limits;
    options.start = *from;
    options.end = *to;
    auto result =
        backtest::RunBacktest(replay, std::move(*strategy), std::move(options));
    if (!result) {
        spdlog::error("backtest: {}", result.error().ToString());
        return 1;
    }

    std::cout << "bars replayed: " << result->bars_replayed
              << "\nfills: " << result->fills.size() << '\n';
    for (const auto& f : result->fills) {
        std::cout << types::FormatIso8601(f.time) << ' ' << f.symbol << ' '
                  << (f.side == types::Side::kBuy ? "buy" : "sell") << ' '
                  << f.quantity << " @ " << f.price << " fee=" << f.fee << ' '
                  << f.fee_asset << '\n';
    }
    for (const auto& p : result->positions) {
        std::cout << "position " << p.symbol << ": qty=" << p.quantity
                  << " avg=" << p.average_price
                  << " realized=" << p.realized_pnl << '\n';
    }
    for (const auto& b : result->balances) {
        std::cout << "balance " << b.asset << ": total=" << b.total << '\n';
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    CLI::App app{
        "Exercises the data and trading interfaces of the configured "
        "provider. Does only what is asked: with just --config it prints the "
        "last 10 completed 1h bars (the default for --bars); --stream adds "
        "or selects live quotes; --stream-bars selects closing bars. --store "
        "saves the bars and quotes to the [database] in the configuration."};
    app.set_version_flag("--version", "kestrel 0.1.0");

    std::string config_path;
    app.add_option("-c,--config", config_path, "Path to the TOML configuration")
        ->required()
        ->check(CLI::ExistingFile);

    int bars_count = 0;
    auto* bars_opt =
        app.add_option("--bars",
                       "Print the last <count> completed bars of "
                       "<timeframe> (e.g. 1h 24) for each symbol, with symbol "
                       "info and balances. Default when neither "
                       "--bars nor --stream is given: 1h 10")
            ->expected(2)
            ->type_name("TIMEFRAME COUNT");

    std::optional<int> stream_seconds;
    auto* stream_opt = app.add_option("--stream", stream_seconds,
                                      "Print live quotes for <seconds> seconds")
                           ->check(CLI::NonNegativeNumber)
                           ->type_name("SECONDS");

    auto* stream_bars_opt =
        app.add_option("--stream-bars",
                       "Stream completed bars of <timeframe> for <seconds> "
                       "seconds (providers that offer it)")
            ->expected(2)
            ->type_name("TIMEFRAME SECONDS");

    std::string run_name;
    auto* run_opt =
        app.add_option("--run", run_name,
                       "Run the named strategy (sma_crossover) on live data "
                       "with the [strategy] parameters, trading through the "
                       "paper or live venue and the [risk] limits until "
                       "Ctrl-C or SIGTERM. Live mode requires [risk]")
            ->type_name("STRATEGY");

    std::string backtest_name;
    auto* backtest_opt =
        app.add_option("--backtest", backtest_name,
                       "Backtest the named strategy over --from..--to using "
                       "historical bars from the provider")
            ->type_name("STRATEGY");
    std::string from_text;
    std::string to_text;
    app.add_option("--from", from_text,
                   "Backtest start, YYYY-MM-DD or an ISO 8601 time (UTC)")
        ->type_name("DATE");
    app.add_option("--to", to_text,
                   "Backtest end (exclusive), same format as --from")
        ->type_name("DATE");

    bool init_db = false;
    app.add_flag("--init-db", init_db,
                 "Create or update the database schema (tables and "
                 "hypertables) and exit; see scripts/setup_database.sh");

    bool store = false;
    app.add_flag("--store", store,
                 "Save the bars from --bars and --stream-bars and the quotes "
                 "from --stream to the [database] (PostgreSQL + TimescaleDB)");

    bool verbose = false;
    app.add_flag("-v,--verbose", verbose, "Enable debug logging");

    CLI11_PARSE(app, argc, argv);

    const bool run_mode = run_opt->count() > 0;
    const bool backtest_mode = backtest_opt->count() > 0;
    if (run_mode || backtest_mode) {
        if (run_mode && backtest_mode) {
            std::cerr << "--run and --backtest cannot be combined\n";
            return 2;
        }
        if (init_db || store || bars_opt->count() > 0 ||
            stream_opt->count() > 0 || stream_bars_opt->count() > 0) {
            std::cerr << "--run and --backtest cannot be combined with other "
                         "actions\n";
            return 2;
        }
    }
    if (backtest_mode && (from_text.empty() || to_text.empty())) {
        std::cerr << "--backtest needs --from and --to\n";
        return 2;
    }
    if (!backtest_mode && (!from_text.empty() || !to_text.empty())) {
        std::cerr << "--from and --to apply only to --backtest\n";
        return 2;
    }

    std::optional<std::pair<types::Timeframe, int>> bars;
    if (run_mode || backtest_mode) {
        // strategy commands do not print bars
    } else if (init_db) {
        // schema only; no bars or streams
    } else if (bars_opt->count() == 0 && stream_opt->count() == 0 &&
               stream_bars_opt->count() == 0) {
        bars = {types::Timeframe::kH1, 10};
    } else if (bars_opt->count() > 0) {
        const auto raw = bars_opt->as<std::vector<std::string>>();
        auto tf = types::ParseTimeframe(raw[0]);
        if (!tf) {
            std::cerr << "--bars: " << tf.error().ToString() << '\n';
            return 2;
        }
        try {
            bars_count = std::stoi(raw[1]);
        } catch (const std::exception&) {
            bars_count = 0;
        }
        if (bars_count <= 0) {
            std::cerr << "--bars: count must be a positive integer, got '"
                      << raw[1] << "'\n";
            return 2;
        }
        if (!types::FixedDuration(*tf)) {
            std::cerr << "--bars: " << raw[0]
                      << " has no fixed length; pick another timeframe\n";
            return 2;
        }
        bars = {*tf, bars_count};
    }
    std::optional<std::pair<types::Timeframe, int>> stream_bars;
    if (stream_bars_opt->count() > 0) {
        const auto raw = stream_bars_opt->as<std::vector<std::string>>();
        auto tf = types::ParseTimeframe(raw[0]);
        if (!tf) {
            std::cerr << "--stream-bars: " << tf.error().ToString() << '\n';
            return 2;
        }
        int seconds = -1;
        try {
            seconds = std::stoi(raw[1]);
        } catch (const std::exception&) {
        }
        if (seconds < 0) {
            std::cerr << "--stream-bars: seconds must be a non-negative "
                         "integer, got '"
                      << raw[1] << "'\n";
            return 2;
        }
        stream_bars = {*tf, seconds};
    }
    if (init_db && (store || bars_opt->count() > 0 || stream_opt->count() > 0 ||
                    stream_bars_opt->count() > 0)) {
        std::cerr << "--init-db cannot be combined with other actions\n";
        return 2;
    }
    if (store && !bars && !stream_bars && !stream_seconds) {
        std::cerr << "--store needs --bars, --stream-bars or --stream\n";
        return 2;
    }
    if (verbose) {
        spdlog::set_level(spdlog::level::debug);
    }

    auto cfg = config::LoadConfiguration(config_path);
    if (!cfg) {
        spdlog::error("config: {}", cfg.error().ToString());
        return 1;
    }
    if (run_mode) {
        return RunStrategy(**cfg, run_name);
    }
    if (backtest_mode) {
        return RunBacktestCommand(**cfg, backtest_name, from_text, to_text);
    }
    std::unique_ptr<db::BarStore> bar_store;
    std::unique_ptr<db::QuoteWriter> quote_writer;
    if (store || init_db) {
        const auto& options = (*cfg)->GetDatabase();
        if (!options) {
            spdlog::error("--store/--init-db need a [database] section in {}",
                          config_path);
            return 1;
        }
        auto database = db::Database::Connect(*options);
        if (!database) {
            spdlog::error("database: {}", database.error().ToString());
            return 1;
        }
        std::shared_ptr<db::Database> shared = std::move(*database);
        if (auto migrated = shared->Migrate(); !migrated) {
            spdlog::error("database: {}", migrated.error().ToString());
            return 1;
        }
        if (init_db) {
            std::cout << "database schema is up to date\n";
            return 0;
        }
        bar_store = std::make_unique<db::BarStore>(shared);
        quote_writer = std::make_unique<db::QuoteWriter>(std::move(shared));
    }

    auto components = CreateComponents(**cfg);
    if (!components) {
        spdlog::error("startup: {}", components.error().ToString());
        return 1;
    }
    auto& [data_source, venue] = *components;
    spdlog::info("provider={} mode={} data={} venue={}",
                 config::ToString((*cfg)->GetProvider()),
                 config::ToString((*cfg)->GetMode()), data_source->Name(),
                 venue->Name());

    if (bars) {
        for (const auto& symbol : (*cfg)->GetSymbols()) {
            if (auto info = data_source->GetSymbolInfo(symbol)) {
                std::cout << symbol << ": tick=" << info->tick_size
                          << " lot=" << info->lot_size
                          << " min=" << info->min_order_size << '\n';
            } else {
                spdlog::error("{}: {}", symbol, info.error().ToString());
            }
            const auto length = *types::FixedDuration(bars->first);
            const auto end = types::Now();
            const auto start = end - length * bars->second;
            auto result = data_source->GetBars(symbol, bars->first, start, end);
            if (!result) {
                spdlog::error("{}: {}", symbol, result.error().ToString());
                continue;
            }
            if (bar_store) {
                auto saved = bar_store->Save(std::string(data_source->Name()),
                                             symbol, bars->first, *result);
                if (saved) {
                    spdlog::info("{}: stored {} {} bars", symbol, *saved,
                                 types::ToString(bars->first));
                } else {
                    spdlog::error("{}: store: {}", symbol,
                                  saved.error().ToString());
                }
            }
            for (const auto& b : *result) {
                std::cout << symbol << ' ' << types::FormatIso8601(b.open_time)
                          << " O=" << b.open << " H=" << b.high
                          << " L=" << b.low << " C=" << b.close
                          << " V=" << b.volume << '\n';
            }
        }

        if (auto balances = venue->GetBalances()) {
            for (const auto& b : *balances) {
                std::cout << "balance " << b.asset << ": total=" << b.total
                          << " available=" << b.available
                          << " locked=" << b.locked << '\n';
            }
        } else {
            spdlog::warn("balances: {}", balances.error().ToString());
        }
    }

    if (stream_bars) {
        std::mutex out_mutex;
        const std::string venue(data_source->Name());
        data::BarCallback store_bar =
            bar_store ? bar_store->MakeStreamSink(venue) : data::BarCallback{};
        auto sub = data_source->StreamBars(
            (*cfg)->GetSymbols(), stream_bars->first,
            [&](const types::Symbol& symbol, types::Timeframe tf,
                const types::Bar& b) {
                {
                    std::lock_guard lock(out_mutex);
                    std::cout << symbol << ' ' << types::ToString(tf) << ' '
                              << types::FormatIso8601(b.open_time)
                              << " O=" << b.open << " H=" << b.high
                              << " L=" << b.low << " C=" << b.close
                              << " V=" << b.volume << '\n';
                }
                if (store_bar) {
                    store_bar(symbol, tf, b);
                }
            });
        if (!sub) {
            spdlog::error("stream-bars: {}", sub.error().ToString());
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::seconds(stream_bars->second));
        (*sub)->Stop();
    }

    if (stream_seconds) {
        // Quotes are printed from the stream thread; std::cout writes are
        // serialised here by a lock because several symbols can update at
        // once.
        std::mutex out_mutex;
        auto sub = data_source->StreamQuotes(
            (*cfg)->GetSymbols(), [&](const types::Quote& q) {
                std::lock_guard lock(out_mutex);
                std::cout << types::FormatIso8601(q.received_time) << ' '
                          << q.symbol << " bid=" << Opt(q.bid)
                          << " ask=" << Opt(q.ask) << " last=" << Opt(q.last)
                          << '\n';
                if (quote_writer) {
                    quote_writer->Push(q);
                }
            });
        if (!sub) {
            spdlog::error("stream: {}", sub.error().ToString());
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::seconds(*stream_seconds));
        (*sub)->Stop();
        if (quote_writer) {
            quote_writer->Flush();
        }
    }
    return 0;
}
