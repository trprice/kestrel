#include "kestrel/strategy/patterns.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace kestrel::strategy {

using types::Decimal;

Decimal Range(const types::Bar& bar) { return bar.high - bar.low; }

namespace {

// Whether `bars` holds `count` bars ending `back` before the newest, without
// the sum overflowing.
bool Holds(std::span<const types::Bar> bars, std::size_t count,
           std::size_t back) {
    return back <= bars.size() && count <= bars.size() - back;
}

}  // namespace

bool IsNarrowRange(std::span<const types::Bar> bars, std::size_t n) {
    if (n < 2 || bars.size() < n) {
        return false;
    }
    const Decimal newest = Range(bars.back());
    const auto before = bars.last(n).first(n - 1);
    return std::ranges::all_of(
        before, [&](const types::Bar& bar) { return newest < Range(bar); });
}

bool IsInsideBar(std::span<const types::Bar> bars) {
    if (bars.size() < 2) {
        return false;
    }
    const types::Bar& mother = bars[bars.size() - 2];
    const types::Bar& inside = bars.back();
    return inside.high <= mother.high && inside.low >= mother.low;
}

std::optional<PriceRange> RangeOf(std::span<const types::Bar> bars,
                                  std::size_t n) {
    if (n == 0 || bars.size() < n) {
        return std::nullopt;
    }
    const auto window = bars.last(n);
    PriceRange range{window.front().high, window.front().low};
    for (const types::Bar& bar : window) {
        range.high = std::max(range.high, bar.high);
        range.low = std::min(range.low, bar.low);
    }
    return range;
}

std::optional<Rectangle> RectangleOf(std::span<const types::Bar> bars,
                                     std::size_t n, const Decimal& zone) {
    const auto range = RangeOf(bars, n);
    if (!range || range->Height().Sign() <= 0) {
        return std::nullopt;
    }
    const Decimal band = range->Height() * zone;
    const Decimal top = range->high - band;
    const Decimal bottom = range->low + band;
    const Decimal middle = range->low + range->Height() * Decimal(5, 1);
    Rectangle rect{*range};
    bool at_top = false;
    bool at_bottom = false;
    std::optional<std::size_t> last_top;
    std::optional<std::size_t> last_bottom;
    const auto window = bars.last(n);
    for (std::size_t i = 0; i < window.size(); ++i) {
        const bool top_now = window[i].high >= top;
        const bool bottom_now = window[i].low <= bottom;
        if (top_now) {
            rect.top_touches += at_top ? 0 : 1;
            last_top = i;
        }
        if (bottom_now) {
            rect.bottom_touches += at_bottom ? 0 : 1;
            last_bottom = i;
        }
        at_top = top_now;
        at_bottom = bottom_now;
    }
    if (last_top && (!last_bottom || *last_bottom < *last_top)) {
        const auto after = window.subspan(*last_top + 1);
        rect.shortfall_up = std::ranges::any_of(
            after, [&](const types::Bar& bar) { return bar.low <= middle; });
    }
    return rect;
}

std::optional<PipeBottom> PipeBottomOf(std::span<const types::Bar> bars,
                                       std::size_t lookback) {
    if (lookback == 0 || bars.size() < lookback + 2) {
        return std::nullopt;
    }
    const types::Bar& first = bars[bars.size() - 2];
    const types::Bar& second = bars.back();
    const auto before = bars.last(lookback + 2).first(lookback);
    const Decimal first_range = Range(first);
    const Decimal second_range = Range(second);
    if (first_range.Sign() <= 0 || second_range.Sign() <= 0) {
        return std::nullopt;
    }
    Decimal ranges;
    for (const types::Bar& bar : before) {
        if (bar.low <= first.low) {
            return std::nullopt;  // not a new low: no decline into it
        }
        ranges += Range(bar);
    }
    // Wider than the average: range * lookback above the sum of the ranges.
    const Decimal n = Decimal::FromInt(static_cast<std::int64_t>(lookback));
    if (first_range * n <= ranges || second_range * n <= ranges) {
        return std::nullopt;
    }
    // The first closes in its lower quarter, the second in its upper half.
    if ((first.close - first.low) * Decimal::FromInt(4) > first_range ||
        (second.close - second.low) * Decimal::FromInt(2) < second_range) {
        return std::nullopt;
    }
    return PipeBottom{
        {std::max(first.high, second.high), std::min(first.low, second.low)},
        second.high};
}

