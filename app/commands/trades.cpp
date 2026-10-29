#include <spdlog/spdlog.h>

#include <algorithm>
#include <iostream>

#include "command.hpp"
#include "common.hpp"
#include "kestrel/db/market_trade_store.hpp"
#include "kestrel/factory.hpp"

namespace kestrel::app {

namespace {

constexpr std::chrono::hours kChunk{24};

// Fetches the venue's public trades for the configured symbols over
// --from..--to, a day at a time, and prints a line per day. With --store they
// are saved to the [database] (market_trades); each day then starts from the
// newest trade already stored in it, so an interrupted backfill picks up where
// it stopped. Market data only: no account, no credentials.
class TradesCommand : public Command {
   public:
    TradesCommand(types::Timestamp from, types::Timestamp to, bool store)
        : mFrom(from), mTo(to), mStore(store) {}

    int Run(const config::Configuration& config) override {
        const types::Timestamp to = std::min(mTo, types::Now());
        if (to <= mFrom) {
            spdlog::error("--to must be after --from (and --from in the past)");
            return 2;
        }
        std::unique_ptr<db::MarketTradeStore> store;
        if (mStore) {
            auto database = OpenDatabase(config);
            if (!database) {
                spdlog::error("database: {}", database.error().ToString());
                return 1;
            }
            store = std::make_unique<db::MarketTradeStore>(std::move(*database));
        }
        auto source = CreateDataSource(config);
        if (!source) {
            spdlog::error("startup: {}", source.error().ToString());
            return 1;
        }
        const std::string venue((*source)->Name());

        std::size_t fetched_total = 0;
        std::size_t stored_total = 0;
        for (const types::Symbol& symbol : config.GetSymbols()) {
            for (types::Timestamp day = mFrom; day < to; day += kChunk) {
                const types::Timestamp day_end = std::min(day + kChunk, to);
                types::Timestamp since = day;
                if (store) {
                    auto newest = store->Newest(venue, symbol, day, day_end);
                    if (!newest) {
                        spdlog::error("database: {}",
                                      newest.error().ToString());
                        return 1;
                    }
                    // Trades at the newest time come back and are skipped.
                    since = newest->value_or(day);
                }
                auto trades = (*source)->GetTrades(symbol, since, day_end);
                if (!trades) {
                    spdlog::error("{} {}: {}", symbol,
                                  types::FormatIso8601(day),
                                  trades.error().ToString());
                    return 1;
                }
                fetched_total += trades->size();
                std::cout << symbol << ' ' << types::FormatIso8601(day)
                          << ": " << trades->size() << " trades";
                if (!trades->empty()) {
                    std::cout << ", " << trades->front().price << " .. "
                              << trades->back().price;
                }
                if (store) {
                    auto saved = store->Save(venue, symbol, *trades);
                    if (!saved) {
                        std::cout << '\n';
                        spdlog::error("database: {}", saved.error().ToString());
                        return 1;
                    }
                    stored_total += *saved;
                    std::cout << ", " << *saved << " new";
                }
                std::cout << '\n';
            }
        }
        std::cout << "fetched " << fetched_total << " trades";
        if (store) {
            std::cout << ", " << stored_total << " new to market_trades";
        }
        std::cout << '\n';
        return 0;
    }

   private:
    types::Timestamp mFrom;
    types::Timestamp mTo;
    bool mStore;
};

struct Args {
    std::string from;
    std::string to;
    bool store = false;
};

}  // namespace

void AddTradesCommand(CLI::App& app, CommandSlot& slot) {
    auto* sub = AddSubcommand(
        app, "trades",
        "Fetch the venue's public trades for the configured symbols over "
        "--from..--to, a day at a time (providers that keep a trade history: "
        "Binance.US)");
    auto args = std::make_shared<Args>();
    sub->add_option("--from", args->from,
                    "Start, YYYY-MM-DD or an ISO 8601 time (UTC)")
        ->required()
        ->check(DateValidator())
        ->type_name("DATE");
    sub->add_option("--to", args->to,
                    "End (exclusive), same format as --from; capped at now")
        ->required()
        ->check(DateValidator())
        ->type_name("DATE");
    sub->add_flag("--store", args->store,
                  "Save them to the [database] (market_trades), resuming each "
                  "day from its newest stored trade");
    sub->callback([&slot, args] {
        slot = std::make_unique<TradesCommand>(
            *types::ParseTimestamp(args->from),
            *types::ParseTimestamp(args->to), args->store);
    });
}

}  // namespace kestrel::app
