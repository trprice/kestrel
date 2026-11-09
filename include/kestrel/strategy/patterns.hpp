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

// An inverse head-and-shoulders in the newest `n` bars: its newest three
// swing lows (SwingPoints, `strength`), a swing high between each pair, the
// middle one (the head) lower than the two beside it (the shoulders). The
// neckline runs through the highest high between the left shoulder and the
// head and the highest high between the head and the right shoulder, so it
// may slope; the height is the neckline less the head, measured at the head.
// The head must be below each shoulder by more than `zone` of the height (or
// it is a triple bottom), and each shoulder at least half the height below
// the neckline where it is. It has not broken out yet (no bar since the
// neckline's second point went above the line; bars between its points may
// rise above the chord, as a rally into a peak does) nor failed (none since the
// right shoulder fell below it by more than the zone). Nullopt when it is not
// one.
struct InverseHeadShoulders {
    PriceRange range;  // the neckline at the head, and the head
    // The level to buy above: the neckline at the bar after the newest, or the
    // newest bar's high if that is higher (a falling neckline can drop under
    // the price before it is broken).
    types::Decimal breakout;
};
std::optional<InverseHeadShoulders> InverseHeadShouldersOf(
    std::span<const types::Bar> bars, std::size_t n, std::size_t strength,
    const types::Decimal& zone);

// A triangle in the newest `n` bars, from its newest `touches` swing highs and
// newest `touches` swing lows (SwingPoints, `strength`; touches >= 2), which
// must overlap in time (the first low before the last high, and the first high
// before the last low). The lows rise, each above the one before. Ascending:
// the highs are flat, within `zone` of the height of the highest, which is the
// resistance. Symmetrical: the highs fall, each below the one before, and the
// resistance is the least-squares line through them, which may be fitted
// rather than through each. The height is the highest of those highs less the
// lowest of the lows. The lines must not have met by the next bar (the lows'
// line, fitted the same way, still under the resistance there). It has not
// broken out yet (no bar above the resistance since the `strength` bars after
// the last high, which are no higher than it) nor failed
// (none since the last low below it by more than the zone). Nullopt when it is
// not one.
enum class TriangleKind { kAscending, kSymmetrical };
struct Triangle {
    PriceRange range;  // the highest high and the lowest low used
    // The level to buy above: the resistance at the bar after the newest, or
    // the newest bar's high if that is higher.
    types::Decimal breakout;
    types::Decimal last_low;  // the newest swing low
};
// A lower bound on the bars that can hold a triangle of `touches`.
std::size_t TriangleBars(std::size_t touches, std::size_t strength);
std::optional<Triangle> TriangleOf(std::span<const types::Bar> bars,
                                   std::size_t n, TriangleKind kind,
                                   std::size_t touches, std::size_t strength,
                                   const types::Decimal& zone);

// A cup-and-handle in the newest `n` bars: two swing highs (SwingPoints,
// `strength`), the rims, with the cup between them: its highs no higher than
// the lower rim, the rims within `zone` of the depth (the right rim less the
// cup's lowest low) of each other, and rounded rather than a V, as at least
// two-fifths of the cup's bars have lows in the bottom third of its depth.
// The handle is every bar since the right rim: no more than half as many as
// the cup's, a pullback from the rim by more than the zone, but staying in
// the upper half of the depth, and not yet broken out (no bar above the right
// rim). Of the rims that make one, the deepest cup is used (the newest, on a
// tie), so a bounce in the handle back near the rim does not stand in for the
// cup before it. No prior advance is required. Nullopt when it is not one.
struct CupHandle {
    PriceRange range;           // the right rim and the cup's lowest low
    types::Decimal handle_low;  // the handle's lowest low
};
// The fewest bars that can hold a cup-and-handle of `strength`.
std::size_t CupHandleBars(std::size_t strength);
std::optional<CupHandle> CupHandleOf(std::span<const types::Bar> bars,
                                     std::size_t n, std::size_t strength,
                                     const types::Decimal& zone);

