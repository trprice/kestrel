#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "command.hpp"
#include "common.hpp"
#include "kestrel/data/databento_client.hpp"
#include "kestrel/db/bar_store.hpp"
#include "kestrel/db/databento_import_store.hpp"
#include "kestrel/db/market_trade_store.hpp"
#include "kestrel/db/quote_store.hpp"
#include "kestrel/net/http_client.hpp"
#include "kestrel/net/timed_http_client.hpp"
#include "kestrel/types/market_hours.hpp"

namespace kestrel::app {

namespace {

constexpr std::chrono::seconds kHttpTimeout{120};
constexpr std::size_t kFileBufferBytes = 256 * 1024;
constexpr const char* kVenue = "databento";

std::string_view Trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    return s;
}

std::string FormatUsd(const types::Decimal& amount) {
    const auto scaled = amount.Rescale(6, types::Rounding::kNearest);
    std::string text = scaled.Normalized().ToString();
    const auto dot = text.find('.');
    if (dot == std::string::npos) {
        return text + ".00";
    }
    if (text.size() - dot - 1 < 2) {
        text.append(2 - (text.size() - dot - 1), '0');
    }
    return text;
}

types::Result<std::vector<types::Timeframe>> ParseRollupList(
    std::string_view text) {
    std::vector<types::Timeframe> out;
    if (Trim(text).empty()) {
        return out;
    }
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t comma = std::min(text.find(',', pos), text.size());
        const std::string_view item = Trim(text.substr(pos, comma - pos));
        if (item.empty()) {
            return types::MakeError(types::ErrorCategory::kInvalidArgument,
                                    "--rollup has an empty timeframe");
        }
        auto tf = types::ParseTimeframe(item);
        if (!tf) {
            return std::unexpected(tf.error());
        }
        out.push_back(*tf);
        if (comma == text.size()) {
            break;
        }
        pos = comma + 1;
        if (pos == text.size()) {
            return types::MakeError(types::ErrorCategory::kInvalidArgument,
                                    "--rollup has a trailing comma");
        }
    }
    return out;
}

CLI::Validator SchemaValidator() {
    return CLI::Validator(
        [](std::string& text) -> std::string {
            auto schema = data::ParseDatabentoSchema(text);
            return schema ? std::string() : schema.error().message;
        },
        "SCHEMA", "databento_schema");
}

CLI::Validator CostValidator() {
    return CLI::Validator(
        [](std::string& text) -> std::string {
            auto d = types::Decimal::Parse(text);
            if (!d || d->IsNegative()) {
                return "--max-cost must be a non-negative USD amount";
            }
            return {};
        },
        "USD", "usd_amount");
}

struct Stores {
    std::unique_ptr<db::BarStore> bars;
    std::unique_ptr<db::QuoteStore> quotes;
    std::unique_ptr<db::MarketTradeStore> trades;
    std::unique_ptr<db::DatabentoImportStore> ledger;
};

data::DatabentoCallbacks MakeStoreSinks(const Stores& stores,
                                        std::size_t& total_saved,
                                        std::size_t& slice_saved) {
    data::DatabentoCallbacks cb;
    cb.on_bars = [&](const types::Symbol& sym, types::Timeframe tf,
                     std::span<const types::Bar> batch) -> types::Result<void> {
        if (!stores.bars) {
            return {};
        }
        auto n = stores.bars->Save(kVenue, sym, tf, batch);
        if (!n) {
            return std::unexpected(n.error());
        }
        total_saved += *n;
        slice_saved += *n;
        return {};
    };
    cb.on_quotes =
        [&](std::span<const types::Quote> batch) -> types::Result<void> {
        if (!stores.quotes) {
            return {};
        }
        auto n = stores.quotes->Save(batch);
        if (!n) {
            return std::unexpected(n.error());
        }
        total_saved += *n;
        slice_saved += *n;
        return {};
    };
    cb.on_trades =
        [&](const types::Symbol& sym,
            std::span<const types::MarketTrade> batch) -> types::Result<void> {
        if (!stores.trades) {
            return {};
        }
        auto n = stores.trades->Save(kVenue, sym, batch);
        if (!n) {
            return std::unexpected(n.error());
        }
        total_saved += *n;
        slice_saved += *n;
        return {};
    };
    return cb;
}

