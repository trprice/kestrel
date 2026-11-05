#include "kestrel/types/market_hours.hpp"

#include <gtest/gtest.h>

#include <set>

namespace kestrel::types {
namespace {

using namespace std::chrono;

Timestamp At(const char* iso) { return *ParseIso8601(iso); }

// The US equity holidays and early closes NYSE published for 2024 to 2027,
// against the rules.
TEST(UsEquityCalendar, MatchesThePublishedHolidaysAndEarlyCloses) {
    const std::set<year_month_day> holidays = {
        2024y / January / 1,   2024y / January / 15,  2024y / February / 19,
        2024y / March / 29,    2024y / May / 27,      2024y / June / 19,
        2024y / July / 4,      2024y / September / 2, 2024y / November / 28,
        2024y / December / 25, 2025y / January / 1,   2025y / January / 9,
        2025y / January / 20,  2025y / February / 17, 2025y / April / 18,
        2025y / May / 26,      2025y / June / 19,     2025y / July / 4,
        2025y / September / 1, 2025y / November / 27, 2025y / December / 25,
        2026y / January / 1,   2026y / January / 19,  2026y / February / 16,
        2026y / April / 3,     2026y / May / 25,      2026y / June / 19,
        2026y / July / 3,      2026y / September / 7, 2026y / November / 26,
        2026y / December / 25, 2027y / January / 1,   2027y / January / 18,
        2027y / February / 15, 2027y / March / 26,    2027y / May / 31,
        2027y / June / 18,     2027y / July / 5,      2027y / September / 6,
        2027y / November / 25, 2027y / December / 24,
    };
    const std::set<year_month_day> early = {
        2024y / July / 3,      2024y / November / 29, 2024y / December / 24,
        2025y / July / 3,      2025y / November / 28, 2025y / December / 24,
        2026y / November / 27, 2026y / December / 24, 2027y / November / 26,
    };
    for (sys_days d{2024y / January / 1}; d <= sys_days{2027y / December / 31};
         d += days(1)) {
        const year_month_day date{d};
        EXPECT_EQ(UsEquityCalendar::IsHoliday(date), holidays.contains(date))
            << date;
        EXPECT_EQ(UsEquityCalendar::IsEarlyClose(date), early.contains(date))
            << date;
    }
}

TEST(UsEquityCalendar, NewYearsDayOnASaturdayIsNotMadeUp) {
    // 2022-01-01 was a Saturday: Friday 2021-12-31 traded.
    EXPECT_FALSE(UsEquityCalendar::IsHoliday(2021y / December / 31));
    // 2023-01-01 was a Sunday: Monday the 2nd was the holiday.
    EXPECT_TRUE(UsEquityCalendar::IsHoliday(2023y / January / 2));
    // No Juneteenth before 2022.
    EXPECT_FALSE(UsEquityCalendar::IsHoliday(2021y / June / 18));
}

TEST(UsEquityCalendar, SessionTimesInNewYorkAcrossDaylightSaving) {
    const UsEquityCalendar calendar;
    // November: EST, UTC-5.
    const auto winter = calendar.On(2026y / November / 5);
    ASSERT_TRUE(winter);
    EXPECT_EQ(winter->pre_open, At("2026-11-05T12:00:00Z"));
    EXPECT_EQ(winter->open, At("2026-11-05T14:30:00Z"));
    EXPECT_EQ(winter->close, At("2026-11-05T21:00:00Z"));
    EXPECT_EQ(winter->post_close, At("2026-11-06T01:00:00Z"));
    // July: EDT, UTC-4.
    const auto summer = calendar.On(2026y / July / 1);
    ASSERT_TRUE(summer);
    EXPECT_EQ(summer->open, At("2026-07-01T13:30:00Z"));
    EXPECT_EQ(summer->close, At("2026-07-01T20:00:00Z"));
    // The day after Thanksgiving closes at 13:00, after hours at 17:00.
    const auto early = calendar.On(2026y / November / 27);
    ASSERT_TRUE(early);
    EXPECT_EQ(early->close, At("2026-11-27T18:00:00Z"));
    EXPECT_EQ(early->post_close, At("2026-11-27T22:00:00Z"));
    EXPECT_FALSE(calendar.On(2026y / November / 26));  // Thanksgiving
    EXPECT_FALSE(calendar.On(2026y / November / 7));   // a Saturday
    // The date is New York's: 01:00 UTC on the 6th is still the 5th there.
    EXPECT_EQ(calendar.DateAt(At("2026-11-06T01:00:00Z")),
              2026y / November / 5);
}

TEST(MarketHours, RegularOrExtendedSessions) {
    const auto calendar = std::make_shared<UsEquityCalendar>();
    const MarketHours regular(calendar, false);
    const MarketHours extended(calendar, true);
    EXPECT_FALSE(regular.IsAlwaysOpen());
    EXPECT_FALSE(regular.IsOpen(At("2026-11-05T14:29:59Z")));
    EXPECT_TRUE(regular.IsOpen(At("2026-11-05T14:30:00Z")));
    EXPECT_TRUE(regular.IsOpen(At("2026-11-05T20:59:59Z")));
    EXPECT_FALSE(regular.IsOpen(At("2026-11-05T21:00:00Z")));
    EXPECT_TRUE(extended.IsOpen(At("2026-11-05T12:00:00Z")));
    EXPECT_TRUE(extended.IsOpen(At("2026-11-06T00:59:00Z")));
    EXPECT_FALSE(extended.IsOpen(At("2026-11-06T01:00:00Z")));
    EXPECT_FALSE(regular.IsOpen(At("2026-11-26T15:00:00Z")));  // Thanksgiving
    // 13:00 on the early close.
    EXPECT_FALSE(regular.IsOpen(At("2026-11-27T18:00:00Z")));
    EXPECT_TRUE(extended.IsOpen(At("2026-11-27T18:00:00Z")));
}

TEST(MarketHours, CurrentOrNextSessionAndOpenTimeInARange) {
    const MarketHours regular(std::make_shared<UsEquityCalendar>(), false);
    // Friday evening: the next session is Monday's.
    const auto next = regular.CurrentOrNext(At("2026-11-06T22:00:00Z"));
    ASSERT_TRUE(next);
    EXPECT_EQ(next->start, At("2026-11-09T14:30:00Z"));
    EXPECT_EQ(next->end, At("2026-11-09T21:00:00Z"));
    // In a session: that session.
    const auto current = regular.CurrentOrNext(At("2026-11-09T15:00:00Z"));
    ASSERT_TRUE(current);
    EXPECT_EQ(current->start, At("2026-11-09T14:30:00Z"));
    // Wednesday's close to Friday's open skips Thanksgiving.
    const auto after = regular.CurrentOrNext(At("2026-11-25T21:00:00Z"));
    ASSERT_TRUE(after);
    EXPECT_EQ(after->start, At("2026-11-27T14:30:00Z"));

    // The weekend has no open time; a range reaching Monday's open does.
    EXPECT_FALSE(regular.AnyOpen(At("2026-11-06T21:00:00Z"),
                                 At("2026-11-09T14:30:00Z")));
    EXPECT_TRUE(regular.AnyOpen(At("2026-11-06T21:00:00Z"),
                                At("2026-11-09T14:31:00Z")));
    EXPECT_TRUE(regular.AnyOpen(At("2026-11-09T15:00:00Z"),
                                At("2026-11-09T15:01:00Z")));
}

TEST(MarketHours, AlwaysOpen) {
    const auto always = MarketHours::AlwaysOpen();
    EXPECT_TRUE(always->IsAlwaysOpen());
    EXPECT_TRUE(always->IsOpen(At("2026-11-07T03:00:00Z")));
    EXPECT_FALSE(always->CurrentOrNext(At("2026-11-07T03:00:00Z")));
    EXPECT_TRUE(always->AnyOpen(At("2026-11-07T03:00:00Z"),
                                At("2026-11-07T03:01:00Z")));
}

}  // namespace
}  // namespace kestrel::types
