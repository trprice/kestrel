#include "kestrel/data/feed_watch.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>

namespace kestrel::data {
namespace {

using namespace std::chrono;

types::Timestamp At(sys_days day, minutes m) {
    return types::Timestamp(day + m);
}

TEST(FeedWatch, ReportsAnOutageOnceAndHowLongItLasted) {
    FeedWatch watch(minutes(5), nullptr);  // always open
    const types::Timestamp t0(sys_days(2026y / 1 / 1));
    EXPECT_FALSE(watch.Check(t0 + hours(1)));  // not started
    watch.Start(t0);
    EXPECT_FALSE(watch.Check(t0 + minutes(4)));
    EXPECT_EQ(watch.Check(t0 + minutes(5)), minutes(5));
    EXPECT_FALSE(watch.Check(t0 + minutes(9)));  // reported already
    EXPECT_EQ(watch.Note(t0 + minutes(9)), minutes(9));
    EXPECT_FALSE(watch.Note(t0 + minutes(10)));  // was not down
    EXPECT_FALSE(watch.Check(t0 + minutes(14)));
    EXPECT_EQ(watch.Check(t0 + minutes(15)), minutes(5));
}

TEST(FeedWatch, CountsOnlyOpenTime) {
    FeedWatch watch(minutes(5),
                    std::make_shared<types::MarketHours>(
                        std::make_shared<types::UsEquityCalendar>(), false));
    // Friday Dec 6 2024 at 20:59 UTC, a minute before the close; Monday's
    // session opens at 14:30.
    watch.Start(At(2024y / December / 6, hours(20) + minutes(59)));
    const sys_days monday = 2024y / December / 9;
    EXPECT_FALSE(watch.Check(At(monday, hours(14) + minutes(33))));
    EXPECT_EQ(watch.Check(At(monday, hours(14) + minutes(34))), minutes(5));
}

TEST(FeedWatch, AZeroThresholdIsOff) {
    FeedWatch watch(milliseconds::zero(), nullptr);
    const types::Timestamp t0(sys_days(2026y / 1 / 1));
    watch.Start(t0);
    EXPECT_FALSE(watch.Check(t0 + hours(24)));
}

}  // namespace
}  // namespace kestrel::data
