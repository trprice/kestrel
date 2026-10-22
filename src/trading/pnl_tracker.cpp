#include "kestrel/trading/pnl_tracker.hpp"

#include <algorithm>

namespace kestrel::trading {

using types::Decimal;
using types::Rounding;
using types::Side;

namespace {

constexpr int kScale = 8;

Decimal Round(const Decimal& d) {
    return d.Rescale(kScale, Rounding::kNearest);
}

// The fee in the quote asset.
Decimal FeeInQuote(const types::Fill& fill) {
    auto parts = types::SplitSymbol(fill.symbol);
    if (parts && !fill.fee_asset.empty() && fill.fee_asset == parts->base) {
        return fill.fee * fill.price;
    }
    return fill.fee;
}

}  // namespace

void PnlTracker::Reset(const std::vector<types::Position>& positions) {
    mHoldings.clear();
    for (const auto& position : positions) {
        mHoldings[position.symbol] =
            Holding{position.quantity, position.average_price, Decimal()};
    }
}

void PnlTracker::ResetRealized() {
    for (auto& [symbol, holding] : mHoldings) {
        holding.realized = Decimal();
    }
}

void PnlTracker::Apply(const types::Fill& fill) {
    Holding& holding = mHoldings[fill.symbol];
    const Decimal fee = FeeInQuote(fill);

    if (fill.side == Side::kBuy) {
        const Decimal quantity = holding.quantity + fill.quantity;
        holding.average_price =
            Decimal::Divide(holding.average_price * holding.quantity +
                                fill.price * fill.quantity,
                            quantity, kScale, Rounding::kNearest);
        holding.quantity = quantity;
        holding.realized = Round(holding.realized - fee);
    } else {
        const Decimal tracked = std::min(fill.quantity, holding.quantity);
        holding.realized =
            Round(holding.realized +
                  (fill.price - holding.average_price) * tracked - fee);
        holding.quantity -= tracked;
        if (holding.quantity.IsZero()) {
            holding.average_price = Decimal();
        }
    }
}

Decimal PnlTracker::Realized() const {
    Decimal sum;
    for (const auto& [symbol, holding] : mHoldings) {
        sum += holding.realized;
    }
    return sum;
}

Decimal PnlTracker::Realized(const types::Symbol& symbol) const {
    auto it = mHoldings.find(symbol);
    return it == mHoldings.end() ? Decimal() : it->second.realized;
}

std::vector<types::Position> PnlTracker::Positions() const {
    std::vector<types::Position> out;
    for (const auto& [symbol, holding] : mHoldings) {
        out.push_back({symbol, holding.quantity, holding.average_price,
                       holding.realized});
    }
    return out;
}

Decimal PnlTracker::Held(const types::Symbol& symbol) const {
    auto it = mHoldings.find(symbol);
    return it == mHoldings.end() ? Decimal() : it->second.quantity;
}

}  // namespace kestrel::trading
