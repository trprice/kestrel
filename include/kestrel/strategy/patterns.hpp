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

// The average close of the `period` bars ending `back` bars before the newest
// (back = 0: ending with it), to 8 decimal places.
std::optional<types::Decimal> Sma(std::span<const types::Bar> bars,
                                  std::size_t period, std::size_t back = 0);

}  // namespace kestrel::strategy
