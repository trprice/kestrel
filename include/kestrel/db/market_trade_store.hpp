#pragma once

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "kestrel/db/database.hpp"
#include "kestrel/types/market.hpp"

namespace kestrel::db {

// Public trades on a venue (everyone's, from DataSource::GetTrades), in the
// `market_trades` hypertable keyed by (venue, symbol, trade_id). A backtest
// can build approximate quotes from them where no quotes were recorded (see
// backtest::QuotesFromTrades). Writing a trade that is already stored changes
// nothing, so an interrupted backfill can simply be run again.
class MarketTradeStore {
   public:
    explicit MarketTradeStore(std::shared_ptr<Database> database)
        : mDatabase(std::move(database)) {}

    // Returns how many of `trades` were new.
    types::Result<std::size_t> Save(const std::string& venue,
                                    const types::Symbol& symbol,
                                    std::span<const types::MarketTrade> trades);

    // The newest trade time stored for `venue` and `symbol` within
    // [from, to); nullopt if there is none.
    types::Result<std::optional<types::Timestamp>> Newest(
        const std::string& venue, const types::Symbol& symbol,
        types::Timestamp from, types::Timestamp to) const;

    // from <= trade_time < to, oldest first (by time, then trade ID).
    types::Result<std::vector<types::MarketTrade>> Load(
        const std::string& venue, const types::Symbol& symbol,
        types::Timestamp from, types::Timestamp to) const;

   private:
    std::shared_ptr<Database> mDatabase;
};

}  // namespace kestrel::db
