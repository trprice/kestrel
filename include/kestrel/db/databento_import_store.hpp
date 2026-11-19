#pragma once

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "kestrel/db/database.hpp"
#include "kestrel/types/symbol.hpp"
#include "kestrel/types/time.hpp"
#include "kestrel/types/timeframe.hpp"

namespace kestrel::db {

// Ledger of finished Databento HTTP slices in `databento_imports`, written only
// after every record in a slice has been saved to its target store. Using a
// ledger instead of guessing from rows in `bars` / `quotes` / `market_trades`
// keeps thinly traded sessions (including empty ones) from being downloaded
// again on every resume, while ensuring a slice interrupted mid-save is
// re-fetched.
class DatabentoImportStore {
   public:
    using Interval = std::pair<types::Timestamp, types::Timestamp>;

    explicit DatabentoImportStore(std::shared_ptr<Database> database)
        : mDatabase(std::move(database)) {}

    // Returns sorted, coalesced `[start, end)` intervals recorded for
    // `(venue, dataset, symbol, target)` that overlap `[from, to)`.
    types::Result<std::vector<Interval>> Load(const std::string& venue,
                                              const std::string& dataset,
                                              const types::Symbol& symbol,
                                              const std::string& target,
                                              types::Timestamp from,
                                              types::Timestamp to) const;

    // The open time of the first stored bar of `(venue, symbol, timeframe)`
    // whose New York minute of the day is not `anchor_minutes` past midnight
    // plus a whole number of `length_minutes`, or nullopt without one. Bars
    // of one timeframe laid out from another anchor overlap these (native
    // `ohlcv-1h` bars start on the hour, rollups at the session's open), and
    // a backtest would replay both.
    types::Result<std::optional<types::Timestamp>> FindOffGridBar(
        const std::string& venue, const types::Symbol& symbol,
        types::Timeframe timeframe, int anchor_minutes,
        int length_minutes) const;

    // Records every `(target, slice)` pair as completed. Idempotent.
    types::Result<void> Record(const std::string& venue,
                               const std::string& dataset,
                               const types::Symbol& symbol,
                               std::span<const std::string> targets,
                               std::span<const Interval> slices);

   private:
    std::shared_ptr<Database> mDatabase;
};

}  // namespace kestrel::db
