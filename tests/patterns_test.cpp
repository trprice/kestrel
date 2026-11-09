#include "kestrel/strategy/patterns.hpp"

#include <gtest/gtest.h>

#include <array>
#include <initializer_list>
#include <limits>
#include <optional>
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

// A range of 4 bars (high 12, low 10), two bars closing below 10 (lows 9.5 and
// 9.4), then a close back above it.
std::vector<types::Bar> Failed() {
    return {B("12", "10", "11"),     B("12", "10", "11"),
            B("12", "10", "11"),     B("12", "10", "11"),
            B("10.5", "9.5", "9.6"), B("10.2", "9.4", "9.8"),
            B("10.6", "9.9", "10.4")};
}

TEST(Patterns, FailedBreakdown) {
    const auto failed = FailedBreakdownOf(Failed(), 4, 2);
    ASSERT_TRUE(failed.has_value());
    EXPECT_EQ(failed->range.high, D("12"));
    EXPECT_EQ(failed->range.low, D("10"));
    EXPECT_EQ(failed->false_low, D("9.4"));
    EXPECT_EQ(failed->reclaim_high, D("10.6"));
    // A break one bar long, after a bar still inside, counts too.
    auto bars = Failed();
    bars[4] = B("11", "10.5", "11");
    const auto short_break = FailedBreakdownOf(bars, 4, 2);
    ASSERT_TRUE(short_break.has_value());
    EXPECT_EQ(short_break->false_low, D("9.4"));
}

TEST(Patterns, FailedBreakdownNeedsACloseBelowThenAbove) {
    auto bars = Failed();
    bars[6] = B("10.1", "9.7", "9.9");  // still below: no reclaim yet
    EXPECT_FALSE(FailedBreakdownOf(bars, 4, 2).has_value());
    bars = Failed();
    bars[5] = B("10.4", "9.6", "10.1");  // back inside a bar earlier
    EXPECT_FALSE(FailedBreakdownOf(bars, 4, 2).has_value());
    bars = Failed();
    bars[4] = B("10.5", "9.5", "10.2");  // lows below, closes above: no
    bars[5] = B("10.4", "9.4", "10.1");  // break by close
    EXPECT_FALSE(FailedBreakdownOf(bars, 4, 2).has_value());
    // Too few bars, or n or within of zero.
    EXPECT_FALSE(FailedBreakdownOf(Failed(), 5, 2).has_value());
    EXPECT_FALSE(FailedBreakdownOf(Failed(), 0, 2).has_value());
    EXPECT_FALSE(FailedBreakdownOf(Failed(), 4, 0).has_value());
    // A break longer than `within`: its first bar (low 9.5) falls in the
    // range, whose low the bar before the newest (close 9.8) is not below.
    EXPECT_FALSE(FailedBreakdownOf(Failed(), 4, 1).has_value());
}

TEST(Patterns, SwingPoints) {
    // Lows 12, 10, 11, 13, 12, 10.2, 11; highs 14, 12, 13, 15, 14, 11.5, 12.5.
    const std::vector<types::Bar> bars = {
        B("14", "12"), B("12", "10"),     B("13", "11"),  B("15", "13"),
        B("14", "12"), B("11.5", "10.2"), B("12.5", "11")};
    const auto swings = SwingPoints(bars, 1);
    ASSERT_EQ(swings.size(), 3u);
    EXPECT_EQ(swings[0].index, 1u);  // the low of 10
    EXPECT_FALSE(swings[0].high);
    EXPECT_EQ(swings[1].index, 3u);  // the high of 15
    EXPECT_TRUE(swings[1].high);
    EXPECT_EQ(swings[2].index, 5u);  // the low of 10.2
    EXPECT_EQ(swings[2].price, D("10.2"));
    // With two bars a side, only the high of 15 is beaten on both.
    const auto wider = SwingPoints(bars, 2);
    ASSERT_EQ(wider.size(), 1u);
    EXPECT_EQ(wider[0].index, 3u);
    // Equal lows count once, at the first.
    const std::vector<types::Bar> flat = {B("12", "11"), B("12", "10"),
                                          B("12", "10"), B("12", "11")};
    const auto once = SwingPoints(flat, 1);
    ASSERT_EQ(once.size(), 1u);
    EXPECT_EQ(once[0].index, 1u);
    EXPECT_TRUE(SwingPoints(bars, 0).empty());
    EXPECT_TRUE(SwingPoints(bars, 4).empty());  // too few bars
}

// Two swing lows (10, then 10.2) with a peak of 15 between them, and the
// price since still under 15 and above the lows. With a zone of 0.1 the
// height is 5, so the lows may be 0.5 apart.
std::vector<types::Bar> DoubleBottom() {
    return {B("14", "12"),   B("12", "10"),          B("13", "11"),
            B("15", "13"),   B("14", "12"),          B("11.5", "10.2"),
            B("12.5", "11"), B("13", "11.5", "12.8")};
}

TEST(Patterns, DoubleBottom) {
    const auto bottom = MultipleBottomOf(DoubleBottom(), 8, 2, 1, D("0.1"));
    ASSERT_TRUE(bottom.has_value());
    EXPECT_EQ(bottom->range.high, D("15"));  // the neckline
    EXPECT_EQ(bottom->range.low, D("10"));
    EXPECT_FALSE(MultipleBottomOf(DoubleBottom(), 8, 3, 1, D("0.1")));
}

TEST(Patterns, DoubleBottomNeedsEachPart) {
    auto bars = DoubleBottom();
    bars[5] = B("11.5", "11");  // lows 1 apart, more than 0.5
    EXPECT_FALSE(MultipleBottomOf(bars, 8, 2, 1, D("0.1")).has_value());
    bars = DoubleBottom();
    bars[7] = B("15.5", "11.5");  // already above the neckline
    EXPECT_FALSE(MultipleBottomOf(bars, 8, 2, 1, D("0.1")).has_value());
    bars = DoubleBottom();
    bars[7] = B("11", "9.4");  // under the lows by more than 0.5: failed
    EXPECT_FALSE(MultipleBottomOf(bars, 8, 2, 1, D("0.1")).has_value());
    // The first low outside the window: one low only.
    EXPECT_FALSE(MultipleBottomOf(DoubleBottom(), 6, 2, 1, D("0.1")));
    EXPECT_FALSE(MultipleBottomOf(DoubleBottom(), 9, 2, 1, D("0.1")));
    EXPECT_FALSE(MultipleBottomOf(DoubleBottom(), 8, 1, 1, D("0.1")));
}