struct CommandOptions {
    data::DatabentoSchema schema = data::DatabentoSchema::kOhlcv1m;
    std::optional<std::string> dataset;
    std::optional<types::Timestamp> from;
    std::optional<types::Timestamp> to;
    std::string file;
    std::string rollup_raw;
    bool dry_run = false;
    std::optional<types::Decimal> max_cost;
    bool store = false;
    bool force = false;
};

class DatabentoCommand : public Command {
   public:
    explicit DatabentoCommand(CommandOptions opts) : mOpts(std::move(opts)) {}

    int Run(const config::Configuration& config) override {
        if (config.GetProvider() != config::Provider::kDatabento) {
            spdlog::error(
                "databento is for provider = \"databento\"; configured "
                "provider is \"{}\"",
                config::ToString(config.GetProvider()));
            return 2;
        }
        const auto& cfg =
            dynamic_cast<const config::DatabentoConfiguration&>(config);

        for (const auto& sym : cfg.GetSymbols()) {
            if (auto native = data::ToDatabentoSymbol(sym); !native) {
                spdlog::error("symbol: {}", native.error().ToString());
                return 2;
            }
        }

        auto parsed_rollups = ParseRollupList(mOpts.rollup_raw);
        if (!parsed_rollups) {
            spdlog::error("--rollup: {}", parsed_rollups.error().message);
            return 2;
        }
        auto rollups =
            data::ValidateDatabentoRollups(mOpts.schema, *parsed_rollups);
        if (!rollups) {
            spdlog::error("--rollup: {}", rollups.error().message);
            return 2;
        }

        const types::Timestamp now = types::Now();
        std::shared_ptr<const types::MarketHours> hours;
        try {
            auto calendar = std::make_shared<const types::UsEquityCalendar>();
            (void)calendar->DateAt(now);
            hours = std::make_shared<const types::MarketHours>(
                std::move(calendar), cfg.ExtendedHours());
        } catch (const std::exception& e) {
            spdlog::error(
                "databento needs the America/New_York time zone (install "
                "tzdata): {}",
                e.what());
            return 1;
        }

        if (!mOpts.file.empty()) {
            return RunFile(cfg, *rollups, hours, now);
        }
        return RunHttp(cfg, *rollups, hours, now);
    }

   private:
    void AttachStores(const std::shared_ptr<db::Database>& database,
                      Stores& stores) const {
        stores.ledger = std::make_unique<db::DatabentoImportStore>(database);
        if (data::BarTimeframeOf(mOpts.schema)) {
            stores.bars = std::make_unique<db::BarStore>(database);
        } else if (data::IsQuoteSchema(mOpts.schema)) {
            stores.quotes = std::make_unique<db::QuoteStore>(database);
        } else {
            stores.trades = std::make_unique<db::MarketTradeStore>(database);
        }
    }

