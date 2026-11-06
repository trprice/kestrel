#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

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

// A failed breakdown (the long side of the deck's false breakout, or trap) at
// the newest bar: the bar before closed below the low of the `n` bars before
// the newest `within + 1`, and the newest closes back above that low, the
// first close back inside. The break must have started within those `within`
// bars, as the range's own closes are never below its low: the bars of a break
// that started earlier fall inside the range and lower its low, so the break
// is measured from a level it never closed below. Nullopt when it is
// not one, or with n or within = 0 or fewer than n + within + 1 bars.
struct FailedBreakdown {
    PriceRange range;             // the range that was broken
    types::Decimal false_low;     // the lowest low since the break
    types::Decimal reclaim_high;  // the newest bar's high
};
std::optional<FailedBreakdown> FailedBreakdownOf(
    std::span<const types::Bar> bars, std::size_t n, std::size_t within);

// The swing points (pivots) of a window of bars, oldest first: a bar whose low
// is below the lows of the `strength` bars before it and no higher than those
// of the `strength` after it is a swing low, and likewise a swing high with
// its high (so a run of equal extremes counts once, at its first bar). The
// newest `strength` bars cannot be swings yet. Empty for strength = 0.
struct Swing {
    std::size_t index;     // in `bars`
    types::Decimal price;  // the bar's low, or its high
    bool high = false;
};
std::vector<Swing> SwingPoints(std::span<const types::Bar> bars,
                               std::size_t strength);

// A double (bottoms = 2) or triple (3) bottom in the newest `n` bars: its
// newest `bottoms` swing lows (SwingPoints, `strength`), each pair separated
// by a swing high, lie within `zone` of the pattern's height of each other.
// The neckline is the highest high strictly between the first and the last
// low (for a triple, the higher of its peaks, as the deck has it), and the
// height is the neckline less the lowest low. It has not broken out yet (no
// bar since the last low went above the neckline) nor failed (none fell below
// the lowest low by more than the zone; a smaller dip is allowed, and does not
// move the low). With bottoms = 2, a formed triple counts as its newest two
// lows. No prior decline is required. Nullopt when it is not one, or for
// bottoms < 2.
struct MultipleBottom {
    PriceRange range;  // the neckline and the lowest low
};
// The fewest bars that can hold `bottoms` swing lows of `strength` with a
// swing high between each pair.
std::size_t MultipleBottomBars(std::size_t bottoms, std::size_t strength);
std::optional<MultipleBottom> MultipleBottomOf(std::span<const types::Bar> bars,
                                               std::size_t n,
                                               std::size_t bottoms,
                                               std::size_t strength,
                                               const types::Decimal& zone);

// The average close of the `period` bars ending `back` bars before the newest
// (back = 0: ending with it), to 8 decimal places.
std::optional<types::Decimal> Sma(std::span<const types::Bar> bars,
                                  std::size_t period, std::size_t back = 0);

}  // namespace kestrel::strategy