std::optional<FailedBreakdown> FailedBreakdownOf(
    std::span<const types::Bar> bars, std::size_t n, std::size_t within) {
    if (n == 0 || within == 0 || bars.size() < n + within + 1) {
        return std::nullopt;
    }
    const auto recent = bars.last(within + 1);  // the break, then the newest
    const auto range = RangeOf(bars.first(bars.size() - within - 1), n);
    const types::Bar& newest = recent.back();
    const types::Bar& before = recent[recent.size() - 2];
    if (!range || before.close >= range->low || newest.close <= range->low) {
        return std::nullopt;
    }
    Decimal false_low = newest.low;
    for (const types::Bar& bar : recent) {
        false_low = std::min(false_low, bar.low);
    }
    return FailedBreakdown{*range, false_low, newest.high};
}

std::vector<Swing> SwingPoints(std::span<const types::Bar> bars,
                               std::size_t strength) {
    std::vector<Swing> swings;
    if (strength == 0 || bars.size() < 2 * strength + 1) {
        return swings;
    }
    for (std::size_t i = strength; i + strength < bars.size(); ++i) {
        bool low = true;
        bool high = true;
        for (std::size_t k = 1; k <= strength; ++k) {
            low = low && bars[i].low < bars[i - k].low &&
                  bars[i].low <= bars[i + k].low;
            high = high && bars[i].high > bars[i - k].high &&
                   bars[i].high >= bars[i + k].high;
        }
        if (high) {
            swings.push_back({i, bars[i].high, true});
        }
        if (low) {
            swings.push_back({i, bars[i].low, false});
        }
    }
    return swings;
}

std::size_t MultipleBottomBars(std::size_t bottoms, std::size_t strength) {
    // The first low needs `strength` bars before it and the last `strength`
    // after, and two swing lows are more than `strength` bars apart (each
    // must beat the other otherwise); a swing high fits between them.
    return 2 * strength + 1 + (bottoms - 1) * (strength + 1);
}

std::optional<MultipleBottom> MultipleBottomOf(std::span<const types::Bar> bars,
                                               std::size_t n,
                                               std::size_t bottoms,
                                               std::size_t strength,
                                               const Decimal& zone) {
    if (bottoms < 2 || n == 0 || bars.size() < n) {
        return std::nullopt;
    }
    const auto window = bars.last(n);
    const std::vector<Swing> swings = SwingPoints(window, strength);
    std::vector<Swing> lows;
    for (const Swing& swing : swings) {
        if (!swing.high) {
            lows.push_back(swing);
        }
    }
    if (lows.size() < bottoms) {
        return std::nullopt;
    }
    const auto chosen = std::span(lows).last(bottoms);
    const std::size_t first = chosen.front().index;
    const std::size_t last = chosen.back().index;
    // Each pair of lows must be separated by a peak: a swing high between.
    for (std::size_t k = 0; k + 1 < chosen.size(); ++k) {
        const std::size_t from = chosen[k].index;
        const std::size_t to = chosen[k + 1].index;
        const bool peak = std::ranges::any_of(swings, [&](const Swing& s) {
            return s.high && s.index > from && s.index < to;
        });
        if (!peak) {
            return std::nullopt;
        }
    }
    // The neckline: the highest high strictly between the first and the last
    // low (not the low bars' own highs: a wide bar into the first low would
    // put it where the rally between never went).
    Decimal neckline = window[first + 1].high;
    for (std::size_t i = first + 1; i < last; ++i) {
        neckline = std::max(neckline, window[i].high);
    }
    Decimal lowest = chosen.front().price;
    Decimal highest = chosen.front().price;
    for (const Swing& swing : chosen) {
        lowest = std::min(lowest, swing.price);
        highest = std::max(highest, swing.price);
    }
    const Decimal height = neckline - lowest;
    if (height.Sign() <= 0 || highest - lowest > height * zone) {
        return std::nullopt;  // no peak between, or bottoms too far apart
    }
    const Decimal floor = lowest - height * zone;
    for (const types::Bar& bar : window.subspan(last + 1)) {
        if (bar.high > neckline || bar.low < floor) {
            return std::nullopt;  // already broken out, or failed
        }
    }
    return MultipleBottom{{neckline, lowest}};
}

