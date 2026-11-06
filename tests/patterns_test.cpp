#include "kestrel/strategy/patterns.hpp"

#include <gtest/gtest.h>

#include <vector>

#include "test_helpers.hpp"

namespace kestrel::strategy {
namespace {

using kestrel::testing::D;

// A bar from its high, low and close (open = close).
types::Bar B(const char* high, const char* low, const char* close = nullptr) {
    types::Bar bar;
    bar.high = D(high);
    bar.low = D(low);
    bar.open = bar.close = D(close ? close : low);
    return bar;
}

TEST(Patterns, Range) { EXPECT_EQ(Range(B("12.5", "10")), D("2.5")); }

TEST(Patterns, NarrowRange) {
    // Ranges 4, 3, 5, 2: the last is narrower than each of the three before.
    const std::vector<types::Bar> nr4 = {B("14", "10"), B("13", "10"),
                                         B("15", "10"), B("12", "10")};
    EXPECT_TRUE(IsNarrowRange(nr4, 4));
    EXPECT_TRUE(IsNarrowRange(nr4, 2));
    // Only the newest n bars count: an earlier, narrower bar does not matter.
    std::vector<types::Bar> longer = {B("11", "10")};
    longer.insert(longer.end(), nr4.begin(), nr4.end());
    EXPECT_TRUE(IsNarrowRange(longer, 4));
    EXPECT_FALSE(IsNarrowRange(longer, 5));
    // An equal range is not narrower.
    const std::vector<types::Bar> tie = {B("14", "10"), B("12", "10"),
                                         B("15", "10"), B("12", "10")};
    EXPECT_FALSE(IsNarrowRange(tie, 4));
    // Too few bars, or n < 2.
    EXPECT_FALSE(IsNarrowRange(nr4, 5));
    EXPECT_FALSE(IsNarrowRange(nr4, 1));
    EXPECT_FALSE(IsNarrowRange({}, 4));
}

TEST(Patterns, InsideBar) {
    EXPECT_TRUE(IsInsideBar(std::vector{B("15", "10"), B("14", "11")}));
    // Touching the previous high or low still counts as inside.
    EXPECT_TRUE(IsInsideBar(std::vector{B("15", "10"), B("15", "10")}));
    EXPECT_FALSE(IsInsideBar(std::vector{B("15", "10"), B("16", "11")}));
    EXPECT_FALSE(IsInsideBar(std::vector{B("15", "10"), B("14", "9")}));
    EXPECT_FALSE(IsInsideBar(std::vector{B("15", "10")}));
}

TEST(Patterns, RangeOfTheNewestBars) {
    const std::vector<types::Bar> bars = {B("30", "1"), B("15", "10"),
                                          B("18", "12"), B("14", "9")};
    const auto range = RangeOf(bars, 3);  // not the first bar's 30 and 1
    ASSERT_TRUE(range);
    EXPECT_EQ(range->high, D("18"));
    EXPECT_EQ(range->low, D("9"));
    EXPECT_EQ(range->Height(), D("9"));
    EXPECT_FALSE(RangeOf(bars, 5));
    EXPECT_FALSE(RangeOf(bars, 0));
}

// High 20, low 10, a zone of 0.1: touches come at 19 and above, and at 11
// and below; the middle is 15.
std::vector<types::Bar> Rect() {
    return {B("20", "15"),     // top 1
            B("19.5", "14"),   // still top 1: the same run
            B("16", "10"),     // bottom 1
            B("18", "12"),     // neither
            B("19", "13"),     // top 2 (exactly at the zone)
            B("15", "11"),     // bottom 2
            B("17", "12"),     // neither
            B("19.2", "16")};  // top 3
}

TEST(Patterns, RectangleCountsSeparateTouchesOfEachBound) {
    const auto rect = RectangleOf(Rect(), 8, D("0.1"));
    ASSERT_TRUE(rect.has_value());
    EXPECT_EQ(rect->range.high, D("20"));
    EXPECT_EQ(rect->range.low, D("10"));
    EXPECT_EQ(rect->top_touches, 3u);
    EXPECT_EQ(rect->bottom_touches, 2u);
    EXPECT_FALSE(rect->shortfall_up);  // nothing after the last top touch
    // A narrower zone (19.5 and 10.5) counts fewer touches.
    const auto narrow = RectangleOf(Rect(), 8, D("0.05"));
    ASSERT_TRUE(narrow.has_value());
    EXPECT_EQ(narrow->top_touches, 1u);
    EXPECT_EQ(narrow->bottom_touches, 1u);
    // Only the newest n bars: without the first, the high is 19.5.
    const auto newer = RectangleOf(Rect(), 7, D("0.1"));
    ASSERT_TRUE(newer.has_value());
    EXPECT_EQ(newer->range.high, D("19.5"));
}

TEST(Patterns, RectangleShortfall) {
    auto bars = Rect();
    bars.push_back(B("17", "14.5"));  // below the middle, short of the bottom
    auto rect = RectangleOf(bars, 9, D("0.1"));
    ASSERT_TRUE(rect.has_value());
    EXPECT_TRUE(rect->shortfall_up);
    bars.back() = B("18", "15.5");  // not down to the middle
    EXPECT_FALSE(RectangleOf(bars, 9, D("0.1"))->shortfall_up);
    bars.back() = B("16", "11");  // down to the bottom: a touch, no shortfall
    rect = RectangleOf(bars, 9, D("0.1"));
    EXPECT_FALSE(rect->shortfall_up);
    EXPECT_EQ(rect->bottom_touches, 3u);
}

TEST(Patterns, RectangleNeedsBarsAndHeight) {
    EXPECT_FALSE(RectangleOf(Rect(), 9, D("0.1")).has_value());
    EXPECT_FALSE(RectangleOf(Rect(), 0, D("0.1")).has_value());
    const std::vector<types::Bar> flat = {B("10", "10"), B("10", "10")};
    EXPECT_FALSE(RectangleOf(flat, 2, D("0.1")).has_value());
}

// Three bars of range 1 (lows 11), then a wide bar to a new low of 9 that
// closes near it (9.2), then a wide bar closing in its upper half (10.5).
std::vector<types::Bar> Pipe() {
    return {B("12", "11", "11.5"), B("12", "11", "11.5"), B("12", "11", "11.5"),
            B("11.5", "9", "9.2"), B("11", "9.1", "10.5")};
}

TEST(Patterns, PipeBottom) {
    const auto pipe = PipeBottomOf(Pipe(), 3);
    ASSERT_TRUE(pipe.has_value());
    EXPECT_EQ(pipe->range.high, D("11.5"));
    EXPECT_EQ(pipe->range.low, D("9"));
    EXPECT_EQ(pipe->breakout, D("11"));  // the second bar's high
    // A shorter look back still sees the decline.
    EXPECT_TRUE(PipeBottomOf(Pipe(), 1).has_value());
}

TEST(Patterns, PipeBottomNeedsEachPart) {
    auto bars = Pipe();
    bars[3] = B("11.5", "9", "10");  // the first closes mid-bar
    EXPECT_FALSE(PipeBottomOf(bars, 3).has_value());
    bars = Pipe();
    bars[4] = B("11", "9.1", "9.5");  // the second closes in its lower half
    EXPECT_FALSE(PipeBottomOf(bars, 3).has_value());
    bars = Pipe();
    bars[4] = B("9.6", "9.1", "9.5");  // the second no wider than average
    EXPECT_FALSE(PipeBottomOf(bars, 3).has_value());
    bars = Pipe();
    bars[1] = B("12", "8.9", "11.5");  // an earlier low below: no new low
    EXPECT_FALSE(PipeBottomOf(bars, 3).has_value());
    // Too few bars, or no look back.
    EXPECT_FALSE(PipeBottomOf(Pipe(), 4).has_value());
    EXPECT_FALSE(PipeBottomOf(Pipe(), 0).has_value());
}

TEST(Patterns, SimpleMovingAverage) {
    const std::vector<types::Bar> bars = {B("10", "0", "1"), B("10", "0", "2"),
                                          B("10", "0", "3"), B("10", "0", "4")};
    EXPECT_EQ(Sma(bars, 2), D("3.5"));     // 3 and 4
    EXPECT_EQ(Sma(bars, 2, 1), D("2.5"));  // 2 and 3
    EXPECT_EQ(Sma(bars, 4), D("2.5"));
    EXPECT_EQ(Sma(bars, 3), D("3"));
    // Rounded to 8 places: (1 + 2 + 4) / 3 is not exact.
    const std::vector<types::Bar> thirds = {
        B("10", "0", "1"), B("10", "0", "2"), B("10", "0", "4")};
    EXPECT_EQ(Sma(thirds, 3), D("2.33333333"));
    EXPECT_FALSE(Sma(bars, 4, 1));
    EXPECT_FALSE(Sma(bars, 0));
    EXPECT_FALSE(Sma(bars, 5));
}

}  // namespace
}  // namespace kestrel::strategy
