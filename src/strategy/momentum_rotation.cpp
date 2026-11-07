#include "kestrel/strategy/momentum_rotation.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "kestrel/types/timeframe.hpp"

namespace kestrel::strategy {

using types::Decimal;
using types::OrderState;
using types::Rounding;
using types::Side;

namespace {

bool Closed(OrderState state) {
    return state == OrderState::kFilled || state == OrderState::kCancelled ||
           state == OrderState::kRejected || state == OrderState::kExpired;
}

std::string Percent(const Decimal& fraction) {
    const Decimal pct =
        (fraction * Decimal::FromInt(100)).Rescale(1, Rounding::kNearest);
    return (pct.Sign() > 0 ? "+" : "") + pct.ToString() + "%";
}

}  // namespace

MomentumRotation::MomentumRotation(MomentumRotationParams params)
    : mParams(std::move(params)) {
    if (mParams.symbols.size() < 2) {
        throw std::invalid_argument(
            "momentum_rotation needs two symbols or more");
    }
    for (const auto& symbol : mParams.symbols) {
        const auto parts = types::SplitSymbol(symbol);
        if (!parts || !parts->quote) {
            throw std::invalid_argument("symbol " + symbol +
                                        " has no quote asset");
        }
        if (!mBase.emplace(symbol, parts->base).second) {
            throw std::invalid_argument("symbol " + symbol +
                                        " is listed twice");
        }
        if (mQuote.empty()) {
            mQuote = *parts->quote;
        } else if (*parts->quote != mQuote) {
            throw std::invalid_argument(
                "all symbols must share one quote asset (" + mQuote + ", " +
                *parts->quote + ")");
        }
    }
    if (mParams.lookback_bars == 0 || mParams.rebalance_bars == 0 ||
        mParams.top_n == 0) {
        throw std::invalid_argument(
            "lookback_bars, rebalance_bars and top_n must be positive");
    }
    if (mParams.top_n > mParams.symbols.size()) {
        throw std::invalid_argument("top_n is more than the symbols");
    }
    if (!types::FixedDuration(mParams.timeframe)) {
        throw std::invalid_argument("timeframe must have a fixed length");
    }
    if (mParams.invest_fraction.Sign() <= 0 ||
        mParams.invest_fraction > Decimal::FromInt(1)) {
        throw std::invalid_argument("invest_fraction must be in (0, 1]");
    }
}

Requirements MomentumRotation::Needs() const {
    Requirements needs;
    needs.symbols = mParams.symbols;
    needs.bar_timeframes = {mParams.timeframe};
    // And one more, for a period ranked late on a bar of the next.
    needs.warmup_bars = mParams.lookback_bars + 2;
    return needs;
}

void MomentumRotation::OnBar(Context& context, const types::Symbol& symbol,
                             types::Timeframe timeframe,
                             const types::Bar& bar) {
    if (timeframe != mParams.timeframe ||
        std::ranges::find(mParams.symbols, symbol) == mParams.symbols.end()) {
        return;
    }
    if (!mPeriod || bar.open_time > *mPeriod) {
        // A new period: the last one, if a bar of it never came, is ranked
        // with what did.
        if (mPeriod && !mDone && !mSeen.empty()) {
            Period(context, *mPeriod);
        }
        mPeriod = bar.open_time;
        mSeen.clear();
        mDone = false;
    } else if (bar.open_time < *mPeriod || mDone) {
        return;  // a period already ranked
    }
    mSeen.insert(symbol);
    if (mSeen.size() == mParams.symbols.size()) {
        mDone = true;
        Period(context, *mPeriod);
    }
}

void MomentumRotation::Period(Context& context, types::Timestamp open_time) {
    mRanked = open_time;
    if (mPeriods++ % mParams.rebalance_bars != 0) {
        return;
    }
    if (!context.MarketOpen()) {
        mDue = true;  // at the next open
        return;
    }
    Rebalance(context);
}

void MomentumRotation::OnSessionOpen(Context& context) {
    if (std::exchange(mDue, false)) {
        Rebalance(context);
    }
}

void MomentumRotation::OnStart(Context& context) {
    // Only what this run buys counts as held: say so about coins the account
    // already holds, which it will neither sell nor count.
    auto balances = context.Venue().GetBalances();
    if (!balances) {
        return;
    }
    for (const auto& balance : *balances) {
        for (const auto& [symbol, base] : mBase) {
            if (balance.asset == base && balance.total.Sign() > 0) {
                spdlog::warn(
                    "momentum_rotation: the account holds {} {}, which this "
                    "run did not buy: it is not counted, and not sold",
                    balance.total.Normalized().ToString(), base);
            }
        }
    }
}

bool MomentumRotation::Reconcile(Context& context) {
    // An order whose closing update never came (a missed poll) would hold up
    // every rebalance: ask the venue.
    for (auto it = mOrders.begin(); it != mOrders.end();) {
        auto status = context.Venue().GetOrder(it->first);
        const bool gone = (status && Closed(status->state)) ||
                          (!status && status.error().category ==
                                          types::ErrorCategory::kNotFound);
        if (gone) {
            spdlog::warn("momentum_rotation: order {} had closed unnoticed",
                         it->first);
            if (it->second && status) {
                mSelling.erase(status->symbol);
            }
            it = mOrders.erase(it);
        } else {
            ++it;
        }
    }
    return mOrders.empty();
}

Decimal MomentumRotation::Sellable(Context& context,
                                   const types::Symbol& symbol) const {
    const auto it = mHeld.find(symbol);
    if (it == mHeld.end() || it->second.Sign() <= 0) {
        return Decimal();
    }
    auto info = context.SymbolInfo(symbol);
    if (!info) {
        return Decimal();
    }
    const Decimal quantity =
        it->second.RoundToIncrement(info->lot_size, Rounding::kDown);
    if (quantity < info->min_order_size ||
        (info->min_notional.Sign() > 0 && LastClose(context, symbol) &&
         quantity * *LastClose(context, symbol) < info->min_notional)) {
        return Decimal();  // dust: as good as not held
    }
    return quantity;
}

void MomentumRotation::Rebalance(Context& context) {
    if (!mOrders.empty() && !Reconcile(context)) {
        spdlog::warn(
            "momentum_rotation: {} order(s) from the last rebalance still "
            "out; skipping this one",
            mOrders.size());
        return;
    }
    // Momentum of each symbol with a bar for the ranked period and one
    // lookback_bars of time before it.
    const auto step = *types::FixedDuration(mParams.timeframe);
    const types::Timestamp from =
        mRanked - step * static_cast<std::int64_t>(mParams.lookback_bars);
    std::vector<std::pair<Decimal, types::Symbol>> ranked;
    for (const auto& symbol : mParams.symbols) {
        // The bar of the ranked period: the newest, or one before it when
        // the period is ranked late, on a bar of the next.
        const auto bars = context.Bars(symbol, mParams.timeframe);
        std::size_t at = bars.size();
        while (at > 0 && bars[at - 1].open_time > mRanked) {
            --at;
        }
        if (at == 0 || bars[at - 1].open_time != mRanked) {
            continue;  // no bar for it
        }
        // The bar lookback_bars before it, by time (a gap in the bars does
        // not lengthen the lookback).
        std::optional<std::size_t> then;
        for (std::size_t i = at - 1; i-- > 0;) {
            if (bars[i].open_time == from) {
                then = i;
                break;
            }
            if (bars[i].open_time < from) {
                break;
            }
        }
        if (!then || bars[*then].close.Sign() <= 0) {
            continue;
        }
        const Decimal momentum =
            Decimal::Divide(bars[at - 1].close, bars[*then].close, 8,
                            Rounding::kNearest) -
            Decimal::FromInt(1);
        ranked.emplace_back(momentum, symbol);
    }
    // A period ranked from a few bars (a backfill delivering one symbol's
    // bars ahead of the others) is no ranking to trade on.
    if (ranked.size() * 2 < mParams.symbols.size()) {
        spdlog::warn(
            "momentum_rotation: only {} of {} symbols could be ranked; no "
            "rebalance",
            ranked.size(), mParams.symbols.size());
        return;
    }
    std::ranges::sort(ranked, [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first > b.first : a.second < b.second;
    });
    std::set<types::Symbol> targets;
    std::set<types::Symbol> rankedSymbols;
    std::string top;
    for (const auto& [momentum, symbol] : ranked) {
        rankedSymbols.insert(symbol);
        if (targets.size() >= mParams.top_n ||
            (mParams.positive_only && momentum.Sign() <= 0)) {
            continue;
        }
        targets.insert(symbol);
        top += (top.empty() ? "" : ", ") + symbol + " " + Percent(momentum);
    }
    spdlog::info("momentum_rotation: {} ranked; top {}", ranked.size(),
                 top.empty() ? std::string("nothing (cash)") : top);

    // Sell what was ranked out of the top. What could not be ranked is kept
    // (and keeps its slot) until it can be.
    std::size_t kept = 0;
    for (const auto& symbol : mParams.symbols) {
        const Decimal quantity = Sellable(context, symbol);
        if (quantity.Sign() <= 0) {
            continue;
        }
        if (targets.contains(symbol) || !rankedSymbols.contains(symbol)) {
            ++kept;
            continue;
        }
        if (Place(context, symbol, Side::kSell, quantity)) {
            mSelling[symbol] = mHeld[symbol] - quantity;
        } else {
            ++kept;  // still held
        }
    }
    // Buy what entered it, into the slots left, once those sells are done.
    mEntries.clear();
    for (const auto& [momentum, symbol] : ranked) {
        if (kept + mEntries.size() >= mParams.top_n) {
            break;
        }
        if (targets.contains(symbol) && Sellable(context, symbol).IsZero()) {
            mEntries.push_back(symbol);
        }
    }
    TryEnter(context);
}

void MomentumRotation::TryEnter(Context& context) {
    if (mEntries.empty()) {
        return;
    }
    if (std::ranges::any_of(mOrders,
                            [](const auto& order) { return order.second; })) {
        return;  // a sell is still out
    }
    for (const auto& [symbol, after] : mSelling) {
        const auto it = mHeld.find(symbol);
        // Its fill not seen yet (equity would count it twice); dust left by
        // the sell is as good as sold.
        if (it != mHeld.end() && it->second > after &&
            Sellable(context, symbol).Sign() > 0) {
            return;
        }
    }
    mSelling.clear();
    Enter(context);
}

void MomentumRotation::Enter(Context& context) {
    const auto entries = std::exchange(mEntries, {});
    const auto equity = Equity(context);
    auto balances = context.Venue().GetBalances();
    if (!equity || !balances) {
        spdlog::warn("momentum_rotation: cannot value the account; no buys");
        return;
    }
    Decimal cash;
    for (const auto& balance : *balances) {
        if (balance.asset == mQuote) {
            cash = balance.available;
        }
    }
    const Decimal slot = Decimal::Divide(
        *equity * mParams.invest_fraction,
        Decimal::FromInt(static_cast<std::int64_t>(mParams.top_n)), 8,
        Rounding::kDown);
    for (const auto& symbol : entries) {
        const auto price = LastClose(context, symbol);
        auto info = context.SymbolInfo(symbol);
        if (!price || price->Sign() <= 0 || !info) {
            continue;
        }
        // Room for fees and for the price moving before the fill.
        const Decimal notional = std::min(slot, cash * mParams.invest_fraction);
        const Decimal quantity =
            Decimal::Divide(notional, *price, 8, Rounding::kDown)
                .RoundToIncrement(info->lot_size, Rounding::kDown);
        if (quantity.Sign() <= 0 || quantity < info->min_order_size ||
            quantity * *price < info->min_notional) {
            spdlog::info("momentum_rotation: {} {} of {} is too small to buy",
                         notional.Rescale(2, Rounding::kDown).ToString(),
                         mQuote, symbol);
            continue;
        }
        if (Place(context, symbol, Side::kBuy, quantity)) {
            cash = cash - quantity * *price;
        }
    }
}

std::optional<Decimal> MomentumRotation::LastClose(
    Context& context, const types::Symbol& symbol) const {
    const auto bars = context.Bars(symbol, mParams.timeframe);
    if (bars.empty()) {
        return std::nullopt;
    }
    return bars.back().close;
}

std::optional<Decimal> MomentumRotation::Equity(Context& context) const {
    auto balances = context.Venue().GetBalances();
    if (!balances) {
        return std::nullopt;
    }
    Decimal equity;
    for (const auto& balance : *balances) {
        if (balance.asset == mQuote) {
            equity = balance.total;
        }
    }
    for (const auto& [symbol, held] : mHeld) {
        if (held.Sign() > 0) {
            const auto price = LastClose(context, symbol);
            if (!price) {
                return std::nullopt;
            }
            equity += held * *price;
        }
    }
    return equity;
}

std::optional<std::string> MomentumRotation::Place(Context& context,
                                                   const types::Symbol& symbol,
                                                   Side side,
                                                   const Decimal& quantity) {
    types::OrderRequest request;
    request.symbol = symbol;
    request.side = side;
    request.type = types::OrderType::kMarket;
    request.quantity = quantity;
    auto ack = context.Venue().PlaceOrder(request);
    if (!ack) {
        spdlog::warn("momentum_rotation: {} {} {} failed: {}",
                     types::ToString(side), quantity.ToString(), symbol,
                     ack.error().ToString());
        return std::nullopt;
    }
    spdlog::info("momentum_rotation: {} {} {} (order {}, {})",
                 types::ToString(side), quantity.ToString(), symbol,
                 ack->order_id, types::ToString(ack->state));
    if (ack->state != OrderState::kFilled && Closed(ack->state)) {
        return std::nullopt;  // closed without a fill
    }
    if (!Closed(ack->state)) {
        mOrders[ack->order_id] = side == Side::kSell;
    }
    return ack->order_id;
}

void MomentumRotation::OnFill(Context& context, const types::Fill& fill) {
    const auto base = mBase.find(fill.symbol);
    if (base == mBase.end()) {
        return;
    }
    Decimal& held = mHeld[fill.symbol];
    if (fill.side == Side::kBuy) {
        held += fill.quantity;
    } else {
        held -= fill.quantity;
    }
    // A fee in the base asset (Binance.US takes a buy's fee in what was
    // bought) leaves less held than was filled; selling the full amount would
    // be refused for the balance.
    if (fill.fee_asset == base->second) {
        held -= fill.fee;
    }
    held = std::max(held, Decimal());
    if (fill.side == Side::kSell) {
        TryEnter(context);
    }
}

void MomentumRotation::OnOrderUpdate(Context& context,
                                     const types::OrderStatus& status) {
    if (!Closed(status.state)) {
        return;
    }
    const auto it = mOrders.find(status.order_id);
    if (it == mOrders.end()) {
        return;
    }
    const bool sell = it->second;
    mOrders.erase(it);
    if (status.state != OrderState::kFilled) {
        spdlog::warn("momentum_rotation: order {} for {} closed: {}",
                     status.order_id, status.symbol,
                     types::ToString(status.state));
        if (sell) {
            mSelling.erase(status.symbol);  // no more of it will sell
        }
    }
    if (sell) {
        TryEnter(context);  // once the sells are done and seen: the cash is in
    }
}

}  // namespace kestrel::strategy
