// kestrel_app: small command-line driver that exercises the data and trading
// interfaces without knowing which provider or mode is configured.

#include <spdlog/spdlog.h>

#include <CLI/CLI.hpp>
#include <chrono>
#include <csignal>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include "kestrel/config/configuration.hpp"
#include "kestrel/db/bar_store.hpp"
#include "kestrel/db/quote_store.hpp"
#include "kestrel/factory.hpp"

namespace {

using namespace kestrel;

std::string Opt(const std::optional<types::Decimal>& d) {
    return d ? d->ToString() : "-";
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

    std::optional<std::pair<types::Timeframe, int>> bars;
    if (init_db) {
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
