#include "kestrel/trading/pnl_tracker.hpp"

#include <spdlog/spdlog.h>

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
    if (const auto parts = types::SplitSymbol(fill.symbol);
        parts && parts->quote && !fill.fee_asset.empty() &&
        fill.fee_asset != parts->base && fill.fee_asset != *parts->quote &&
        mOtherFeeAssets.insert(fill.fee_asset).second) {
        // Valued 1:1 with the quote asset, which is wrong (a BNB fee on
        // Binance.US): the P&L and the risk gate's loss count are off by it.
        spdlog::warn(
            "pnl: fees in {} (on {}) are counted as if in {}; P&L is off by "
            "the difference",
            fill.fee_asset, fill.symbol, *parts->quote);
    }
    Holding& holding = mHoldings[fill.symbol];
    const Decimal fee = FeeInQuote(fill);

    // Signed: what the fill adds to the position, and what of the position it
    // closes first (a buy closes a short, a sell a long).
    const bool buy = fill.side == Side::kBuy;
    const Decimal open = buy ? -holding.quantity : holding.quantity;
    Decimal closed = std::min(fill.quantity, std::max(open, Decimal()));
    Decimal opened = fill.quantity - closed;
    if (!buy && !mShorts && holding.quantity.Sign() >= 0) {
        opened = Decimal();  // spot: an excess sale has no cost basis
    }
    Decimal gained = -fee;
    if (closed.Sign() > 0) {
        gained += (buy ? holding.average_price - fill.price
                       : fill.price - holding.average_price) *
                  closed;
        holding.quantity += buy ? closed : -closed;
        if (holding.quantity.IsZero()) {
            holding.average_price = Decimal();
        }
    }
    holding.realized = Round(holding.realized + gained);
    if (opened.Sign() > 0) {
        const Decimal size = holding.quantity.Abs();
        holding.average_price =
            Decimal::Divide(holding.average_price * size + fill.price * opened,
                            size + opened, kScale, Rounding::kNearest);
        holding.quantity += buy ? opened : -opened;
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
    out.reserve(mHoldings.size());
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
