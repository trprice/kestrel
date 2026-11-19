#include "kestrel/strategy/trend_ensemble.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>

#include "kestrel/strategy/orders.hpp"
#include "kestrel/strategy/patterns.hpp"
#include "kestrel/types/timeframe.hpp"

namespace kestrel::strategy {

using types::Decimal;
using types::Rounding;
using types::Side;

namespace {

Decimal Count(std::size_t n) {
    return Decimal::FromInt(static_cast<std::int64_t>(n));
}

std::string Percent(const Decimal& fraction) {
    return (fraction * Decimal::FromInt(100))
               .Rescale(1, Rounding::kNearest)
               .ToString() +
           "%";
}

// Bars further apart than this many periods, and than kMaxGap, are a halt
// or a relisting (Binance.US had no XRP/USDT from Jan 2021 to Jul 2023):
// the models start again after it and the volatility looks only past it,
// rather than joining prices years apart as if a bar apart. A missed bar or
// two is not one, nor, on hourly bars, an outage of a few hours.
constexpr std::int64_t kMaxGapBars = 7;
constexpr std::chrono::seconds kMaxGap = std::chrono::days(7);

// The parameters, checked before the book is built from them.
const TrendEnsembleParams& Checked(const TrendEnsembleParams& params) {
    if (params.symbols.empty()) {
        throw std::invalid_argument("trend_ensemble needs a symbol or more");
    }
    return params;
}

}  // namespace

TrendEnsemble::TrendEnsemble(TrendEnsembleParams params)
    : mParams(std::move(params)),
      mBook(Checked(mParams).symbols, mParams.timeframe),
      mGate(mParams.symbols.size()),
      mPeriodsPerYear(0) {
    const auto step = types::FixedDuration(mParams.timeframe);
    if (!step) {
        throw std::invalid_argument("timeframe must have a fixed length");
    }
    mStep = *step;
    // 365 a year on daily bars: crypto trades every day.
    mPeriodsPerYear = static_cast<std::size_t>(
        std::chrono::seconds(std::chrono::days(365)) / *step);
    if (mPeriodsPerYear == 0) {
        throw std::invalid_argument("timeframe must be under a year");
    }
    // A cap well past any channel worth running, so the warm-up cannot
    // overflow.
    constexpr std::size_t kMaxLookback = 10000;
    if (mParams.lookbacks.empty() ||
        std::ranges::any_of(mParams.lookbacks, [](std::size_t n) {
            return n < 2 || n > kMaxLookback;
        })) {
        throw std::invalid_argument(
            "lookbacks must be one or more lengths of 2 to 10000");
    }
    auto sorted = mParams.lookbacks;
    std::ranges::sort(sorted);
    if (std::ranges::adjacent_find(sorted) != sorted.end()) {
        // A repeat would silently count one model twice.
        throw std::invalid_argument("lookbacks must not repeat");
    }
    if (mParams.vol_bars < 2 || mParams.vol_bars > kMaxLookback) {
        throw std::invalid_argument("vol_bars must be 2 to 10000");
    }
    if (mParams.target_vol.Sign() <= 0) {
        throw std::invalid_argument("target_vol must be positive");
    }
    if (mParams.max_weight.Sign() <= 0 ||
        mParams.max_weight > Decimal::FromInt(1)) {
        throw std::invalid_argument(
            "max_weight must be in (0, 1]: spot trading has no leverage");
    }
    if (mParams.rebalance_threshold.IsNegative()) {
        throw std::invalid_argument("rebalance_threshold must not be negative");
    }
    if (mParams.invest_fraction.Sign() <= 0 ||
        mParams.invest_fraction > Decimal::FromInt(1)) {
        throw std::invalid_argument("invest_fraction must be in (0, 1]");
    }
    for (const auto& symbol : mParams.symbols) {
        mState[symbol].models.resize(mParams.lookbacks.size());
    }
}

Requirements TrendEnsemble::Needs() const {
    Requirements needs;
    needs.symbols = mParams.symbols;
    needs.bar_timeframes = {mParams.timeframe};
    // Three times the longest channel. A model's trailing stop depends on
    // when it went long, and a model of n bars starts n - 1 bars into the
    // replay at startup, so each replays at least twice its own length and
    // agrees with a run that never stopped unless it has been long longer.
    const std::size_t longest = std::ranges::max(mParams.lookbacks);
    needs.warmup_bars = std::max(3 * longest, mParams.vol_bars + 1) + 1;
    return needs;
}

void TrendEnsemble::OnStart(Context& context) {
    mBook.AdoptHoldings(context, mParams.adopt_holdings);
    for (const auto& symbol : mParams.symbols) {
        Advance(context, symbol);
    }
}

std::chrono::seconds TrendEnsemble::MaxGap() const {
    return std::max(mStep * kMaxGapBars, kMaxGap);
}

std::size_t TrendEnsemble::SegmentStart(std::span<const types::Bar> bars,
                                        std::size_t upto) const {
    if (bars.empty()) {
        return 0;
    }
    for (std::size_t k = std::min(upto, bars.size() - 1); k > 0; --k) {
        if (bars[k].open_time - bars[k - 1].open_time > MaxGap()) {
            return k;
        }
    }
    return 0;
}

void TrendEnsemble::Advance(Context& context, const types::Symbol& symbol) {
    auto& state = mState.at(symbol);
    const auto bars = context.Bars(symbol, mParams.timeframe);
    if (bars.empty()) {
        return;
    }
    std::size_t from = 0;
    if (state.last) {
        while (from < bars.size() && bars[from].open_time <= *state.last) {
            ++from;
        }
    }
    std::size_t start = from < bars.size() ? SegmentStart(bars, from) : 0;
    for (std::size_t k = from; k < bars.size(); ++k) {
        if (k > 0 && bars[k].open_time - bars[k - 1].open_time > MaxGap()) {
            // A halt or a relisting: every model starts again from here.
            if (state.last) {
                context.Log().Info(
                    "{}: no bars from {} to {}; its models start again", symbol,
                    types::FormatIso8601(bars[k - 1].open_time),
                    types::FormatIso8601(bars[k].open_time));
            }
            for (Model& model : state.models) {
                model = Model{};
            }
            start = k;
        }
        const Decimal& close = bars[k].close;
        for (std::size_t m = 0; m < mParams.lookbacks.size(); ++m) {
            const std::size_t n = mParams.lookbacks[m];
            if (k + 1 - start < n) {
                continue;  // not n closes since the start or the last gap
            }
            Decimal high = close;
            Decimal low = close;
            for (std::size_t i = k + 1 - n; i < k; ++i) {
                high = std::max(high, bars[i].close);
                low = std::min(low, bars[i].close);
            }
            // Exact: half of a sum needs one place more than its terms.
            const Decimal mid = Decimal::Divide(
                high + low, Decimal::FromInt(2),
                std::min(std::max(high.Scale(), low.Scale()) + 1,
                         Decimal::kMaxScale),
                Rounding::kNearest);
            Model& model = state.models[m];
            if (close >= high) {
                // The highest close of the channel: long, or still long.
                model.stop = model.long_ ? std::max(model.stop, mid) : mid;
                model.long_ = true;
            } else if (model.long_ && close <= model.stop) {
                model.long_ = false;  // the trailing stop
            } else if (model.long_) {
                model.stop = std::max(model.stop, mid);
            }
        }
        state.last = bars[k].open_time;
    }
}

std::size_t TrendEnsemble::LongModels(const types::Symbol& symbol) const {
    const auto& models = mState.at(symbol).models;
    return static_cast<std::size_t>(std::ranges::count_if(
        models, [](const Model& model) { return model.long_; }));
}

Decimal TrendEnsemble::Conviction(const types::Symbol& symbol) const {
    return Decimal::Divide(Count(LongModels(symbol)),
                           Count(mParams.lookbacks.size()), 8,
                           Rounding::kNearest);
}

std::optional<Decimal> TrendEnsemble::Weight(
    Context& context, const types::Symbol& symbol) const {
    const std::size_t longs = LongModels(symbol);
    if (longs == 0) {
        return Decimal();
    }
    // Over the bars since the last gap only.
    const auto bars = context.Bars(symbol, mParams.timeframe);
    const auto vol =
        Volatility(bars.subspan(SegmentStart(bars, bars.size() - 1)),
                   mParams.vol_bars, mPeriodsPerYear);
    if (!vol) {
        return std::nullopt;
    }
    // Each model's weight is min(target_vol / volatility, max_weight); the
    // symbol's is their average, of its equal share of what may be invested.
    const Decimal per_model =
        vol->Sign() > 0 ? std::min(Decimal::Divide(mParams.target_vol, *vol, 8,
                                                   Rounding::kDown),
                                   mParams.max_weight)
                        : mParams.max_weight;
    const Decimal share =
        Decimal::Divide(mParams.invest_fraction, Count(mParams.symbols.size()),
                        8, Rounding::kDown);
    return Decimal::Divide(per_model * Count(longs) * share,
                           Count(mParams.lookbacks.size()), 8, Rounding::kDown);
}

void TrendEnsemble::OnBar(Context& context, const types::Symbol& symbol,
                          types::Timeframe timeframe, const types::Bar& bar) {
    if (timeframe != mParams.timeframe || !mState.contains(symbol)) {
        return;
    }
    Advance(context, symbol);
    for (const auto period : mGate.Add(symbol, bar.open_time)) {
        if (!context.MarketOpen()) {
            mDue = period;  // at the next open
            continue;
        }
        Rebalance(context, period);
    }
}

void TrendEnsemble::OnSessionOpen(Context& context) {
    if (const auto due = std::exchange(mDue, std::nullopt)) {
        Rebalance(context, *due);
    }
}

void TrendEnsemble::Rebalance(Context& context, types::Timestamp period) {
    // Buys queued by an earlier rebalance are superseded by this one, whether
    // it trades or is put off.
    mEntries.clear();
    mPending.reset();
    if (mBook.OrdersOut()) {
        for (const auto& closed : mBook.Reconcile(context)) {
            if (closed.sell && !closed.symbol.empty()) {
                mSelling.erase(closed.symbol);
            }
        }
        if (mBook.OrdersOut()) {
            // Two periods can complete together (a symbol's bar missing from
            // the first); this one runs once those orders have closed.
            context.Log().Info(
                "{} order(s) from the last rebalance still out; rebalancing "
                "once they close",
                mBook.OrderCount());
            mPending = period;
            return;
        }
    }
    const auto equity = mBook.Equity(context);
    if (!equity) {
        context.Log().Warn("cannot value the account; no rebalance");
        return;
    }
    std::string summary;
    Decimal invested;
    for (const auto& symbol : mParams.symbols) {
        const auto bars = context.Bars(symbol, mParams.timeframe);
        if (bars.empty() || bars.back().open_time != period) {
            continue;  // no bar for the period: left as it is
        }
        auto& state = mState.at(symbol);
        const std::size_t longs = LongModels(symbol);
        const auto weight = Weight(context, symbol);
        if (!weight) {
            continue;  // too new for a volatility: left as it is
        }
        const Decimal price = bars.back().close;
        // A weight rounded to zero with models long (a volatility thousands
        // of times target_vol) is as good as none: sold like one.
        const Decimal target = *equity * *weight;
        const Decimal value = mBook.Held(symbol) * price;
        if (longs > 0) {
            summary += (summary.empty() ? "" : ", ") + symbol + " " +
                       std::to_string(longs) + "/" +
                       std::to_string(mParams.lookbacks.size()) + " " +
                       Percent(*weight);
            invested += *weight;
        }
        // A change of signal trades; a drift in size only past the threshold.
        const bool signal = state.traded_long != longs;
        const Decimal gap = (target - value).Abs();
        const bool near = gap <= target * mParams.rebalance_threshold;
        if (!signal && near) {
            continue;
        }
        // The signal counts as traded once an order for it is placed; one too
        // small (or refused) is tried again at the next rebalance.
        if (target == value) {
            state.traded_long = longs;
        } else if (target < value) {
            const Decimal sellable = mBook.Sellable(context, symbol);
            Decimal quantity =
                target.IsZero()
                    ? sellable
                    : std::min(
                          sellable,
                          RoundDownToLot(context, symbol,
                                         Decimal::Divide(value - target, price,
                                                         8, Rounding::kDown)));
            auto info = context.SymbolInfo(symbol);
            if (quantity.Sign() <= 0 || !info ||
                quantity < info->min_order_size ||
                quantity * price < info->min_notional) {
                // Too small to sell. Within the threshold (an adopted coin
                // already near its target, say) that is as good as traded.
                if (near) {
                    state.traded_long = longs;
                }
                continue;
            }
            if (mBook.Place(context, symbol, Side::kSell, quantity)) {
                mSelling[symbol] = mBook.Held(symbol) - quantity;
                state.traded_long = longs;
            }
        } else {
            mEntries[symbol] = Entry{target, longs, near};
        }
    }
    context.Log().Info("{}; target {} invested",
                       summary.empty() ? std::string("no trends (cash)")
                                       : "in a trend: " + summary,
                       Percent(invested));
    TryEnter(context);
}

void TrendEnsemble::TryEnter(Context& context) {
    if (mEntries.empty() || mBook.SellOut()) {
        return;  // nothing to buy, or a sell is still out
    }
    for (const auto& [symbol, after] : mSelling) {
        // Its fill not seen yet (the cash is not in); dust left by the sell
        // is as good as sold.
        if (mBook.Held(symbol) > after &&
            mBook.Sellable(context, symbol).Sign() > 0) {
            return;
        }
    }
    mSelling.clear();
    Enter(context);
}

void TrendEnsemble::Enter(Context& context) {
    const auto entries = std::exchange(mEntries, {});
    auto cash = mBook.FreeCash(context);
    if (!cash) {
        context.Log().Warn("cannot read the cash balance; no buys");
        return;
    }
    for (const auto& [symbol, entry] : entries) {
        const Decimal& target = entry.target;
        const auto price = mBook.LastClose(context, symbol);
        auto info = context.SymbolInfo(symbol);
        if (!price || price->Sign() <= 0 || !info) {
            continue;
        }
        // What the target lacks, within the cash, less 1% for fees and the
        // price moving before the fill.
        const Decimal want = target - mBook.Held(symbol) * *price;
        const Decimal notional = std::min(want, *cash * Decimal(99, 2));
        const Decimal quantity = RoundDownToLot(
            context, symbol,
            Decimal::Divide(notional, *price, 8, Rounding::kDown));
        if (quantity.Sign() <= 0 || quantity < info->min_order_size ||
            quantity * *price < info->min_notional) {
            // Too small to buy: as good as traded within the threshold.
            if (entry.near) {
                mState.at(symbol).traded_long = entry.longs;
            }
            continue;
        }
        if (mBook.Place(context, symbol, Side::kBuy, quantity)) {
            *cash -= quantity * *price;
            mState.at(symbol).traded_long = entry.longs;
        }
    }
}

void TrendEnsemble::OnFill(Context& context, const types::Fill& fill) {
    mBook.OnFill(fill);
    if (fill.side == Side::kSell) {
        TryEnter(context);
    }
    RunPending(context);
}

void TrendEnsemble::RunPending(Context& context) {
    if (!mPending || mBook.OrdersOut()) {
        return;
    }
    if (!context.MarketOpen()) {
        mDue = std::exchange(mPending, std::nullopt);  // at the next open
        return;
    }
    Rebalance(context, *mPending);
}

void TrendEnsemble::OnOrderUpdate(Context& context,
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
        TryEnter(context);
    }
    RunPending(context);
}

}  // namespace kestrel::strategy