std::optional<Decimal> Sma(std::span<const types::Bar> bars, std::size_t period,
                           std::size_t back) {
    if (period == 0 || !Holds(bars, period, back)) {
        return std::nullopt;
    }
    Decimal sum;
    for (const types::Bar& bar : bars.first(bars.size() - back).last(period)) {
        sum += bar.close;
    }
    return Decimal::Divide(sum,
                           Decimal::FromInt(static_cast<std::int64_t>(period)),
                           8, types::Rounding::kNearest);
}

std::optional<PriceRange> OpeningRangeOf(std::span<const types::Bar> bars,
                                         std::size_t n,
                                         types::Timestamp start) {
    // The window opens at or after the open, and the bar before it before.
    if (n == 0 || bars.size() <= n || bars[bars.size() - n].open_time < start ||
        bars[bars.size() - n - 1].open_time >= start) {
        return std::nullopt;
    }
    return RangeOf(bars, n);
}

namespace {

using types::Rounding;
__extension__ using i128 = __int128;           // GCC and Clang, as in Decimal
__extension__ using u128 = unsigned __int128;  // likewise

// What the indicators answer: 8 decimal places, like Sma.
constexpr int kOutScale = 8;

Decimal Trim(const Decimal& value, int scale) {
    return value.Scale() > scale ? value.Rescale(scale, Rounding::kNearest)
                                 : value;
}

Decimal Out(const Decimal& value) { return Trim(value, kOutScale); }

Decimal Count(std::size_t n) {
    return Decimal::FromInt(static_cast<std::int64_t>(n));
}

// The bars up to `back` before the newest.
std::span<const types::Bar> Window(std::span<const types::Bar> bars,
                                   std::size_t back) {
    return bars.first(bars.size() - back);
}

std::vector<Decimal> Closes(std::span<const types::Bar> bars) {
    std::vector<Decimal> closes;
    closes.reserve(bars.size());
    for (const types::Bar& bar : bars) {
        closes.push_back(bar.close);
    }
    return closes;
}

// The places a running average of `values` can carry: as many as an int64
// mantissa holds beside the largest value's integer digits, less one for
// headroom, and never fewer than the values' own places. 17 for prices
// under 1, 11 for six figures, 8 for nine (BTC in KRW), so a tiny price keeps
// its significant digits and a large one does not overflow.
int WorkScale(std::span<const Decimal> values) {
    Decimal largest;
    int places = 0;
    for (const Decimal& value : values) {
        largest = std::max(largest, value.Abs());
        places = std::max(places, value.Scale());
    }
    int digits = 0;
    for (auto whole = static_cast<u128>(largest.Mantissa()); whole >= 1;) {
        ++digits;
        whole /= 10;
    }
    digits = std::max(0, digits - largest.Scale());  // integer digits
    return std::clamp(std::max(Decimal::kMaxScale - 1 - digits, places), 0,
                      Decimal::kMaxScale);
}

// The smoothed average of `values` (at least `period` of them): the mean of
// the first `period`, then value += alpha * (next - value) for each one after.
// One entry per value from the period-th on, at WorkScale(values).
std::vector<Decimal> Smoothed(std::span<const Decimal> values,
                              std::size_t period, const Decimal& alpha) {
    const int scale = WorkScale(values);
    Decimal sum;
    for (const Decimal& value : values.first(period)) {
        sum += value;
    }
    std::vector<Decimal> out;
    out.reserve(values.size() - period + 1);
    Decimal average =
        Decimal::Divide(sum, Count(period), scale, Rounding::kNearest);
    out.push_back(average);
    for (const Decimal& value : values.subspan(period)) {
        average = Trim(average + alpha * (value - average), scale);
        out.push_back(average);
    }
    return out;
}

Decimal EmaAlpha(std::size_t period) {
    return Decimal::Divide(Decimal::FromInt(2), Count(period + 1),
                           Decimal::kMaxScale, Rounding::kNearest);
}

Decimal WilderAlpha(std::size_t period) {
    return Decimal::Divide(Decimal::FromInt(1), Count(period),
                           Decimal::kMaxScale, Rounding::kNearest);
}

// 100 * part / (part + rest) to kOutScale places, both at least zero and not
// both zero, from their mantissas in 128 bits, so neither magnitude nor
// scale can overflow it.
Decimal Percent(const Decimal& part, const Decimal& rest) {
    i128 a = part.Mantissa();
    i128 b = rest.Mantissa();
    for (int s = part.Scale(); s < rest.Scale(); ++s) {
        a *= 10;  // under 10^19 * 10^18
    }
    for (int s = rest.Scale(); s < part.Scale(); ++s) {
        b *= 10;
    }
    constexpr i128 kLimit = static_cast<i128>(1'000'000'000'000'000'000) *
                            1'000'000'000;  // 10^27: room for * 10^10
    while (a >= kLimit || b >= kLimit) {
        a /= 10;
        b /= 10;
    }
    const i128 numerator = a * 10'000'000'000;  // 100 * 10^kOutScale
    const i128 denominator = a + b;
    const i128 percent = (numerator + denominator / 2) / denominator;
    return Decimal(static_cast<std::int64_t>(percent), kOutScale);
}

// The square root of sum / count (sum at least zero, count above zero), to
// 12 places where it fits and fewer where it does not, rounded to nearest:
// an integer square root of the quotient scaled by 10^(2 * scale), in 128
// bits, so six-figure prices squared lose nothing.
Decimal SqrtOfMean(const Decimal& sum, std::size_t count) {
    const auto mantissa = static_cast<u128>(sum.Mantissa());
    int digits = 0;
    for (u128 m = mantissa; m > 0; m /= 10) {
        ++digits;
    }
    // mantissa * 10^shift must stay under 10^38 (u128 holds 3.4e38).
    int scale = 12;
    while (scale > 0 && digits + 2 * scale - sum.Scale() > 38) {
        --scale;
    }
    u128 n = mantissa;
    for (int i = sum.Scale(); i < 2 * scale; ++i) {
        n *= 10;
    }
    for (int i = 2 * scale; i < sum.Scale(); ++i) {
        n /= 10;
    }
    const u128 c = count;
    n = (n + c / 2) / c;
    auto root = static_cast<u128>(std::sqrt(static_cast<long double>(n)));
    while (root > 0 && root * root > n) {
        --root;
    }
    while ((root + 1) * (root + 1) <= n) {
        ++root;
    }
    if (n - root * root > root) {  // past (root + 0.5)^2
        ++root;
    }
    constexpr auto kMax = static_cast<u128>(INT64_MAX);
    while (root > kMax) {
        root = (root + 5) / 10;
        --scale;
    }
    return Decimal(static_cast<std::int64_t>(root), scale);
}

}  // namespace