TEST(Patterns, TheNecklineIsBetweenTheLows) {
    // A wide bar into the first low (high 20): the neckline is the rally
    // between the lows, 13, not that bar's high.
    const std::vector<types::Bar> bars = {
        B("21", "12"), B("20", "10"),     B("12", "11"), B("13", "11.5"),
        B("12", "11"), B("11.5", "10.1"), B("12", "11")};
    const auto bottom = MultipleBottomOf(bars, 7, 2, 1, D("0.1"));
    ASSERT_TRUE(bottom.has_value());
    EXPECT_EQ(bottom->range.high, D("13"));
    EXPECT_EQ(bottom->range.low, D("10"));
}

TEST(Patterns, DoubleBottomLowsNeedAPeakBetween) {
    // Two swing lows (10, 10.1) with highs only falling between them: no
    // swing high, so no pattern.
    const std::vector<types::Bar> bars = {B("14", "12"),     B("12", "10"),
                                          B("11.8", "10.5"), B("11.6", "10.4"),
                                          B("11.5", "10.1"), B("12", "11")};
    EXPECT_FALSE(MultipleBottomOf(bars, 6, 2, 1, D("0.1")).has_value());
}

TEST(Patterns, DoubleBottomAllowsADipInsideTheZone) {
    // Down to 9.7 after the second low: below 10 but above the failure
    // floor of 9.5; the low stays 10 until the dip is itself a swing.
    auto bars = DoubleBottom();
    bars[7] = B("12", "9.7", "11.5");
    const auto bottom = MultipleBottomOf(bars, 8, 2, 1, D("0.1"));
    ASSERT_TRUE(bottom.has_value());
    EXPECT_EQ(bottom->range.low, D("10"));
}

TEST(Patterns, DoubleBottomWithWiderSwings) {
    // DoubleBottom with each bar twice: at a strength of 2 the lows are the
    // first of each pair, and the peak of 15 the first of its pair.
    std::vector<types::Bar> bars;
    for (const types::Bar& bar : DoubleBottom()) {
        bars.push_back(bar);
        bars.push_back(bar);
    }
    const auto bottom = MultipleBottomOf(bars, 16, 2, 2, D("0.1"));
    ASSERT_TRUE(bottom.has_value());
    EXPECT_EQ(bottom->range.high, D("15"));
    EXPECT_EQ(bottom->range.low, D("10"));
}

TEST(Patterns, MultipleBottomBars) {
    EXPECT_EQ(MultipleBottomBars(2, 1), 5u);  // low, high, low and a side each
    EXPECT_EQ(MultipleBottomBars(2, 2), 8u);
    EXPECT_EQ(MultipleBottomBars(3, 2), 11u);
}

TEST(Patterns, TripleBottom) {
    // Lows of 10, 10.1 and 10.2 with peaks of 14 and 15 between.
    const std::vector<types::Bar> bars = {
        B("13", "12"), B("12", "10"),   B("14", "11"), B("12", "10.1"),
        B("15", "11"), B("12", "10.2"), B("13", "11")};
    const auto triple = MultipleBottomOf(bars, 7, 3, 1, D("0.1"));
    ASSERT_TRUE(triple.has_value());
    EXPECT_EQ(triple->range.high, D("15"));  // the higher of the peaks
    EXPECT_EQ(triple->range.low, D("10"));
    // As a double bottom, the newest two lows and the peak of 15 between.
    const auto twice = MultipleBottomOf(bars, 7, 2, 1, D("0.1"));
    ASSERT_TRUE(twice.has_value());
    EXPECT_EQ(twice->range.low, D("10.1"));
}

TEST(Patterns, OpeningRange) {
    // Bars at minutes 0, 5, ..., 25; the session opens at minute 10.
    std::vector<types::Bar> bars = {B("20", "1"),  B("21", "2"), B("12", "10"),
                                    B("13", "11"), B("14", "9"), B("15", "12")};
    for (std::size_t i = 0; i < bars.size(); ++i) {
        bars[i].open_time = types::FromEpochSeconds(static_cast<int>(i) * 300);
    }
    const auto open = types::FromEpochSeconds(600);
    const std::span<const types::Bar> all(bars);
    // On the third bar from the open, the range of those three only.
    const auto range = OpeningRangeOf(all.first(5), 3, open);
    ASSERT_TRUE(range);
    EXPECT_EQ(range->high, D("14"));
    EXPECT_EQ(range->low, D("9"));
    // Before it and after it, none: once a session.
    EXPECT_FALSE(OpeningRangeOf(all.first(4), 3, open));
    EXPECT_FALSE(OpeningRangeOf(all, 3, open));
    // One bar: the first bar from the open.
    EXPECT_TRUE(OpeningRangeOf(all.first(3), 1, open));
    // The bar before the open is needed to tell, and n must be positive.
    EXPECT_FALSE(OpeningRangeOf(all.subspan(2, 3), 3, open));
    EXPECT_FALSE(OpeningRangeOf(all.first(5), 0, open));
}

// Swing lows of 10 (the left shoulder), 8 (the head) and 10.2 (the right),
// with peaks of 15 between: a flat neckline at 15, a height of 7, so the
// zone of 0.1 is 0.7.
std::vector<types::Bar> HeadShoulders() {
    return {B("14", "12"),   B("12", "10"), B("13", "11"),
            B("15", "13"),   B("13", "11"), B("11", "8"),
            B("13", "10"),   B("15", "12"), B("13", "11"),
            B("12", "10.2"), B("13", "11"), B("14", "12.5", "13.5")};
}

TEST(Patterns, InverseHeadShoulders) {
    const auto hs = InverseHeadShouldersOf(HeadShoulders(), 12, 1, D("0.1"));
    ASSERT_TRUE(hs.has_value());
    EXPECT_EQ(hs->range.high, D("15"));
    EXPECT_EQ(hs->range.low, D("8"));
    EXPECT_EQ(hs->breakout, D("15"));
    // A double bottom of the head and the right shoulder does not hold: they
    // are 2.2 apart.
    EXPECT_FALSE(MultipleBottomOf(HeadShoulders(), 12, 2, 1, D("0.1")));
}

TEST(Patterns, InverseHeadShouldersWithASlopingNeckline) {
    // The second peak 14: the neckline falls 0.25 a bar, 14.5 at the head
    // (a height of 6.5) and 12.75 at the bar after the newest, under the
    // newest bar's high of 12.9, which is the level instead.
    auto bars = HeadShoulders();
    bars[7] = B("14", "12");
    bars[11] = B("12.9", "12.5", "12.8");
    const auto hs = InverseHeadShouldersOf(bars, 12, 1, D("0.1"));
    ASSERT_TRUE(hs.has_value());
    EXPECT_EQ(hs->range.high, D("14.5"));
    EXPECT_EQ(hs->range.low, D("8"));
    EXPECT_EQ(hs->breakout, D("12.9"));
    bars[11] = B("12.7", "12.5", "12.6");
    EXPECT_EQ(InverseHeadShouldersOf(bars, 12, 1, D("0.1"))->breakout,
              D("12.75"));
    // A newest bar's high of 13.1 is above the line there (13): broken.
    bars[11] = B("13.1", "12.5", "13");
    EXPECT_FALSE(InverseHeadShouldersOf(bars, 12, 1, D("0.1")).has_value());
}