// The opening range: the high and low of a session's first `n` bars, answered
// on the newest bar when it is the n-th to open at or after `start` (the
// session's open), so once a session. The bar before them is needed to tell:
// nullopt on any other bar, with n = 0, or with fewer than n + 1 bars.
std::optional<PriceRange> OpeningRangeOf(std::span<const types::Bar> bars,
                                         std::size_t n, types::Timestamp start);

// The average close of the `period` bars ending `back` bars before the newest
// (back = 0: ending with it), to 8 decimal places.
std::optional<types::Decimal> Sma(std::span<const types::Bar> bars,
                                  std::size_t period, std::size_t back = 0);

// Indicators. Like Sma, each is the value at the bar `back` bars before the
// newest (back = 0: the newest), to 8 decimal places, and nullopt for a zero
// period or a window too short for it. The smoothed ones (Ema, Rsi, Atr,
// Macd) use the whole window, not just its last `period` bars: they are
// seeded with a simple average of its first values and smoothed over the
// rest, so they depend a little on how long it is. The seed's influence
// fades as (1 - alpha)^bars, so give them warmup_bars of a few times the
// period for values that match a charting package's. They carry as many
// places between steps as the prices allow (17 under 1, 11 at six figures),
// so nothing overflows: values are good to the 8th place up to about 10^8,
// and to about 10^-7 at nine figures (BTC in KRW). A price-valued result
// under about 0.01 has few significant digits in 8 places.

// The exponential moving average of closes, alpha = 2 / (period + 1), seeded
// with the average of the window's first `period` closes. Needs period bars.
std::optional<types::Decimal> Ema(std::span<const types::Bar> bars,
                                  std::size_t period, std::size_t back = 0);

// Wilder's relative strength index, 0 to 100: 100 - 100 / (1 + average gain /
// average loss) of the close-to-close changes, the averages seeded with the
// first `period` changes and then smoothed with alpha = 1 / period. 100 when
// there were no losses, 50 when the price never moved. Needs period + 1 bars.
std::optional<types::Decimal> Rsi(std::span<const types::Bar> bars,
                                  std::size_t period = 14,
                                  std::size_t back = 0);

// The true range of `bar` after one that closed at `prev_close`: the largest
// of its range and its high's and low's distances from that close, so a gap
// counts.
types::Decimal TrueRange(const types::Bar& bar,
                         const types::Decimal& prev_close);

// Wilder's average true range: the true ranges (each bar after the first)
// averaged over `period`, then smoothed with alpha = 1 / period. Needs
// period + 1 bars.
std::optional<types::Decimal> Atr(std::span<const types::Bar> bars,
                                  std::size_t period = 14,
                                  std::size_t back = 0);

// Bollinger bands: the `period`-bar average close (Sma) and `width` standard
// deviations of those closes (the population deviation, as Bollinger defines
// it) above and below it. Needs period bars; a negative width is an error
// (nullopt). upper and lower are middle plus and minus one rounded offset,
// width times the unrounded deviation.
struct BollingerBands {
    types::Decimal middle;
    types::Decimal upper;
    types::Decimal lower;
    types::Decimal deviation;  // the standard deviation of the closes
};
std::optional<BollingerBands> Bollinger(std::span<const types::Bar> bars,
                                        std::size_t period = 20,
                                        const types::Decimal& width = {2, 0},
                                        std::size_t back = 0);

// MACD: the `fast` EMA of closes less the `slow` one (the line), the
// `signal`-period EMA of that line (seeded with the average of its first
// `signal` values), and the line less the signal (the histogram). The line
// starts once the slow EMA has `slow` bars, so this needs slow + signal - 1
// bars. Nullopt also unless fast < slow. The histogram is exactly line less
// signal as returned.
struct MacdValue {
    types::Decimal line;
    types::Decimal signal;
    types::Decimal histogram;
};
std::optional<MacdValue> Macd(std::span<const types::Bar> bars,
                              std::size_t fast = 12, std::size_t slow = 26,
                              std::size_t signal = 9, std::size_t back = 0);

}  // namespace kestrel::strategy
