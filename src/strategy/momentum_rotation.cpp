#include "kestrel/strategy/momentum_rotation.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "kestrel/types/timeframe.hpp"

namespace kestrel::strategy {

using types::Decimal;
using types::Rounding;
using types::Side;

namespace {

std::string Percent(const Decimal& fraction) {
    const Decimal pct =
        (fraction * Decimal::FromInt(100)).Rescale(1, Rounding::kNearest);
    return (pct.Sign() > 0 ? "+" : "") + pct.ToString() + "%";
}

// The parameters, checked before the book is built from them.
const MomentumRotationParams& Checked(const MomentumRotationParams& params) {
    if (params.symbols.size() < 2) {
        throw std::invalid_argument(
            "momentum_rotation needs two symbols or more");
    }
    return params;
}

}  // namespace

MomentumRotation::MomentumRotation(MomentumRotationParams params)
    : mParams(std::move(params)),
      mBook(Checked(mParams).symbols, mParams.timeframe),
      mGate(mParams.symbols.size()) {
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
    for (const auto period : mGate.Add(symbol, bar.open_time)) {
        Period(context, period);
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
    // The coins of the universe the account already holds: with
    // adopt_holdings, counted as held (a restart picks up what the last run
    // bought, and sells it if it is ranked out); else neither counted nor
    // sold, and said so.
    mBook.AdoptHoldings(context, mParams.adopt_holdings);
}

void MomentumRotation::Rebalance(Context& context) {
    if (mBook.OrdersOut()) {
        for (const auto& closed : mBook.Reconcile(context)) {
            if (closed.sell && !closed.symbol.empty()) {
                mSelling.erase(closed.symbol);
            }
        }
        if (mBook.OrdersOut()) {
            context.Log().Warn(
                "{} order(s) from the last rebalance still out; skipping this "
                "one",
                mBook.OrderCount());
            return;  // its buys wait on its sells
        }
    }
    // Buys queued by an earlier rebalance are superseded by this one, even
    // when it ends without trading.
    mEntries.clear();
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
        context.Log().Warn(
            "only {} of {} symbols could be ranked; no rebalance",
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
    context.Log().Info("{} ranked; top {}", ranked.size(),
                       top.empty() ? std::string("nothing (cash)") : top);

    // Sell what was ranked out of the top. What could not be ranked is kept
    // (and keeps its slot) until it can be.
    std::size_t kept = 0;
    for (const auto& symbol : mParams.symbols) {
        const Decimal quantity = mBook.Sellable(context, symbol);
        if (quantity.Sign() <= 0) {
            continue;
        }
        if (targets.contains(symbol) || !rankedSymbols.contains(symbol)) {
            ++kept;
            continue;
        }
        if (mBook.Place(context, symbol, Side::kSell, quantity)) {
            mSelling[symbol] = mBook.Held(symbol) - quantity;
        } else {
            ++kept;  // still held
        }
    }
    // Buy what entered it, into the slots left, once those sells are done.
    for (const auto& [momentum, symbol] : ranked) {
        if (kept + mEntries.size() >= mParams.top_n) {
            break;
        }
        if (targets.contains(symbol) &&
            mBook.Sellable(context, symbol).IsZero()) {
            mEntries.push_back(symbol);
        }
    }
    TryEnter(context);
}

void MomentumRotation::TryEnter(Context& context) {
    if (mEntries.empty() || mBook.SellOut()) {
        return;  // nothing to buy, or a sell is still out
    }
    for (const auto& [symbol, after] : mSelling) {
        // Its fill not seen yet (equity would count it twice); dust left by
        // the sell is as good as sold.
        if (mBook.Held(symbol) > after &&
            mBook.Sellable(context, symbol).Sign() > 0) {
            return;
        }
    }
    mSelling.clear();
    Enter(context);
}

void MomentumRotation::Enter(Context& context) {
    const auto entries = std::exchange(mEntries, {});
    const auto equity = mBook.Equity(context);
    auto cash = mBook.FreeCash(context);
    if (!equity || !cash) {
        context.Log().Warn("cannot value the account; no buys");
        return;
    }
    const Decimal slot = Decimal::Divide(
        *equity * mParams.invest_fraction,
        Decimal::FromInt(static_cast<std::int64_t>(mParams.top_n)), 8,
        Rounding::kDown);
    for (const auto& symbol : entries) {
        const auto price = mBook.LastClose(context, symbol);
        auto info = context.SymbolInfo(symbol);
        if (!price || price->Sign() <= 0 || !info) {
            continue;
        }
        // Room for fees and for the price moving before the fill.
        const Decimal notional =
            std::min(slot, *cash * mParams.invest_fraction);
        const Decimal quantity =
            Decimal::Divide(notional, *price, 8, Rounding::kDown)
                .RoundToIncrement(info->lot_size, Rounding::kDown);
        if (quantity.Sign() <= 0 || quantity < info->min_order_size ||
            quantity * *price < info->min_notional) {
            context.Log().Info("{} {} of {} is too small to buy",
                               notional.Rescale(2, Rounding::kDown).ToString(),
                               mBook.Quote(), symbol);
            continue;
        }
        if (mBook.Place(context, symbol, Side::kBuy, quantity)) {
            *cash = *cash - quantity * *price;
        }
    }
}

void MomentumRotation::OnFill(Context& context, const types::Fill& fill) {
    mBook.OnFill(fill);
    if (fill.side == Side::kSell) {
        TryEnter(context);
    }
}

void MomentumRotation::OnOrderUpdate(Context& context,
                                     const types::OrderStatus& status) {
    const auto done = mBook.OnOrderUpdate(status);
    if (!done) {
        return;
    }
    if (!done->filled) {
        context.Log().Warn("order {} for {} closed: {}", status.order_id,
                           done->symbol, types::ToString(status.state));
        if (done->sell) {
            mSelling.erase(done->symbol);  // no more of it will sell
        }
    }
    if (done->sell) {
        TryEnter(context);  // once the sells are done and seen: the cash is in
    }
}

}  // namespace kestrel::strategy
