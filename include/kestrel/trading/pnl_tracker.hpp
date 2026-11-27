#pragma once

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "kestrel/types/decimal.hpp"
#include "kestrel/types/orders.hpp"
#include "kestrel/types/symbol.hpp"

namespace kestrel::trading {

// Realized profit and loss worked out from fills alone, so it does not depend
// on what a venue reports in Position::realized_pnl (some venues drop a
// position, and its P&L, once it is closed).
//
// This is the one place P&L is calculated: PaperTradingVenue reports its
// positions from it and RiskGatedVenue measures its daily loss with it. The
// bookkeeping:
//   - average-cost basis: a buy raises the average price of what is held, a
//     sell realizes (price - average) on the quantity sold and leaves the
//     average unchanged;
//   - fees are realized when they are paid, buys included;
//   - a sell of more than is held realizes nothing on the excess, which has no
//     known cost basis.
// With shorts, the excess is sold short instead: the position goes negative at
// the sale's price (averaged with any short already open), a buy covers it,
// realizing (average - price) on the quantity covered, and a buy of more than
// is short goes long with the rest.
// Fees are valued in the quote asset: a fee in the symbol's base asset is
// converted at the fill price and taken off the coin held (on a buy, off what
// was bought; on a sell, off what remains, or added to the short), and any
// other fee asset is taken as quote (logged the first time it is seen).
// Amounts are kept to 8 decimal places. Not thread-safe.
class PnlTracker {
   public:
    PnlTracker() = default;
    explicit PnlTracker(bool shorts) : mShorts(shorts) {}

    // Forgets everything and takes `positions` as the starting holdings, each
    // at its average price. Realized P&L restarts at zero.
    void Reset(const std::vector<types::Position>& positions);

    // Zeroes the realized P&L but keeps the cost basis, for a new day.
    void ResetRealized();

    // Applies a fill; each fill must be applied once.
    void Apply(const types::Fill& fill);

    // Summed over all symbols, so they should share a quote asset.
    types::Decimal Realized() const;
    types::Decimal Realized(const types::Symbol& symbol) const;
    types::Decimal Held(const types::Symbol& symbol) const;

    // Every symbol seen, flat ones included, as quantity held (negative when
    // short), average price of that quantity (zero when flat) and realized
    // P&L.
    std::vector<types::Position> Positions() const;

   private:
    struct Holding {
        types::Decimal quantity;
        types::Decimal average_price;
        types::Decimal realized;
    };
    std::map<types::Symbol, Holding> mHoldings;
    bool mShorts = false;
    std::set<std::string> mOtherFeeAssets;  // logged once each
};

// Works out a live spot account's starting holdings and average-cost basis for
// `symbols` from the venue's stored `fills` (replayed oldest first by
// (time, length(fill_id), fill_id) through PnlTracker, with historical realized
// P&L zeroed) reconciled against the account's `balances` by base asset. Used
// for the recorder's P&L only; the risk gate never counts these holdings.
//   - A fill quoted in one of the symbols' base assets (a ZEC/BTC buy spends
//     BTC) takes the coins spent off that asset's replayed holdings at their
//     average cost; coins received that way, or bought on pairs outside
//     `symbols` (whose prices are in another quote currency), have no cost
//     basis in `symbols`' quote currency and are reconciled as excess below.
//   - wallet == replayed across `symbols`: keeps each symbol's replayed
//     quantity and average price;
//   - wallet > replayed across `symbols` (a transfer in, buys before the first
//     sync, or coins from pairs outside `symbols`): values the excess at
//     `reference_price(symbol)`, blends it into the symbol's quantity and
//     average price, and logs a warning (if no symbol for that base asset has
//     a positive reference price, the unpriced excess is left out of the
//     starting positions with a warning, so selling it realizes nothing rather
//     than a phantom gain);
//   - wallet < replayed across `symbols` (a withdrawal, or an unsynced sale):
//     clamps `symbols` in order down to the wallet balance while keeping each
//     symbol's average price (zero when clamped to zero), and logs a warning.
// A difference worth less than 1 in the quote currency (exchange dust) is
// logged at info instead of as a warning; the values are the same.
// Pairs in `symbols` that share a base asset (e.g. ZEC/USD and ZEC/USDT) share
// one wallet balance, so it is never counted twice or warned about when their
// replayed holdings add up to it.
std::vector<types::Position> ReconcileStartingPositions(
    std::span<const types::Symbol> symbols, std::span<const types::Fill> fills,
    std::span<const types::Balance> balances,
    const std::function<std::optional<types::Decimal>(const types::Symbol&)>&
        reference_price = {});

}  // namespace kestrel::trading