TEST(Patterns, InverseHeadShouldersWithARisingNeckline) {
    // The first peak 14 and the second 15.01: the neckline rises 0.2525 a
    // bar, 14.505 at the head and 16.2725 at the bar after the newest.
    auto bars = HeadShoulders();
    bars[3] = B("14", "13");
    bars[7] = B("15.01", "12");
    const auto hs = InverseHeadShouldersOf(bars, 12, 1, D("0.1"));
    ASSERT_TRUE(hs.has_value());
    EXPECT_EQ(hs->range.high, D("14.505"));
    EXPECT_EQ(hs->breakout, D("16.2725"));
    // A rally into the second peak above the chord (14.7575 at the sixth)
    // is part of the pattern, not a break.
    bars[6] = B("14.9", "10");
    EXPECT_TRUE(InverseHeadShouldersOf(bars, 12, 1, D("0.1")).has_value());
    // After the right shoulder, a bar above the line is (15.7675 at the
    // tenth).
    bars[10] = B("15.8", "11");
    EXPECT_FALSE(InverseHeadShouldersOf(bars, 12, 1, D("0.1")).has_value());
}

TEST(Patterns, InverseHeadShouldersNeedsEachPart) {
    auto bars = HeadShoulders();
    bars[9] = B("12", "8.5");  // the right shoulder 0.5 over the head: < 0.7
    EXPECT_FALSE(InverseHeadShouldersOf(bars, 12, 1, D("0.1")).has_value());
    bars[9] = B("12", "8.75");  // 0.75 over it
    EXPECT_TRUE(InverseHeadShouldersOf(bars, 12, 1, D("0.1")).has_value());
    bars = HeadShoulders();
    bars[1] = B("12", "8.5");  // the left shoulder 0.5 over the head
    EXPECT_FALSE(InverseHeadShouldersOf(bars, 12, 1, D("0.1")).has_value());
    bars = HeadShoulders();
    // A left shoulder of 12, 3 under the neckline: less than half of 7.
    bars[0] = B("14", "12.5");
    bars[1] = B("13", "12");
    bars[2] = B("13.5", "12.5");
    EXPECT_FALSE(InverseHeadShouldersOf(bars, 12, 1, D("0.1")).has_value());
    bars = HeadShoulders();
    bars[11] = B("15.5", "12.5");  // already above the neckline
    EXPECT_FALSE(InverseHeadShouldersOf(bars, 12, 1, D("0.1")).has_value());
    bars = HeadShoulders();
    bars[11] = B("11", "9.4");  // under the right shoulder by more than 0.7
    EXPECT_FALSE(InverseHeadShouldersOf(bars, 12, 1, D("0.1")).has_value());
    bars[11] = B("11", "9.6");  // a dip inside the zone
    EXPECT_TRUE(InverseHeadShouldersOf(bars, 12, 1, D("0.1")).has_value());
    bars = HeadShoulders();
    // A right shoulder of 12, 3 under the neckline: less than half of 7.
    bars[8] = B("13.5", "12.5");
    bars[9] = B("13", "12");
    bars[10] = B("13.5", "12.5");
    EXPECT_FALSE(InverseHeadShouldersOf(bars, 12, 1, D("0.1")).has_value());
    // The left shoulder outside the window: two lows only.
    EXPECT_FALSE(InverseHeadShouldersOf(HeadShoulders(), 8, 1, D("0.1")));
    EXPECT_FALSE(InverseHeadShouldersOf(HeadShoulders(), 13, 1, D("0.1")));
    EXPECT_FALSE(InverseHeadShouldersOf(HeadShoulders(), 0, 1, D("0.1")));
}

// Swing highs of 15, 14.9 and 15, and rising swing lows of 10, 11.5 and
// 12.5 between them.
std::vector<types::Bar> Ascending() {
    return {B("13", "11"),   B("15", "13"),     B("13", "10"),
            B("14.9", "12"), B("13.5", "11.5"), B("15", "13"),
            B("14", "12.5"), B("14.5", "13"),   B("14.6", "13.5", "14.2")};
}

TEST(Patterns, AscendingTriangle) {
    // The newest two of each: highs 14.9 and 15, lows 11.5 and 12.5; a height
    // of 3.5, so the highs may be 0.35 apart.
    const auto two =
        TriangleOf(Ascending(), 9, TriangleKind::kAscending, 2, 1, D("0.1"));
    ASSERT_TRUE(two.has_value());
    EXPECT_EQ(two->range.high, D("15"));
    EXPECT_EQ(two->range.low, D("11.5"));
    EXPECT_EQ(two->breakout, D("15"));
    EXPECT_EQ(two->last_low, D("12.5"));
    const auto three =
        TriangleOf(Ascending(), 9, TriangleKind::kAscending, 3, 1, D("0.1"));
    ASSERT_TRUE(three.has_value());
    EXPECT_EQ(three->range.low, D("10"));
    // The highs are flat, not falling: no symmetrical triangle.
    EXPECT_FALSE(
        TriangleOf(Ascending(), 9, TriangleKind::kSymmetrical, 2, 1, D("0.1")));
}

TEST(Patterns, AscendingTriangleNeedsEachPart) {
    const auto of = [](const std::vector<types::Bar>& bars) {
        return TriangleOf(bars, 9, TriangleKind::kAscending, 2, 1, D("0.1"))
            .has_value();
    };
    auto bars = Ascending();
    bars[3] = B("14.5", "12");  // highs 0.5 apart: not flat
    EXPECT_FALSE(of(bars));
    bars[3] = B("14.7", "12");  // 0.3 apart
    EXPECT_TRUE(of(bars));
    bars = Ascending();
    bars[6] = B("14", "11.4");  // the lows fall
    EXPECT_FALSE(of(bars));
    bars = Ascending();
    bars[8] = B("15.2", "13.5");  // already above the resistance
    EXPECT_FALSE(of(bars));
    bars = Ascending();
    bars[8] = B("14.6", "12.1");  // under the last low by more than 0.35
    EXPECT_FALSE(of(bars));
    bars[8] = B("14.6", "12.2");
    EXPECT_TRUE(of(bars));
    EXPECT_FALSE(
        TriangleOf(Ascending(), 9, TriangleKind::kAscending, 1, 1, D("0.1")));
    EXPECT_FALSE(
        TriangleOf(Ascending(), 9, TriangleKind::kAscending, 4, 1, D("0.1")));
    EXPECT_FALSE(
        TriangleOf(Ascending(), 10, TriangleKind::kAscending, 2, 1, D("0.1")));
    // Only the newest four bars: one low.
    EXPECT_FALSE(
        TriangleOf(Ascending(), 4, TriangleKind::kAscending, 2, 1, D("0.1")));
}