std::optional<Decimal> Ema(std::span<const types::Bar> bars, std::size_t period,
                           std::size_t back) {
    if (period == 0 || !Holds(bars, period, back)) {
        return std::nullopt;
    }
    const std::vector<Decimal> closes = Closes(Window(bars, back));
    return Out(Smoothed(closes, period, EmaAlpha(period)).back());
}

std::optional<Decimal> Rsi(std::span<const types::Bar> bars, std::size_t period,
                           std::size_t back) {
    // period + 1 bars, written so a huge period cannot wrap.
    if (period == 0 || !Holds(bars, period, back) ||
        bars.size() - back == period) {
        return std::nullopt;
    }
    const auto window = Window(bars, back);
    std::vector<Decimal> gains;
    std::vector<Decimal> losses;
    for (std::size_t i = 1; i < window.size(); ++i) {
        const Decimal change = window[i].close - window[i - 1].close;
        gains.push_back(change.IsNegative() ? Decimal() : change);
        losses.push_back(change.IsNegative() ? -change : Decimal());
    }
    const Decimal alpha = WilderAlpha(period);
    const Decimal gain = Smoothed(gains, period, alpha).back();
    const Decimal loss = Smoothed(losses, period, alpha).back();
    if (gain.IsZero() && loss.IsZero()) {
        return Decimal::FromInt(50);
    }
    // 100 - 100 / (1 + gain / loss), without dividing by a zero loss.
    return Percent(gain, loss);
}

