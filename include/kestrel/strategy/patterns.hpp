#pragma once

#include <cstddef>
#include <optional>
#include <span>

#include "kestrel/types/decimal.hpp"
#include "kestrel/types/market.hpp"

namespace kestrel::strategy {

// Bar patterns and indicators for strategies: pure functions over a window of
// closed bars, oldest first (Context::Bars). Each looks back from the newest
// bar and answers nullopt or false when the window is too short.

// High minus low.
types::Decimal Range(const types::Bar& bar);

// Whether the newest bar's range is narrower than each of the `n - 1` bars
// before it (n = 4 is "NR4"). An equal range is not narrower. False for n < 2
// or fewer than n bars.
bool IsNarrowRange(std::span<const types::Bar> bars, std::size_t n);

// Whether the newest bar is an inside bar: its high no higher and its low no
// lower than the bar before's. False with fewer than 2 bars.
bool IsInsideBar(std::span<const types::Bar> bars);

// The highest high and the lowest low of the newest `n` bars.
struct PriceRange {
    types::Decimal high;
    types::Decimal low;
    types::Decimal Height() const { return high - low; }
};
std::optional<PriceRange> RangeOf(std::span<const types::Bar> bars,
                                  std::size_t n);

// A trading range over the newest `n` bars (the deck's rectangle): its high
// (resistance) and low (support), and how many separate times the price came
// within `zone` of the height of each. A run of bars in a zone is one touch,
// and one wide bar can touch both. Nullopt with fewer than n bars, n = 0, or
// no height.
struct Rectangle {
    PriceRange range;
    std::size_t top_touches = 0;
    std::size_t bottom_touches = 0;
    // The "shortfall" that often precedes an upward break: since the last top
    // touch the price fell below the middle, but turned before touching the
    // bottom.
    bool shortfall_up = false;
};
std::optional<Rectangle> RectangleOf(std::span<const types::Bar> bars,
                                     std::size_t n, const types::Decimal& zone);

// A pipe bottom, or two-bar reversal, at the newest two bars: after a decline
// (the first makes a lower low than any of the `lookback` bars before it), the
// first closes in the lower quarter of its range and the second in the upper
// half of its own, and both are wider than those bars' average range. Its
// range spans both bars; the break is through the second bar's high. Nullopt
// when it is not one, or with lookback = 0 or fewer than lookback + 2 bars.
struct PipeBottom {
    PriceRange range;         // the higher high and lower low of the two
    types::Decimal breakout;  // the second bar's high
};
std::optional<PipeBottom> PipeBottomOf(std::span<const types::Bar> bars,
                                       std::size_t lookback);

// The average close of the `period` bars ending `back` bars before the newest
// (back = 0: ending with it), to 8 decimal places.
std::optional<types::Decimal> Sma(std::span<const types::Bar> bars,
                                  std::size_t period, std::size_t back = 0);

}  // namespace kestrel::strategy
