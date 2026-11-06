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
