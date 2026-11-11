#include "kestrel/backtest/summary.hpp"

#include <algorithm>

#include "kestrel/trading/pnl_tracker.hpp"

namespace kestrel::backtest {

using types::Decimal;
using types::ErrorCategory;
using types::MakeError;
using types::Rounding;

namespace {

constexpr int kScale = 8;

Decimal Round(const Decimal& d) {
    return d.Rescale(kScale, Rounding::kNearest);
}

Decimal Fraction(const Decimal& part, const Decimal& whole) {
    return whole.Sign() > 0
               ? Decimal::Divide(part, whole, kScale, Rounding::kNearest)
               : Decimal();
}

// The fee in the quote asset, as PnlTracker values it.
Decimal FeeInQuote(const types::Fill& fill, const std::string& base) {
    return fill.fee_asset == base ? Round(fill.fee * fill.price) : fill.fee;
}

Decimal HeldOf(const std::map<std::string, Decimal>& balances,
               const std::string& asset) {
    auto it = balances.find(asset);
    return it == balances.end() ? Decimal() : it->second;
}

}  // namespace

types::Result<SummaryBuilder> SummaryBuilder::Create(
    const std::vector<types::Symbol>& symbols,
    const std::string& default_quote_asset,
    const std::map<std::string, Decimal>& start_balances,
    types::Timestamp start, std::chrono::milliseconds sample_interval) {
    SummaryBuilder builder;
    for (const types::Symbol& symbol : symbols) {
        auto parts = types::SplitSymbol(symbol);
        if (!parts) {
            return std::unexpected(parts.error());
        }
        const std::string quote = parts->quote.value_or(default_quote_asset);
        if (builder.mSummary.quote_asset.empty()) {
            builder.mSummary.quote_asset = quote;
        } else if (quote != builder.mSummary.quote_asset) {
            return MakeError(ErrorCategory::kInvalidArgument,
                             "the symbols do not share a quote asset (" +
                                 builder.mSummary.quote_asset + " and " +
                                 quote + "), so their equity cannot be added");
        }
        SymbolSummary s;
        s.symbol = symbol;
        s.base = parts->base;
        s.start_held = HeldOf(start_balances, s.base);
        s.min_held = s.max_held = s.end_held = s.start_held;
        builder.mSummary.symbols.push_back(std::move(s));
    }
    if (builder.mSummary.symbols.empty()) {
        return MakeError(ErrorCategory::kInvalidArgument,
                         "a summary needs at least one symbol");
    }
    for (const auto& [asset, total] : start_balances) {
        const bool known =
            asset == builder.mSummary.quote_asset ||
            std::ranges::any_of(builder.mSummary.symbols,
                                [&](const auto& s) { return s.base == asset; });
        if (!known && !total.IsZero()) {
            return MakeError(ErrorCategory::kInvalidArgument,
                             "the starting balance of " + asset +
                                 " has no price in this backtest");
        }
    }
    builder.mStartBalances = start_balances;
    builder.mBalances = start_balances;
    builder.mMarks.resize(builder.mSummary.symbols.size());
    builder.mMarkHistory.resize(builder.mSummary.symbols.size());
    builder.mStart = start;
    builder.mInterval = std::max(sample_interval, std::chrono::milliseconds(0));
    return builder;
}

std::optional<Decimal> SummaryBuilder::MarkOf(const types::Quote& quote) {
    if (quote.bid && quote.ask) {
        return Round((*quote.bid + *quote.ask) * Decimal(5, 1));
    }
    return quote.last;
}

std::optional<Decimal> SummaryBuilder::Value(
    const std::map<std::string, Decimal>& balances) const {
    Decimal sum;
    for (const auto& [asset, total] : balances) {
        if (total.IsZero()) {
            continue;
        }
        if (asset == mSummary.quote_asset) {
            sum += total;
            continue;
        }
        bool valued = false;
        for (std::size_t i = 0; i < mSummary.symbols.size(); ++i) {
            if (mSummary.symbols[i].base == asset) {
                if (!mMarks[i]) {
                    return std::nullopt;
                }
                sum += Round(total * *mMarks[i]);
                valued = true;
                break;
            }
        }
        if (!valued) {
            return std::nullopt;
        }
    }
    return sum;
}

EquityPoint SummaryBuilder::Point(types::Timestamp time) const {
    EquityPoint point;
    point.time = time;
    point.equity = mEquity;
    point.hold_equity = Value(mStartBalances).value_or(Decimal());
    for (std::size_t i = 0; i < mSummary.symbols.size(); ++i) {
        point.marks.push_back(mMarks[i].value_or(Decimal()));
        point.held.push_back(HeldOf(mBalances, mSummary.symbols[i].base));
    }
    return point;
}

void SummaryBuilder::Observe(types::Timestamp time, const types::Symbol& symbol,
                             std::optional<Decimal> mark,
                             const std::vector<types::Balance>& balances) {
    if (mValued && mInterval.count() > 0) {
        while (mNextSample < time) {
            mSummary.curve.push_back(Point(mNextSample));
            mNextSample += mInterval;
        }
    }

    for (std::size_t i = 0; i < mSummary.symbols.size(); ++i) {
        SymbolSummary& s = mSummary.symbols[i];
        if (mark && s.symbol == symbol) {
            if (!mMarks[i]) {
                s.first_mark = Round(*mark);
            }
            mMarks[i] = Round(*mark);
            mMarkHistory[i].emplace_back(time, *mMarks[i]);
        }
    }
    mBalances.clear();
    for (const types::Balance& balance : balances) {
        mBalances[balance.asset] = balance.total;
    }
    for (SymbolSummary& s : mSummary.symbols) {
        const Decimal held = HeldOf(mBalances, s.base);
        s.min_held = std::min(s.min_held, held);
        s.max_held = std::max(s.max_held, held);
    }

    const auto equity = Value(mBalances);
    if (!equity) {
        return;
    }
    mEquity = *equity;
    if (!mValued) {
        const auto start = Value(mStartBalances);
        if (!start) {
            return;
        }
        mValued = true;
        mSummary.start_time = time;
        mSummary.start_equity = *start;
        mPeak = mEquity;
        mPeakTime = time;
        mSummary.drawdown_peak_time = mSummary.drawdown_trough_time = time;
        mSummary.curve.push_back(Point(time));
        mNextSample = mStart;
        while (mInterval.count() > 0 && mNextSample <= time) {
            mNextSample += mInterval;
        }
        return;
    }
    if (mEquity > mPeak) {
        mPeak = mEquity;
        mPeakTime = time;
    } else if (const Decimal fall = mPeak - mEquity;
               fall > mSummary.max_drawdown) {
        mSummary.max_drawdown = fall;
        mSummary.max_drawdown_fraction = Fraction(fall, mPeak);
        mSummary.drawdown_peak_time = mPeakTime;
        mSummary.drawdown_trough_time = time;
    }
    if (mInterval.count() > 0) {
        if (mNextSample == time) {
            mSummary.curve.push_back(Point(mNextSample));
            mNextSample += mInterval;
        } else if (!mSummary.curve.empty() &&
                   mSummary.curve.back().time == time) {
            mSummary.curve.back() = Point(time);
        }
    }
}

std::vector<Markout> SummaryBuilder::Markouts(
    types::Timestamp end, const std::vector<types::Fill>& fills) const {
    std::vector<Markout> markouts(std::size(kMarkoutHorizons) + 2);
    markouts.front().horizon = std::chrono::milliseconds(0);
    for (std::size_t i = 0; i < std::size(kMarkoutHorizons); ++i) {
        markouts[i + 1].horizon = kMarkoutHorizons[i];
    }  // the last one's horizon stays none: the end

    using History = std::vector<std::pair<types::Timestamp, Decimal>>;
    // The last mark at or before `time` (strictly before, if `before`).
    const auto mark_at = [](const History& history, types::Timestamp time,
                            bool before) -> std::optional<Decimal> {
        const auto it =
            before ? std::ranges::lower_bound(history, time, {},
                                              &History::value_type::first)
                   : std::ranges::upper_bound(history, time, {},
                                              &History::value_type::first);
        if (it == history.begin()) {
            return std::nullopt;
        }
        return std::prev(it)->second;
    };

    for (const types::Fill& fill : fills) {
        const auto it = std::ranges::find(mSummary.symbols, fill.symbol,
                                          &SymbolSummary::symbol);
        if (it == mSummary.symbols.end()) {
            continue;
        }
        const History& history = mMarkHistory[static_cast<std::size_t>(
            it - mSummary.symbols.begin())];
        // The mid the order was resting against: before the quote that
        // filled it, which has already moved.
        if (!mark_at(history, fill.time, true)) {
            continue;
        }
        const Decimal notional = Round(fill.price * fill.quantity);
        for (Markout& m : markouts) {
            std::optional<Decimal> mid;
            if (!m.horizon) {
                mid = mark_at(history, end, false);
            } else if (m.horizon->count() == 0) {
                mid = mark_at(history, fill.time, true);
            } else if (fill.time + *m.horizon <= end) {
                mid = mark_at(history, fill.time + *m.horizon, false);
            }
            if (!mid) {
                continue;
            }
            const Decimal edge = fill.side == types::Side::kBuy
                                     ? *mid - fill.price
                                     : fill.price - *mid;
            ++m.fills;
            m.pnl += Round(edge * fill.quantity);
            m.volume += notional;
        }
    }
    for (Markout& m : markouts) {
        m.bps = Fraction(m.pnl * Decimal::FromInt(10000), m.volume)
                    .Rescale(2, Rounding::kNearest);
    }
    return markouts;
}

types::Result<BacktestSummary> SummaryBuilder::Finish(
    types::Timestamp end, const std::vector<types::Fill>& fills) {
    if (!mValued) {
        return MakeError(ErrorCategory::kNotFound,
                         "no price was replayed for every balance held, so "
                         "the equity was never known");
    }
    while (mInterval.count() > 0 && mNextSample < end) {
        mSummary.curve.push_back(Point(mNextSample));
        mNextSample += mInterval;
    }
    // A sample due at `end` was taken before anything that happened at it
    // (a bar closing at the end, say); the last point is the final state.
    if (!mSummary.curve.empty() && mSummary.curve.back().time == end) {
        mSummary.curve.pop_back();
    }
    mSummary.curve.push_back(Point(end));

    BacktestSummary& s = mSummary;
    s.end_time = end;
    s.end_equity = mEquity;
    // Known to value: mValued (checked above) is set only once it has.
    s.hold_equity = *Value(mStartBalances);
    s.return_fraction = Fraction(s.end_equity - s.start_equity, s.start_equity);
    for (std::size_t i = 0; i < s.symbols.size(); ++i) {
        s.symbols[i].last_mark = mMarks[i].value_or(s.symbols[i].first_mark);
        s.symbols[i].end_held = HeldOf(mBalances, s.symbols[i].base);
    }

    // The starting holding is given its symbol's first mark as cost basis,
    // so selling it realizes the move since then rather than the whole sale.
    trading::PnlTracker pnl(/*shorts=*/true);
    std::vector<types::Position> held;
    for (const SymbolSummary& symbol : s.symbols) {
        if (!symbol.start_held.IsZero()) {
            held.push_back(
                {symbol.symbol, symbol.start_held, symbol.first_mark, {}});
        }
    }
    pnl.Reset(held);
    for (const types::Fill& fill : fills) {
        const auto it =
            std::ranges::find(s.symbols, fill.symbol, &SymbolSummary::symbol);
        const std::string base = it == s.symbols.end() ? "" : it->base;
        ++s.fills;
        ++(fill.side == types::Side::kBuy ? s.buys : s.sells);
        ++(fill.liquidity == types::Liquidity::kMaker ? s.maker_fills
                                                      : s.taker_fills);
        s.volume += Round(fill.price * fill.quantity);
        s.fees += FeeInQuote(fill, base);

        const Decimal held_before = pnl.Held(fill.symbol);
        const Decimal before = pnl.Realized(fill.symbol);
        pnl.Apply(fill);
        if (fill.side == types::Side::kSell ? held_before.Sign() > 0
                                            : held_before.IsNegative()) {
            ++s.closing_fills;
            if ((pnl.Realized(fill.symbol) - before).Sign() > 0) {
                ++s.winning_fills;
            }
        }
    }
    s.markouts = Markouts(end, fills);
    s.realized = pnl.Realized();
    s.unrealized = s.end_equity - s.start_equity - s.realized;
    return s;
}

}  // namespace kestrel::backtest