// Swing highs falling 0.1 a bar (16, 15.8, 15.6) and swing lows rising 0.25
// a bar (10, 10.5, 11).
std::vector<types::Bar> Symmetrical() {
    return {B("14", "12"),   B("16", "13"),   B("13", "10"),
            B("15.8", "12"), B("13", "10.5"), B("15.6", "12.5"),
            B("14", "11"),   B("14.5", "12"), B("14.8", "12.5", "14.5")};
}

TEST(Patterns, SymmetricalTriangle) {
    const auto two = TriangleOf(Symmetrical(), 9, TriangleKind::kSymmetrical, 2,
                                1, D("0.1"));
    ASSERT_TRUE(two.has_value());
    EXPECT_EQ(two->range.high, D("15.8"));
    EXPECT_EQ(two->range.low, D("10.5"));
    EXPECT_EQ(two->breakout, D("15.2"));  // the line at the next bar
    EXPECT_EQ(two->last_low, D("11"));
    const auto three = TriangleOf(Symmetrical(), 9, TriangleKind::kSymmetrical,
                                  3, 1, D("0.1"));
    ASSERT_TRUE(three.has_value());
    EXPECT_EQ(three->range.high, D("16"));
    EXPECT_EQ(three->range.low, D("10"));
    EXPECT_EQ(three->breakout, D("15.2"));
}

TEST(Patterns, SymmetricalTriangleFitsALineThroughTheHighs) {
    // Highs of 16, 15.9 and 15.6 at bars 1, 3 and 5: the least-squares line
    // falls 0.1 a bar from a mean of 15.8333..., 15.23333333 at bar 9.
    auto bars = Symmetrical();
    bars[3] = B("15.9", "12");
    const auto fitted =
        TriangleOf(bars, 9, TriangleKind::kSymmetrical, 3, 1, D("0.1"));
    ASSERT_TRUE(fitted.has_value());
    EXPECT_EQ(fitted->breakout, D("15.23333333"));
    // A newest bar's high between the line there (15.33333333) and at the
    // next bar is the level instead.
    bars[8] = B("15.3", "12.5");
    EXPECT_EQ(TriangleOf(bars, 9, TriangleKind::kSymmetrical, 3, 1, D("0.1"))
                  ->breakout,
              D("15.3"));
    bars[8] = B("15.4", "12.5");  // above it: broken out
    EXPECT_FALSE(
        TriangleOf(bars, 9, TriangleKind::kSymmetrical, 3, 1, D("0.1")));
}

TEST(Patterns, SymmetricalTriangleNeedsEachPart) {
    const auto of = [](const std::vector<types::Bar>& bars) {
        return TriangleOf(bars, 9, TriangleKind::kSymmetrical, 2, 1, D("0.1"))
            .has_value();
    };
    auto bars = Symmetrical();
    bars[5] = B("15.8", "12.5");  // the highs flat, not falling
    EXPECT_FALSE(of(bars));
    bars = Symmetrical();
    // Lows rising 0.95 a bar from 10.5: 15.25 at the next bar, past the
    // highs' 15.2 there. The lines have met.
    bars[6] = B("14", "12.4");
    bars[7] = B("14.5", "12.6");
    bars[8] = B("14.8", "12.8");
    EXPECT_FALSE(of(bars));
    bars[6] = B("14", "12.2");  // 14.75 there
    EXPECT_TRUE(of(bars));
    bars = Symmetrical();
    bars[8] = B("15.4", "12.5");  // above the line (15.3) at the newest bar
    EXPECT_FALSE(of(bars));
    // The bar after the last high (15.6) is under it but above the line
    // (15.5): part of the swing, not a break.
    bars = Symmetrical();
    bars[6] = B("15.55", "11");
    EXPECT_TRUE(of(bars));
    bars[7] = B("15.45", "12");  // the line is 15.4 here
    EXPECT_FALSE(of(bars));
}

TEST(Patterns, ATrianglesHighsAndLowsOverlap) {
    // Flat highs at bars 1 and 3, then rising lows at 5 and 7: a top and a
    // decline, not a triangle.
    const std::vector<types::Bar> bars = {
        B("13", "12"),      B("15", "12.5"),   B("14", "12.6"),
        B("14.95", "12.7"), B("14", "12"),     B("13.5", "11"),
        B("13.4", "11.5"),  B("13.3", "11.3"), B("13.2", "11.6")};
    EXPECT_FALSE(TriangleOf(bars, 9, TriangleKind::kAscending, 2, 1, D("0.1")));
}

TEST(Patterns, TriangleBars) {
    EXPECT_EQ(TriangleBars(2, 1), 6u);
    EXPECT_EQ(TriangleBars(2, 2), 9u);
}

// Rims of 15 and 15.1 around a rounded cup down to 10 (a depth of 5.1, so
// the zone of 0.1 is 0.51), then a two-bar handle down to 13.6.
std::vector<types::Bar> CupHandle() {
    return {B("12", "10"),     B("15", "13"),     B("14", "12"),
            B("13", "11"),     B("12", "10.2"),   B("11.8", "10"),
            B("12", "10.1"),   B("13", "11"),     B("14", "12"),
            B("15.1", "13.5"), B("14.5", "13.6"), B("14.8", "14", "14.5")};
}

TEST(Patterns, CupHandle) {
    const auto cup = CupHandleOf(CupHandle(), 12, 1, D("0.1"));
    ASSERT_TRUE(cup.has_value());
    EXPECT_EQ(cup->range.high, D("15.1"));
    EXPECT_EQ(cup->range.low, D("10"));
    EXPECT_EQ(cup->handle_low, D("13.6"));
}

