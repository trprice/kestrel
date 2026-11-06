#include "kestrel/strategy/patterns.hpp"

#include <algorithm>

namespace kestrel::strategy {

using types::Decimal;

Decimal Range(const types::Bar& bar) { return bar.high - bar.low; }

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

std::optional<Decimal> Sma(std::span<const types::Bar> bars, std::size_t period,
                           std::size_t back) {
    if (period == 0 || bars.size() < period + back) {
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

}  // namespace kestrel::strategy
