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