    // Refuses (returning 2) when bars of a timeframe this import writes are
    // stored for one of its symbols on another grid: native ohlcv-1h bars
    // start on the hour, rollups at the session's open (09:30, or 07:00 with
    // extended_hours), and a backtest would replay both sets, overlapping.
    // Returns 0 when there is nothing to check or nothing off the grid.
    int CheckBarGrids(const config::DatabentoConfiguration& cfg,
                      const std::vector<types::Timeframe>& rollups,
                      const Stores& stores) const {
        if (!stores.ledger || !stores.bars) {
            return 0;
        }
        struct Grid {
            types::Timeframe timeframe;
            int anchor;  // minutes past New York midnight
            std::string target;
        };
        std::vector<Grid> grids;
        if (mOpts.schema == data::DatabentoSchema::kOhlcv1h) {
            grids.push_back({types::Timeframe::kH1, 0, "ohlcv-1h"});
        }
        const int session_open = cfg.ExtendedHours() ? 7 * 60 : 9 * 60 + 30;
        for (const types::Timeframe tf : rollups) {
            grids.push_back({tf, session_open,
                             "rollup:" + std::string(types::ToString(tf))});
        }
        for (const Grid& grid : grids) {
            const auto length = types::FixedDuration(grid.timeframe);
            if (!length) {
                continue;
            }
            const int minutes = static_cast<int>(length->count() / 60);
            // A length dividing all three anchors (the hour, 07:00, 09:30:
            // 3m, 5m, 10m, 15m, 30m) has one grid whichever made the bars,
            // so its (largest) bar series need not be scanned.
            if (420 % minutes == 0 && 570 % minutes == 0) {
                continue;
            }
            for (const auto& symbol : cfg.GetSymbols()) {
                auto off = stores.ledger->FindOffGridBar(
                    kVenue, symbol, grid.timeframe, grid.anchor, minutes);
                if (!off) {
                    spdlog::error("database: {}", off.error().ToString());
                    return 1;
                }
                if (!*off) {
                    continue;
                }
                const std::string tf(types::ToString(grid.timeframe));
                // The ledger targets that wrote bars of this timeframe.
                const std::string targets =
                    grid.timeframe == types::Timeframe::kH1
                        ? "'ohlcv-1h', 'rollup:1h'"
                        : "'rollup:" + tf + "'";
                spdlog::error(
                    "{} {} bars are already stored on another grid (one opens "
                    "at {}). Native ohlcv-1h bars start on the hour; rollups "
                    "start at the session's open, 09:30 New York time, or "
                    "07:00 with extended_hours. This import ({}) would add "
                    "overlapping {} bars, and a backtest would replay both. "
                    "Delete the stored ones first: "
                    "DELETE FROM bars WHERE venue = '{}' AND symbol = '{}' AND "
                    "timeframe = '{}'; DELETE FROM databento_imports WHERE "
                    "venue = '{}' AND symbol = '{}' AND target IN ({});",
                    symbol, tf, types::FormatIso8601(**off), grid.target, tf,
                    kVenue, symbol, tf, kVenue, symbol, targets);
                return 2;
            }
        }
        return 0;
    }

    int RunFile(const config::DatabentoConfiguration& cfg,
                const std::vector<types::Timeframe>& rollups,
                const std::shared_ptr<const types::MarketHours>& hours,
                types::Timestamp now) const {
        if (mOpts.dry_run || mOpts.max_cost || mOpts.dataset || mOpts.force) {
            spdlog::error(
                "--dry-run, --max-cost, --dataset and --force apply to HTTP "
                "pulls, not --file");
            return 2;
        }
        if (mOpts.from && mOpts.to && *mOpts.to <= *mOpts.from) {
            spdlog::error("--to must be after --from");
            return 2;
        }
        if (mOpts.file != "-" &&
            (mOpts.file.ends_with(".zst") || mOpts.file.ends_with(".dbn"))) {
            spdlog::error(
                "--file reads uncompressed CSV; decompress .zst first (e.g. "
                "`zstdcat {} | kestrel_app ... databento --file -`) and export "
                ".dbn to CSV",
                mOpts.file);
            return 2;
        }

        std::vector<char> file_buf;
        std::ifstream file_in;
        if (mOpts.file != "-") {
            file_buf.resize(kFileBufferBytes);
            file_in.rdbuf()->pubsetbuf(
                file_buf.data(), static_cast<std::streamsize>(file_buf.size()));
            file_in.open(mOpts.file);
            if (!file_in) {
                spdlog::error("--file: cannot open {}", mOpts.file);
                return 2;
            }
        } else {
            std::ios::sync_with_stdio(false);
            std::cin.tie(nullptr);
        }

        Stores stores;
        if (mOpts.store) {
            auto database = OpenDatabase(cfg);
            if (!database) {
                spdlog::error("database: {}", database.error().ToString());
                return 1;
            }
            AttachStores(*database, stores);
            if (const int code = CheckBarGrids(cfg, rollups, stores); code) {
                return code;
            }
        }

        data::DatabentoParseOptions parse_opts;
        parse_opts.schema = mOpts.schema;
        parse_opts.venue = kVenue;
        parse_opts.symbols.insert(cfg.GetSymbols().begin(),
                                  cfg.GetSymbols().end());
        parse_opts.rollups = rollups;
        parse_opts.hours = hours;
        parse_opts.now = now;
        parse_opts.from = mOpts.from;
        parse_opts.to = mOpts.to;

        std::size_t total_saved = 0;
        std::size_t slice_saved = 0;
        std::istream& input = mOpts.file == "-" ? std::cin : file_in;
        auto stats = data::ParseDatabentoCsv(
            input, parse_opts,
            MakeStoreSinks(stores, total_saved, slice_saved));
        if (!stats) {
            spdlog::error("databento: {}", stats.error().ToString());
            return 1;
        }
        PrintSummary(*stats, total_saved, mOpts.store);
        std::cout.flush();
        return 0;
    }