Decimal TrueRange(const types::Bar& bar, const Decimal& prev_close) {
    return std::max({Range(bar), (bar.high - prev_close).Abs(),
                     (bar.low - prev_close).Abs()});
}

std::optional<Decimal> Atr(std::span<const types::Bar> bars, std::size_t period,
                           std::size_t back) {
    if (period == 0 || !Holds(bars, period, back) ||
        bars.size() - back == period) {
        return std::nullopt;
    }
    const auto window = Window(bars, back);
    std::vector<Decimal> ranges;
    ranges.reserve(window.size() - 1);
    for (std::size_t i = 1; i < window.size(); ++i) {
        ranges.push_back(TrueRange(window[i], window[i - 1].close));
    }
    return Out(Smoothed(ranges, period, WilderAlpha(period)).back());
}

std::optional<BollingerBands> Bollinger(std::span<const types::Bar> bars,
                                        std::size_t period,
                                        const Decimal& width,
                                        std::size_t back) {
    if (period == 0 || width.IsNegative() || !Holds(bars, period, back)) {
        return std::nullopt;
    }
    const std::vector<Decimal> closes = Closes(Window(bars, back).last(period));
    Decimal sum;
    for (const Decimal& close : closes) {
        sum += close;
    }
    const Decimal mean = Decimal::Divide(sum, Count(period), WorkScale(closes),
                                         Rounding::kNearest);
    Decimal squares;
    for (const Decimal& close : closes) {
        const Decimal deviation = close - mean;
        squares += deviation * deviation;
    }
    // The offset from the unrounded deviation, so the bands are as close as
    // 8 places allow; then upper - middle is exactly the offset.
    const Decimal middle = Out(mean);
    const Decimal deviation = SqrtOfMean(squares, period);
    const Decimal offset = Out(width * deviation);
    return BollingerBands{.middle = middle,
                          .upper = middle + offset,
                          .lower = middle - offset,
                          .deviation = Out(deviation)};
}

std::optional<MacdValue> Macd(std::span<const types::Bar> bars,
                              std::size_t fast, std::size_t slow,
                              std::size_t signal, std::size_t back) {
    // slow + signal - 1 bars, written so huge values cannot wrap.
    if (fast == 0 || signal == 0 || fast >= slow || !Holds(bars, slow, back) ||
        signal - 1 > bars.size() - back - slow) {
        return std::nullopt;
    }
    const std::vector<Decimal> closes = Closes(Window(bars, back));
    const std::vector<Decimal> fast_ema =
        Smoothed(closes, fast, EmaAlpha(fast));
    const std::vector<Decimal> slow_ema =
        Smoothed(closes, slow, EmaAlpha(slow));
    // fast_ema[i + slow - fast] and slow_ema[i] are the same bar's.
    std::vector<Decimal> line;
    line.reserve(slow_ema.size());
    for (std::size_t i = 0; i < slow_ema.size(); ++i) {
        line.push_back(fast_ema[i + slow - fast] - slow_ema[i]);
    }
    const Decimal line_value = Out(line.back());
    const Decimal signal_value =
        Out(Smoothed(line, signal, EmaAlpha(signal)).back());
    return MacdValue{.line = line_value,
                     .signal = signal_value,
                     .histogram = line_value - signal_value};
}

}  // namespace kestrel::strategy
