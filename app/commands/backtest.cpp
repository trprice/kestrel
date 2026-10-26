#include "kestrel/backtest/backtest.hpp"

#include <spdlog/spdlog.h>

#include <iostream>

#include "command.hpp"
#include "common.hpp"
#include "kestrel/backtest/replay_data_source.hpp"
#include "kestrel/factory.hpp"

namespace kestrel::app {

namespace {

class BacktestCommand : public Command {
   public:
    BacktestCommand(std::string strategy_name, types::Timestamp from,
                    types::Timestamp to)
        : mName(std::move(strategy_name)), mFrom(from), mTo(to) {}

    int Run(const config::Configuration& config) override {
        auto limits = ResolveRiskLimits(mName, config);
        if (!limits) {
            spdlog::error("risk: {}", limits.error().ToString());
            return 1;
        }
        auto strategy = CreateStrategy(mName, config);
        if (!strategy) {
            spdlog::error("strategy: {}", strategy.error().ToString());
            return 1;
        }
        auto source = CreateDataSource(config);
        if (!source) {
            spdlog::error("startup: {}", source.error().ToString());
            return 1;
        }
        // The database is optional: without a [database] section, or if it
        // cannot be reached, everything comes from the venue.
        std::unique_ptr<db::BarStore> bar_store;
        std::unique_ptr<db::QuoteStore> quote_store;
        backtest::StoredData stored;
        auto database = OpenDatabase(config);
        if (database) {
            bar_store = std::make_unique<db::BarStore>(*database);
            quote_store = std::make_unique<db::QuoteStore>(*database);
            stored.bars = bar_store.get();
            stored.quotes = quote_store.get();
        } else if (database.error().category !=
                   types::ErrorCategory::kNotFound) {
            spdlog::warn("database unavailable, using the venue: {}",
                         database.error().ToString());
        }
        auto replay = backtest::LoadReplayData(**source, (*strategy)->Needs(),
                                               mFrom, mTo, stored);
        if (!replay) {
            return Fail("data", replay.error());
        }

        backtest::BacktestOptions options;
        options.paper = config.GetPaperOptions();
        options.risk = *limits;
        options.start = mFrom;
        options.end = mTo;
        auto result = backtest::RunBacktest(*replay, std::move(*strategy),
                                            std::move(options));
        if (!result) {
            return Fail("backtest", result.error());
        }
        PrintReport(*result);
        return 0;
    }

   private:
    // A bad range is a usage error; anything else a failure.
    static int Fail(const char* what, const types::Error& error) {
        spdlog::error("{}: {}", what, error.ToString());
        return error.category == types::ErrorCategory::kInvalidArgument ? 2 : 1;
    }

    static void PrintReport(const backtest::BacktestResult& result) {
        std::cout << "bars replayed: " << result.bars_replayed
                  << "\nquotes replayed: " << result.quotes_replayed
                  << "\nfills: " << result.fills.size() << '\n';
        for (const auto& f : result.fills) {
            std::cout << types::FormatIso8601(f.time) << ' ' << f.symbol << ' '
                      << (f.side == types::Side::kBuy ? "buy" : "sell") << ' '
                      << f.quantity << " @ " << f.price << " fee=" << f.fee
                      << ' ' << f.fee_asset << '\n';
        }
        for (const auto& p : result.positions) {
            std::cout << "position " << p.symbol << ": qty=" << p.quantity
                      << " avg=" << p.average_price
                      << " realized=" << p.realized_pnl << '\n';
        }
        for (const auto& b : result.balances) {
            std::cout << "balance " << b.asset << ": total=" << b.total << '\n';
        }
    }

    std::string mName;
    types::Timestamp mFrom;
    types::Timestamp mTo;
};

struct Args {
    std::string strategy;
    std::string from;
    std::string to;
};

}  // namespace

void AddBacktestCommand(CLI::App& app, CommandSlot& slot) {
    auto* sub = AddSubcommand(
        app, "backtest",
        "Backtest the named strategy over --from..--to using stored bars and "
        "quotes from the database, falling back to the provider for missing "
        "bars");
    auto args = std::make_shared<Args>();
    sub->add_option("strategy", args->strategy, "Strategy name")->required();
    sub->add_option("--from", args->from,
                    "Start, YYYY-MM-DD or an ISO 8601 time (UTC)")
        ->required()
        ->check(DateValidator())
        ->type_name("DATE");
    sub->add_option("--to", args->to, "End (exclusive), same format as --from")
        ->required()
        ->check(DateValidator())
        ->type_name("DATE");
    sub->callback([&slot, args] {
        slot = std::make_unique<BacktestCommand>(
            args->strategy, *types::ParseTimestamp(args->from),
            *types::ParseTimestamp(args->to));
    });
}

}  // namespace kestrel::app