    int RunHttp(const config::DatabentoConfiguration& cfg,
                const std::vector<types::Timeframe>& rollups,
                const std::shared_ptr<const types::MarketHours>& hours,
                types::Timestamp now) const {
        if (!mOpts.from || !mOpts.to) {
            spdlog::error(
                "databento needs either --file <PATH> or both --from and --to");
            return 2;
        }
        const types::Timestamp from = *mOpts.from;
        const types::Timestamp to = std::min(*mOpts.to, now);
        if (to <= from) {
            spdlog::error("--to must be after --from (and --from in the past)");
            return 2;
        }
        if (mOpts.dry_run == mOpts.max_cost.has_value()) {
            spdlog::error(
                "HTTP pulls from Databento cost money: pass exactly one of "
                "--dry-run (check cost only) or --max-cost <USD>");
            return 2;
        }
        if (mOpts.dry_run && mOpts.store) {
            spdlog::error(
                "--dry-run checks the cost without fetching; omit --store "
                "(sessions recorded in [database] are already excluded unless "
                "--force is set)");
            return 2;
        }
        if (!mOpts.dry_run && !mOpts.store) {
            spdlog::error(
                "HTTP pulls without --dry-run must pass --store so paid data "
                "is saved to the [database]");
            return 2;
        }

        const std::string dataset = mOpts.dataset.value_or(cfg.Dataset());
        if (dataset.empty()) {
            spdlog::error("--dataset must not be empty");
            return 2;
        }
        if (!cfg.Credentials()) {
            spdlog::error(
                "databento HTTP pulls need an API key: set "
                "KESTREL_DATABENTO_API_KEY or add [databento] api_key to the "
                "secrets file");
            return 1;
        }

        Stores stores;
        if (mOpts.store) {
            auto database = OpenDatabase(cfg);
            if (!database) {
                spdlog::error("database: {}", database.error().ToString());
                return 1;
            }
            AttachStores(*database, stores);
        } else if (cfg.GetDatabase() && !mOpts.force) {
            if (auto database = OpenDatabase(cfg)) {
                AttachStores(*database, stores);
            } else {
                spdlog::warn(
                    "database unavailable ({}); estimating cost for the full "
                    "range",
                    database.error().ToString());
            }
        }
        if (const int code = CheckBarGrids(cfg, rollups, stores); code) {
            return code;
        }

        auto http = std::make_shared<net::TimedHttpClient>(
            std::make_shared<net::CprHttpClient>(kHttpTimeout),
            std::make_shared<net::LatencyStats>(), cfg.GetHttp().slow_request);
        data::DatabentoClient::Options client_opts;
        client_opts.rest_base_url = cfg.RestUrl();
        data::DatabentoClient client(cfg.Credentials()->api_key, http,
                                     std::move(client_opts));

        std::size_t total_saved = 0;
        std::size_t slice_saved = 0;

        data::DatabentoHttpOptions http_opts;
        http_opts.dataset = dataset;
        http_opts.venue = kVenue;
        http_opts.schema = mOpts.schema;
        http_opts.symbols = cfg.GetSymbols();
        http_opts.rollups = rollups;
        http_opts.from = from;
        http_opts.to = to;
        http_opts.now = now;
        http_opts.hours = hours;
        http_opts.dry_run = mOpts.dry_run;
        http_opts.max_cost = mOpts.max_cost;
        http_opts.force = mOpts.force;
        http_opts.sinks = MakeStoreSinks(stores, total_saved, slice_saved);
        if (stores.ledger) {
            http_opts.load_imported = [&](const types::Symbol& sym,
                                          const std::string& target) {
                return stores.ledger->Load(kVenue, dataset, sym, target, from,
                                           to);
            };
            http_opts.record_imported =
                [&](const types::Symbol& sym,
                    std::span<const std::string> targets,
                    std::span<const data::DatabentoHttpOptions::Interval>
                        slices) {
                    return stores.ledger->Record(kVenue, dataset, sym, targets,
                                                 slices);
                };
        }
        if (stores.bars && !rollups.empty()) {
            http_opts.load_stored_bars = [&](const types::Symbol& sym,
                                             types::Timestamp start,
                                             types::Timestamp end) {
                return stores.bars->Load(kVenue, sym, types::Timeframe::kM1,
                                         start, end);
            };
        }
        http_opts.on_slice = [&](const types::Symbol& sym,
                                 types::Timestamp s_start,
                                 types::Timestamp s_end,
                                 const data::DatabentoParseStats& stats) {
            PrintSliceLine(sym, s_start, s_end, stats,
                           std::exchange(slice_saved, 0));
        };

        auto result = data::ImportDatabentoHttp(client, http_opts);
        if (!result) {
            spdlog::error("databento: {}", result.error().ToString());
            return 1;
        }
        if (result->clamped_to) {
            spdlog::info("databento: clamped --to to dataset available end {}",
                         types::FormatIso8601(*result->clamped_to));
        }
        if (result->slices_from_stored > 0) {
            std::cout << result->slices_from_stored
                      << " slice(s) with their 1m bars already stored "
                      << (mOpts.dry_run || result->cost_exceeded ? "would be"
                                                                 : "were")
                      << " rolled up from those bars, at no cost\n";
        }
        if (result->slices_planned == 0) {
            if (result->slices_from_stored > 0) {
                if (!mOpts.dry_run) {
                    PrintSummary(result->parsed, total_saved, true);
                }
                return 0;
            }
            if (result->slices_skipped > 0) {
                std::cout << "nothing to fetch: all open sessions in the range "
                             "are already stored (pass --force to re-fetch)\n";
            } else {
                std::cout
                    << "nothing to fetch: no open available sessions in the "
                       "range\n";
            }
            return 0;
        }

        std::cout << "estimated cost: "
                  << (result->cost_is_upper_bound ? "up to $" : "$")
                  << FormatUsd(result->estimated_cost) << " ("
                  << result->slices_planned << " slice(s) on " << dataset << ' '
                  << data::ToString(mOpts.schema);
        if (result->slices_skipped > 0) {
            std::cout << ", " << result->slices_skipped << " already stored";
        }
        std::cout << ")\n";

        if (mOpts.dry_run) {
            return 0;
        }
        if (result->cost_exceeded) {
            spdlog::error("estimated cost ${} exceeds --max-cost ${}; refusing",
                          FormatUsd(result->estimated_cost),
                          FormatUsd(*mOpts.max_cost));
            return 2;
        }

        PrintSummary(result->parsed, total_saved, true);
        return 0;
    }