TEST(Patterns, CupHandleNeedsEachPart) {
    const auto of = [](const std::vector<types::Bar>& bars) {
        return CupHandleOf(bars, bars.size(), 1, D("0.1")).has_value();
    };
    auto bars = CupHandle();
    // A V: only the bottom bar's low is in the bottom third (under 11.7).
    bars[2] = B("14", "13.5");
    bars[3] = B("13", "12.5");
    bars[4] = B("12", "12");
    bars[6] = B("12", "12");
    bars[7] = B("13", "12.5");
    bars[8] = B("14", "13.5");
    EXPECT_FALSE(of(bars));
    bars = CupHandle();
    bars[10] = B("14.5", "12.4");  // the handle below the half (12.55)
    EXPECT_FALSE(of(bars));
    bars[10] = B("14.5", "12.6");
    EXPECT_TRUE(of(bars));
    bars = CupHandle();
    bars[10] = B("14.9", "14.7");  // a pullback of 0.4: under the zone
    bars[11] = B("15", "14.8");
    EXPECT_FALSE(of(bars));
    bars[10] = B("14.9", "14.5");  // of 0.6
    EXPECT_TRUE(of(bars));
    bars = CupHandle();
    bars[11] = B("15.2", "14");  // already above the right rim
    EXPECT_FALSE(of(bars));
    bars = CupHandle();
    bars[9] = B("15.7", "13.5");  // rims 0.7 apart, over the zone of 0.57
    EXPECT_FALSE(of(bars));
    bars = CupHandle();
    bars[5] = B("15.05", "10");  // a high inside the cup above the rim
    EXPECT_FALSE(of(bars));
    bars = CupHandle();
    bars.push_back(B("14.6", "13.8"));
    EXPECT_TRUE(of(bars));  // a handle of 3 bars against a cup of 7
    bars.push_back(B("14.7", "13.9"));
    EXPECT_FALSE(of(bars));  // 4: over half
    // The left rim outside the window.
    EXPECT_FALSE(CupHandleOf(CupHandle(), 10, 1, D("0.1")).has_value());
    EXPECT_FALSE(CupHandleOf(CupHandle(), 13, 1, D("0.1")).has_value());
}

TEST(Patterns, CupHandleTakesTheDeepestCup) {
    // CupHandle with a cup of 8 bars and a handle of 4 whose bounce to 15.05
    // is a swing high near the rim: rims of 15.1 and 15.05 around the
    // handle's first bars also make a (shallow) cup-and-handle.
    auto bars = CupHandle();
    bars.insert(bars.begin() + 6, B("11.9", "10.05"));
    bars.resize(11);
    for (const types::Bar& bar : {B("14.5", "13.6"), B("14.4", "13.7"),
                                  B("15.05", "14"), B("14.6", "14.4")}) {
        bars.push_back(bar);
    }
    const auto shallow = CupHandleOf(std::span(bars).last(6), 6, 1, D("0.1"));
    ASSERT_TRUE(shallow.has_value());
    EXPECT_EQ(shallow->range.low, D("13.6"));
    const auto cup = CupHandleOf(bars, bars.size(), 1, D("0.1"));
    ASSERT_TRUE(cup.has_value());
    EXPECT_EQ(cup->range.high, D("15.1"));
    EXPECT_EQ(cup->range.low, D("10"));
    EXPECT_EQ(cup->handle_low, D("13.6"));
}

TEST(Patterns, CupHandleWithWiderSwings) {
    std::vector<types::Bar> bars;
    for (const types::Bar& bar : CupHandle()) {
        bars.push_back(bar);
        bars.push_back(bar);
    }
    const auto cup = CupHandleOf(bars, bars.size(), 2, D("0.1"));
    ASSERT_TRUE(cup.has_value());
    EXPECT_EQ(cup->range.high, D("15.1"));
    EXPECT_EQ(cup->range.low, D("10"));
}

