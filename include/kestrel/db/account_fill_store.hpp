#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "kestrel/db/database.hpp"
#include "kestrel/trading/trading_venue.hpp"
#include "kestrel/types/orders.hpp"

namespace kestrel::db {

// The account's own trade history, as the venue reports it, in the
// `account_fills` table keyed by (venue, fill_id): every fill on the account,
// whether a Kestrel run, another program or a person placed the order. Unlike
// `fills` (what one run saw), it is not tied to a run, so it is the record to
// work out a starting holding's cost basis from and to check runs against.
// Writing a fill that is already stored changes nothing.
class AccountFillStore {
   public:
    explicit AccountFillStore(std::shared_ptr<Database> database)
        : mDatabase(std::move(database)) {}

    // Returns how many of `fills` were new.
    types::Result<std::size_t> Save(const std::string& venue,
                                    std::span<const types::Fill> fills);

    // The newest fill time stored for `venue` and `symbol`; nullopt if none.
    types::Result<std::optional<types::Timestamp>> Newest(
        const std::string& venue, const types::Symbol& symbol) const;

    // from <= fill_time < to, oldest first; every symbol unless one is given.
    types::Result<std::vector<types::Fill>> Load(
        const std::string& venue, types::Timestamp from, types::Timestamp to,
        const std::optional<types::Symbol>& symbol = std::nullopt) const;

   private:
    std::shared_ptr<Database> mDatabase;
};

struct FillSync {
    types::Timestamp since;  // where the fetch started
    std::size_t fetched = 0;
    std::size_t stored = 0;  // of those, new to the table
};

// Where a sync of `venue`'s `symbols` has to start: the oldest of the
// symbols' newest stored fills (a fill at that time comes back again and is
// skipped as a duplicate), or `first_since` for a symbol with none stored yet.
types::Result<types::Timestamp> SyncStart(
    const AccountFillStore& store, const std::string& venue,
    const std::vector<types::Symbol>& symbols, types::Timestamp first_since);

// Brings `store` up to date with `venue`'s fills for `symbols`: GetFills from
// SyncStart, saved under venue.Name(). Run it again to top up; every run is
// incremental. A caller that fetches the fills anyway can fetch from
// SyncStart (or earlier) itself and Save them, instead of fetching twice.
types::Result<FillSync> SyncAccountFills(
    trading::TradingVenue& venue, AccountFillStore& store,
    const std::vector<types::Symbol>& symbols, types::Timestamp first_since);

}  // namespace kestrel::db