    void PrintSliceLine(const types::Symbol& symbol, types::Timestamp start,
                        types::Timestamp end,
                        const data::DatabentoParseStats& stats,
                        std::size_t saved) const {
        std::cout << symbol << ' ' << types::FormatIso8601(start) << " .. "
                  << types::FormatIso8601(end) << ": ";
        if (data::BarTimeframeOf(mOpts.schema)) {
            std::cout << stats.bars << " bars";
            if (stats.rollup_bars > 0) {
                std::cout << " (+" << stats.rollup_bars << " rollup)";
            }
            std::cout << ", " << saved << " stored";
        } else if (data::IsQuoteSchema(mOpts.schema)) {
            std::cout << stats.quotes << " quotes, " << saved << " new";
        } else {
            std::cout << stats.trades << " trades, " << saved << " new";
        }
        std::cout << '\n';
    }

    void PrintSummary(const data::DatabentoParseStats& stats, std::size_t saved,
                      bool stored) const {
        if (data::BarTimeframeOf(mOpts.schema)) {
            std::cout << "imported " << stats.bars << " bars";
            if (stats.rollup_bars > 0) {
                std::cout << " and " << stats.rollup_bars << " rollup bars";
            }
            if (stored) {
                std::cout << ", " << saved << " written to bars";
            }
        } else if (data::IsQuoteSchema(mOpts.schema)) {
            std::cout << "imported " << stats.quotes << " quotes";
            if (stored) {
                std::cout << ", " << saved << " new to quotes";
            }
        } else {
            std::cout << "imported " << stats.trades << " trades";
            if (stored) {
                std::cout << ", " << saved << " new to market_trades";
            }
        }
        if (stats.skipped_symbol > 0 || stats.skipped_outside > 0) {
            std::cout << " (skipped:";
            if (stats.skipped_symbol > 0) {
                std::cout << ' ' << stats.skipped_symbol << " other symbol(s)";
                if (stats.skipped_outside > 0) {
                    std::cout << ',';
                }
            }
            if (stats.skipped_outside > 0) {
                std::cout << ' ' << stats.skipped_outside
                          << " outside session/range";
            }
            std::cout << ')';
        }
        std::cout << '\n';
    }