TEST(Patterns, CupHandleBars) {
    EXPECT_EQ(CupHandleBars(1), 6u);
    EXPECT_EQ(CupHandleBars(2), 10u);
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

// Closes alone (high = low = close).
std::vector<types::Bar> Closes(std::initializer_list<const char*> closes) {
    std::vector<types::Bar> bars;
    for (const char* close : closes) {
        bars.push_back(B(close, close, close));
    }
    return bars;
}

// 60 hourly-looking bars around 1200, for values checked against an exact
// (rational) reference computation of each definition.
const std::vector<types::Bar>& Series() {
    static const std::vector<types::Bar> bars = {
        B("1236.10", "1223.78", "1227.82"), B("1234.97", "1227.08", "1234.48"),
        B("1253.59", "1233.52", "1248.11"), B("1254.07", "1242.49", "1243.08"),
        B("1265.53", "1240.89", "1260.34"), B("1261.22", "1237.43", "1241.87"),
        B("1242.58", "1236.53", "1238.99"), B("1244.63", "1218.36", "1222.70"),
        B("1228.49", "1203.86", "1205.12"), B("1226.20", "1198.67", "1223.92"),
        B("1235.57", "1223.29", "1229.61"), B("1239.23", "1225.55", "1233.24"),
        B("1235.50", "1214.80", "1215.27"), B("1219.43", "1212.31", "1218.07"),
        B("1219.54", "1209.70", "1215.23"), B("1221.07", "1196.90", "1200.05"),
        B("1209.97", "1198.20", "1202.99"), B("1208.94", "1181.37", "1187.21"),
        B("1195.29", "1183.40", "1193.37"), B("1198.97", "1170.07", "1177.36"),
        B("1183.13", "1159.32", "1159.93"), B("1167.38", "1154.85", "1165.28"),
        B("1178.58", "1160.91", "1173.14"), B("1188.18", "1168.38", "1184.97"),
        B("1193.59", "1181.27", "1188.95"), B("1191.49", "1179.38", "1181.22"),
        B("1197.83", "1178.73", "1189.85"), B("1195.73", "1170.13", "1173.20"),
        B("1179.77", "1169.69", "1174.71"), B("1189.17", "1171.77", "1184.58"),
        B("1190.26", "1183.38", "1189.52"), B("1194.76", "1187.84", "1190.48"),
        B("1204.99", "1188.93", "1201.49"), B("1224.71", "1197.18", "1219.71"),
        B("1226.55", "1200.52", "1201.31"), B("1218.33", "1195.45", "1212.62"),
        B("1228.15", "1209.14", "1224.94"), B("1236.99", "1218.86", "1233.41"),
        B("1239.68", "1228.74", "1233.75"), B("1234.70", "1213.80", "1216.56"),
        B("1223.69", "1209.17", "1215.97"), B("1216.59", "1191.15", "1198.63"),
        B("1210.53", "1192.01", "1207.36"), B("1218.00", "1202.80", "1211.03"),
        B("1218.36", "1198.73", "1202.68"), B("1225.85", "1199.13", "1219.01"),
        B("1223.73", "1196.30", "1199.93"), B("1206.18", "1185.62", "1186.81"),
        B("1187.63", "1184.58", "1187.03"), B("1201.43", "1185.71", "1198.49"),
        B("1211.26", "1194.42", "1208.73"), B("1213.81", "1203.92", "1204.74"),
        B("1209.33", "1187.44", "1191.55"), B("1196.89", "1190.15", "1194.05"),
        B("1212.00", "1188.42", "1207.60"), B("1214.83", "1194.75", "1199.00"),
        B("1205.99", "1189.80", "1193.69"), B("1215.27", "1192.15", "1212.91"),
        B("1214.71", "1194.76", "1196.30"), B("1203.04", "1183.42", "1185.80"),
    };
    return bars;
}

// Within `tolerance` (one unit in the 8th place) of `expected`.
void ExpectNear(const std::optional<types::Decimal>& actual,
                const char* expected, const char* tolerance = "0.00000001") {
    ASSERT_TRUE(actual);
    EXPECT_LE((*actual - D(expected)).Abs(), D(tolerance))
        << *actual << " against " << expected;
}

TEST(Indicators, Ema) {
    // alpha = 0.5: seeded with (1 + 2 + 3) / 3 = 2, then 3, then 4.
    const auto bars = Closes({"1", "2", "3", "4", "5"});
    EXPECT_EQ(Ema(bars, 3), D("4"));
    EXPECT_EQ(Ema(bars, 3, 1), D("3"));
    EXPECT_EQ(Ema(bars, 3, 2), D("2"));
    EXPECT_FALSE(Ema(bars, 3, 3));
    EXPECT_FALSE(Ema(bars, 0));
    EXPECT_FALSE(Ema(bars, 6));
    EXPECT_EQ(Ema(bars, 1), D("5"));  // alpha = 1: the close itself
    ExpectNear(Ema(Series(), 20), "1200.37138738");
    ExpectNear(Ema(Series(), 20, 3), "1201.39895007");
    ExpectNear(Ema(Series(), 12), "1198.71087984");
}

TEST(Indicators, Rsi) {
    // Changes +1, -1, +2; period 2: gains seeded 0.5 then 1.25, losses 0.5
    // then 0.25, so 100 * 1.25 / 1.5.
    const auto bars = Closes({"10", "11", "10", "12"});
    EXPECT_EQ(Rsi(bars, 2), D("83.33333333"));
    EXPECT_EQ(Rsi(bars, 2, 1), D("50"));  // one gain, one loss of 1
    EXPECT_EQ(Rsi(Closes({"1", "2", "3"}), 2), D("100"));
    EXPECT_EQ(Rsi(Closes({"3", "2", "1"}), 2), D("0"));
    EXPECT_EQ(Rsi(Closes({"2", "2", "2"}), 2), D("50"));
    EXPECT_FALSE(Rsi(bars, 4));  // needs period + 1 bars
    EXPECT_FALSE(Rsi(bars, 3, 1));
    EXPECT_FALSE(Rsi(bars, 0));
    ExpectNear(Rsi(Series()), "44.39322895");
    ExpectNear(Rsi(Series(), 14, 5), "51.87861662");
}

TEST(Indicators, TrueRange) {
    // A gap counts: from a close below the bar, or above it.
    EXPECT_EQ(TrueRange(B("12", "10"), D("8")), D("4"));
    EXPECT_EQ(TrueRange(B("12", "10"), D("13")), D("3"));
    EXPECT_EQ(TrueRange(B("12", "10"), D("11")), D("2"));
}

TEST(Indicators, Atr) {
    // True ranges 2, 4 and 1; period 2: seeded 3, then 3 + (1 - 3) / 2.
    const std::vector<types::Bar> bars = {
        B("10", "10", "10"), B("11", "9", "10"), B("14", "12", "13"),
        B("13", "12", "12")};
    EXPECT_EQ(Atr(bars, 2), D("2"));
    EXPECT_EQ(Atr(bars, 2, 1), D("3"));
    EXPECT_FALSE(Atr(bars, 2, 2));
    EXPECT_FALSE(Atr(bars, 4));
    EXPECT_FALSE(Atr(bars, 0));
    ExpectNear(Atr(Series()), "18.13856820");
    ExpectNear(Atr(Series(), 14, 2), "17.87650514");
}

TEST(Indicators, Bollinger) {
    // Mean 5, population deviation exactly 2.
    const auto bars = Closes({"2", "4", "4", "4", "5", "5", "7", "9"});
    const auto bands = Bollinger(bars, 8);
    ASSERT_TRUE(bands);
    EXPECT_EQ(bands->middle, D("5"));
    EXPECT_EQ(bands->deviation, D("2"));
    EXPECT_EQ(bands->upper, D("9"));
    EXPECT_EQ(bands->lower, D("1"));
    // 5, 5, 7, 9: mean 6.5, variance 2.75.
    const auto last4 = Bollinger(bars, 4, D("1"));
    ASSERT_TRUE(last4);
    EXPECT_EQ(last4->deviation, D("1.65831240"));
    EXPECT_EQ(last4->upper, D("8.15831240"));
    const auto flat = Bollinger(bars, 3, D("0"), 4);  // 4, 4, 4
    ASSERT_TRUE(flat);
    EXPECT_EQ(flat->deviation, D("0"));
    EXPECT_EQ(flat->upper, flat->lower);
    EXPECT_FALSE(Bollinger(bars, 8, D("-1")));
    EXPECT_FALSE(Bollinger(bars, 8, D("2"), 1));
    EXPECT_FALSE(Bollinger(bars, 0));

    const auto series = Bollinger(Series());
    ASSERT_TRUE(series);
    EXPECT_EQ(series->middle, D("1201.0655"));
    ExpectNear(series->upper, "1220.08731219");
    ExpectNear(series->lower, "1182.04368781");
    ExpectNear(series->deviation, "9.51090610");
    const auto wide = Bollinger(Series(), 20, D("2.5"), 1);
    ASSERT_TRUE(wide);
    ExpectNear(wide->upper, "1226.11467989");
    ExpectNear(wide->lower, "1179.09232011");
}

TEST(Indicators, BollingerLargePrices) {
    // Six-figure prices (BTC) swinging by thousands: the squares stay exact.
    const auto bars =
        Closes({"100000", "104000", "96000", "108000", "92000", "100000"});
    const auto bands = Bollinger(bars, 6, D("2"));
    ASSERT_TRUE(bands);
    EXPECT_EQ(bands->middle, D("100000"));
    // Squares 0, 16e6, 16e6, 64e6, 64e6, 0: variance 160e6 / 6.
    EXPECT_EQ(bands->deviation, D("5163.97779494"));
}

TEST(Indicators, Macd) {
    // fast 2 (alpha 2/3), slow 3 (alpha 1/2), signal 2 on closes 1..5:
    // fast EMA 1.5, 2.5, 3.5, 4.5; slow 2, 3, 4; line 0.5 three times, so
    // the signal is 0.5 and the histogram 0.
    const auto bars = Closes({"1", "2", "3", "4", "5"});
    const auto macd = Macd(bars, 2, 3, 2);
    ASSERT_TRUE(macd);
    EXPECT_EQ(macd->line, D("0.5"));
    EXPECT_EQ(macd->signal, D("0.5"));
    EXPECT_EQ(macd->histogram, D("0"));
    EXPECT_TRUE(Macd(bars, 2, 3, 2, 1));   // 4 bars = slow + signal - 1
    EXPECT_FALSE(Macd(bars, 2, 3, 2, 2));  // 3 bars
    EXPECT_FALSE(Macd(bars, 3, 3, 2));     // fast must be below slow
    EXPECT_FALSE(Macd(bars, 0, 3, 2));
    EXPECT_FALSE(Macd(bars, 2, 3, 0));

    const auto series = Macd(Series());
    ASSERT_TRUE(series);
    ExpectNear(series->line, "-2.94218074");
    ExpectNear(series->signal, "-2.28133297");
    ExpectNear(series->histogram, "-0.66084776");
    const auto earlier = Macd(Series(), 12, 26, 9, 4);
    ASSERT_TRUE(earlier);
    ExpectNear(earlier->line, "-2.35492492");
    ExpectNear(earlier->signal, "-2.23586229");
    ExpectNear(earlier->histogram, "-0.11906263");
}

// Bars from {high, low, close}.
std::vector<types::Bar> Hlc(
    std::initializer_list<std::array<const char*, 3>> bars) {
    std::vector<types::Bar> out;
    for (const auto& [high, low, close] : bars) {
        out.push_back(B(high, low, close));
    }
    return out;
}

TEST(Indicators, PriceScales) {
    // The working precision follows the prices: a tiny price keeps its
    // significant digits and a nine-figure one does not overflow. Each is
    // checked against the exact computation.
    {
        // BTC/USDT-like, around 65,000 with cent ticks.
        const auto bars = Hlc({{"65001.45", "64998.21", "64998.37"},
                               {"64998.67", "64996.72", "64997.98"},
                               {"65000.78", "64997.01", "64999.58"},
                               {"64999.82", "64997.48", "64998.72"},
                               {"64999.71", "64994.91", "64996.01"},
                               {"64997.15", "64992.35", "64993.03"},
                               {"64993.29", "64991.56", "64992.37"},
                               {"64992.42", "64989.62", "64989.68"},
                               {"64992.24", "64988.71", "64992.22"},
                               {"64993.30", "64991.36", "64991.43"},
                               {"64994.39", "64990.31", "64993.83"},
                               {"64997.31", "64993.24", "64995.90"},
                               {"64997.02", "64995.34", "64996.43"},
                               {"64998.87", "64996.38", "64998.13"},
                               {"65000.81", "64997.88", "64999.39"},
                               {"65000.14", "64997.99", "64998.29"},
                               {"64999.97", "64997.21", "64998.69"},
                               {"65001.36", "64997.92", "65000.88"},
                               {"65002.38", "64999.51", "65000.78"},
                               {"65003.95", "64999.28", "65002.95"},
                               {"65004.17", "64999.68", "65000.30"},
                               {"65002.49", "64999.86", "65001.43"},
                               {"65003.58", "65000.48", "65002.18"},
                               {"65003.30", "64998.76", "65000.06"},
                               {"65000.47", "64996.83", "64998.16"},
                               {"65000.12", "64996.91", "64999.18"},
                               {"65000.38", "64996.37", "64996.48"},
                               {"64998.11", "64995.48", "64996.63"},
                               {"64997.06", "64994.09", "64995.37"},
                               {"64995.40", "64994.18", "64994.69"},
                               {"64998.61", "64994.10", "64997.21"},
                               {"64999.66", "64996.33", "64998.35"},
                               {"65002.16", "64997.18", "65001.26"},
                               {"65002.66", "65001.00", "65001.01"},
                               {"65003.24", "65000.68", "65001.93"},
                               {"65005.67", "65001.41", "65004.24"},
                               {"65005.74", "65003.01", "65005.60"},
                               {"65007.78", "65004.19", "65006.33"},
                               {"65007.62", "65004.32", "65005.37"},
                               {"65008.24", "65004.31", "65007.33"}});
        ExpectNear(Ema(bars, 10), "65003.55822196");
        ExpectNear(Rsi(bars), "66.42720990");
        ExpectNear(Atr(bars), "3.30195531");
        const auto macd = Macd(bars);
        ASSERT_TRUE(macd);
        ExpectNear(macd->line, "2.47872611");
    }
    {
        // SHIB-like, around 0.00001 with 1e-8 ticks.
        const auto bars = Hlc({{"0.00001260", "0.00001232", "0.00001259"},
                               {"0.00001270", "0.00001229", "0.00001234"},
                               {"0.00001260", "0.00001226", "0.00001251"},
                               {"0.00001265", "0.00001250", "0.00001259"},
                               {"0.00001271", "0.00001246", "0.00001266"},
                               {"0.00001288", "0.00001255", "0.00001276"},
                               {"0.00001294", "0.00001268", "0.00001280"},
                               {"0.00001308", "0.00001280", "0.00001307"},
                               {"0.00001321", "0.00001290", "0.00001300"},
                               {"0.00001340", "0.00001287", "0.00001328"},
                               {"0.00001360", "0.00001323", "0.00001355"},
                               {"0.00001362", "0.00001340", "0.00001340"},
                               {"0.00001350", "0.00001316", "0.00001321"},
                               {"0.00001332", "0.00001294", "0.00001299"},
                               {"0.00001340", "0.00001286", "0.00001326"},
                               {"0.00001354", "0.00001315", "0.00001343"},
                               {"0.00001357", "0.00001331", "0.00001336"},
                               {"0.00001366", "0.00001322", "0.00001354"},
                               {"0.00001372", "0.00001339", "0.00001365"},
                               {"0.00001380", "0.00001341", "0.00001352"},
                               {"0.00001378", "0.00001338", "0.00001364"},
                               {"0.00001378", "0.00001341", "0.00001356"},
                               {"0.00001375", "0.00001346", "0.00001368"},
                               {"0.00001395", "0.00001360", "0.00001390"},
                               {"0.00001424", "0.00001381", "0.00001409"},
                               {"0.00001422", "0.00001389", "0.00001398"},
                               {"0.00001420", "0.00001383", "0.00001414"},
                               {"0.00001427", "0.00001412", "0.00001416"},
                               {"0.00001446", "0.00001416", "0.00001436"},
                               {"0.00001470", "0.00001433", "0.00001464"},
                               {"0.00001465", "0.00001429", "0.00001437"},
                               {"0.00001451", "0.00001434", "0.00001444"},
                               {"0.00001466", "0.00001436", "0.00001462"},
                               {"0.00001468", "0.00001446", "0.00001447"},
                               {"0.00001448", "0.00001443", "0.00001444"},
                               {"0.00001455", "0.00001432", "0.00001437"},
                               {"0.00001437", "0.00001420", "0.00001422"},
                               {"0.00001424", "0.00001399", "0.00001399"},
                               {"0.00001399", "0.00001360", "0.00001371"},
                               {"0.00001375", "0.00001352", "0.00001357"}});
        ExpectNear(Ema(bars, 10), "0.00001405");
        ExpectNear(Rsi(bars), "43.13340987");
        ExpectNear(Atr(bars), "0.00000029");
        const auto macd = Macd(bars);
        ASSERT_TRUE(macd);
        ExpectNear(macd->line, "0.00000019");
    }
    {
        // BTC/KRW-like, around 140,000,000 with 1,000 ticks: good to about
        // 1e-7 at nine figures.
        const auto bars = Hlc({{"140139000", "139910000", "139943000"},
                               {"140142000", "139795000", "140021000"},
                               {"140024000", "139668000", "139788000"},
                               {"139929000", "139694000", "139753000"},
                               {"139873000", "139511000", "139649000"},
                               {"140032000", "139548000", "139911000"},
                               {"139970000", "139727000", "139765000"},
                               {"140099000", "139762000", "140000000"},
                               {"140040000", "139755000", "139765000"},
                               {"139780000", "139697000", "139773000"},
                               {"140056000", "139664000", "139957000"},
                               {"140208000", "139844000", "140061000"},
                               {"140154000", "139874000", "139898000"},
                               {"139932000", "139508000", "139634000"},
                               {"139700000", "139445000", "139556000"},
                               {"139671000", "139427000", "139564000"},
                               {"139805000", "139475000", "139659000"},
                               {"140054000", "139555000", "139905000"},
                               {"140262000", "139819000", "140203000"},
                               {"140274000", "139891000", "139932000"},
                               {"140104000", "139786000", "139966000"},
                               {"140274000", "139912000", "140248000"},
                               {"140603000", "140176000", "140535000"},
                               {"140551000", "140239000", "140362000"},
                               {"140579000", "140274000", "140557000"},
                               {"140662000", "140287000", "140325000"},
                               {"140400000", "139936000", "140045000"},
                               {"140200000", "140034000", "140170000"},
                               {"140266000", "139766000", "139916000"},
                               {"140095000", "139845000", "139954000"},
                               {"140231000", "139945000", "140171000"},
                               {"140189000", "140152000", "140188000"},
                               {"140325000", "139990000", "139998000"},
                               {"140102000", "139826000", "139900000"},
                               {"139939000", "139859000", "139869000"},
                               {"139996000", "139777000", "139916000"},
                               {"140012000", "139661000", "139757000"},
                               {"140061000", "139659000", "139928000"},
                               {"140226000", "139799000", "140200000"},
                               {"140310000", "140117000", "140177000"}});
        ExpectNear(Ema(bars, 10), "140033399.89276030", "0.0000001");
        ExpectNear(Rsi(bars), "53.81834567");
        ExpectNear(Atr(bars), "304576.67633517", "0.0000001");
        const auto macd = Macd(bars);
        ASSERT_TRUE(macd);
        ExpectNear(macd->line, "41493.88754215", "0.0000001");
    }
}

TEST(Indicators, RsiLargeMoves) {
    // A 10,000 move (BTC on daily bars) once overflowed the ratio.
    EXPECT_EQ(Rsi(Closes({"50000", "60000"}), 1), D("100"));
    EXPECT_EQ(Rsi(Closes({"60000", "50000"}), 1), D("0"));
    // Gains seeded (10,000 + 0) / 2, losses (0 + 5,000) / 2.
    EXPECT_EQ(Rsi(Closes({"50000", "60000", "55000"}), 2), D("66.66666667"));
    EXPECT_EQ(Rsi(Closes({"100000000", "150000000", "125000000"}), 2),
              D("66.66666667"));
}

TEST(Indicators, PeriodOne) {
    // Period 1 is the newest value alone.
    EXPECT_EQ(Rsi(Closes({"1", "2", "1"}), 1), D("0"));
    EXPECT_EQ(Rsi(Closes({"1", "2"}), 1), D("100"));
    const std::vector<types::Bar> bars = {B("10", "10", "10"),
                                          B("14", "12", "13")};
    EXPECT_EQ(Atr(bars, 1), D("4"));
    const auto bands = Bollinger(bars, 1);
    ASSERT_TRUE(bands);
    EXPECT_EQ(bands->deviation, D("0"));
    EXPECT_EQ(bands->upper, D("13"));
    EXPECT_EQ(bands->lower, D("13"));
}

TEST(Indicators, BollingerHugeDeviation) {
    // Squares of 20,000,000 deviations take the square root's reduced
    // precision: sqrt(2e14) = 14142135.623730950...
    const auto bands = Bollinger(
        Closes({"100000000", "120000000", "80000000", "100000000"}), 4);
    ASSERT_TRUE(bands);
    EXPECT_EQ(bands->deviation, D("14142135.62373095"));
    EXPECT_EQ(bands->upper - bands->middle, D("28284271.2474619"));
}

TEST(Indicators, WindowLimits) {
    const auto bars = Closes({"1", "2", "3", "4", "5", "6", "7", "8"});
    // back equal to what is left over still answers; one more does not.
    EXPECT_TRUE(Rsi(bars, 2, 5));
    EXPECT_FALSE(Rsi(bars, 2, 6));
    EXPECT_TRUE(Bollinger(bars, 4, D("1"), 4));
    EXPECT_FALSE(Bollinger(bars, 4, D("1"), 5));
    // Enough bars for the slow EMA but not for the signal after it.
    EXPECT_FALSE(Macd(Closes({"1", "2", "3", "4"}), 2, 3, 3));
    // Huge periods and backs answer nullopt rather than wrapping.
    constexpr auto kHuge = std::numeric_limits<std::size_t>::max();
    EXPECT_FALSE(Sma(bars, 2, kHuge - 1));
    EXPECT_FALSE(Ema(bars, 2, kHuge - 1));
    EXPECT_FALSE(Rsi(bars, kHuge));
    EXPECT_FALSE(Atr(bars, kHuge, 1));
    EXPECT_FALSE(Bollinger(bars, kHuge));
    EXPECT_FALSE(Macd(bars, 2, 3, kHuge));
    EXPECT_FALSE(Macd(bars, 2, kHuge, 2));
}

}  // namespace
}  // namespace kestrel::strategy
