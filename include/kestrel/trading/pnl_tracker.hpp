#pragma once

#include <map>
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
// Fees are valued in the quote asset: a fee in the symbol's base asset is
// converted at the fill price, and any other fee asset is taken as quote.
// Amounts are kept to 8 decimal places. Not thread-safe.
class PnlTracker {
   public:
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

    // Every symbol seen, flat ones included, as quantity held, average price
    // of that quantity (zero when flat) and realized P&L.
    std::vector<types::Position> Positions() const;

   private:
    struct Holding {
        types::Decimal quantity;
        types::Decimal average_price;
        types::Decimal realized;
    };
    std::map<types::Symbol, Holding> mHoldings;
};

}  // namespace kestrel::trading