    CommandOptions mOpts;
};

struct Args {
    std::string schema;
    std::string dataset;
    std::string from;
    std::string to;
    std::string file;
    std::string rollup;
    bool dry_run = false;
    std::string max_cost;
    bool store = false;
    bool force = false;
};

}  // namespace

void AddDatabentoCommand(CLI::App& app, CommandSlot& slot) {
    auto* sub = AddSubcommand(
        app, "databento",
        "Import historical US equity bars, quotes or trades from Databento "
        "(HTTP range or local CSV file)");
    auto args = std::make_shared<Args>();
    sub->add_option("--schema", args->schema,
                    "Schema: ohlcv-1m, ohlcv-1h, ohlcv-1d, bbo-1s, bbo-1m, "
                    "tbbo, mbp-1 or trades")
        ->required()
        ->check(SchemaValidator())
        ->type_name("SCHEMA");
    sub->add_option("--dataset", args->dataset,
                    "Dataset code (overrides [databento] dataset, e.g. "
                    "EQUS.MINI)")
        ->type_name("DATASET");
    sub->add_option("--from", args->from,
                    "Start, YYYY-MM-DD or an ISO 8601 time (UTC)")
        ->check(DateValidator())
        ->type_name("DATE");
    sub->add_option("--to", args->to,
                    "End (exclusive), same format as --from; capped at now")
        ->check(DateValidator())
        ->type_name("DATE");
    sub->add_option("--file", args->file,
                    "Read uncompressed Databento CSV from a file, or '-' for "
                    "stdin, instead of HTTP")
        ->type_name("PATH");
    sub->add_option("--rollup", args->rollup,
                    "Also aggregate ohlcv-1m into larger intraday timeframes, "
                    "comma-separated (e.g. 5m,15m,30m)")
        ->type_name("TFS");
    sub->add_flag("--dry-run", args->dry_run,
                  "HTTP only: query metadata.get_cost and exit without "
                  "downloading");
    sub->add_option("--max-cost", args->max_cost,
                    "HTTP only: refuse if estimated cost exceeds this USD cap "
                    "(e.g. 5.00)")
        ->check(CostValidator())
        ->type_name("USD");
    sub->add_flag("--store", args->store,
                  "Save imported records to the [database] under venue "
                  "\"databento\" (required for HTTP pulls unless --dry-run)");
    sub->add_flag("--force", args->force,
                  "HTTP: re-fetch sessions already recorded in the [database]");
    sub->callback([&slot, args] {
        CommandOptions opts;
        opts.schema = *data::ParseDatabentoSchema(args->schema);
        if (!args->dataset.empty()) {
            opts.dataset = args->dataset;
        }
        if (!args->from.empty()) {
            opts.from = *types::ParseTimestamp(args->from);
        }
        if (!args->to.empty()) {
            opts.to = *types::ParseTimestamp(args->to);
        }
        opts.file = args->file;
        opts.rollup_raw = args->rollup;
        opts.dry_run = args->dry_run;
        if (!args->max_cost.empty()) {
            opts.max_cost = *types::Decimal::Parse(args->max_cost);
        }
        opts.store = args->store;
        opts.force = args->force;
        slot = std::make_unique<DatabentoCommand>(std::move(opts));
    });
}

}  // namespace kestrel::app
