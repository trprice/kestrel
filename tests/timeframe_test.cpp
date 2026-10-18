#include "kestrel/data/binanceus_data_source.hpp"
#include "kestrel/data/kraken_data_source.hpp"
#include "kestrel/data/schwab_data_source.hpp"
#include "test_helpers.hpp"

namespace {

using namespace kestrel;
using types::Timeframe;

TEST(Timeframe, StringRoundTrip) {
    for (const auto tf : types::AllTimeframes()) {
        auto parsed = types::ParseTimeframe(types::ToString(tf));
        ASSERT_OK(parsed);
        EXPECT_EQ(*parsed, tf);
    }
    EXPECT_FALSE(types::ParseTimeframe("7m").has_value());
}

TEST(Timeframe, Durations) {
    EXPECT_EQ(*types::FixedDuration(Timeframe::kH4), std::chrono::hours(4));
    EXPECT_EQ(*types::FixedDuration(Timeframe::kW1),
              std::chrono::hours(24 * 7));
    EXPECT_FALSE(types::FixedDuration(Timeframe::kMo1).has_value());
}

TEST(KrakenIntervals, SupportedAndNot) {
    EXPECT_EQ(*data::KrakenInterval(Timeframe::kM1), 1);
    EXPECT_EQ(*data::KrakenInterval(Timeframe::kH4), 240);
    EXPECT_EQ(*data::KrakenInterval(Timeframe::kW1), 10080);
    EXPECT_EQ(*data::KrakenInterval(Timeframe::kD15), 21600);
    for (const auto tf : {Timeframe::kM3, Timeframe::kM10, Timeframe::kH2,
                          Timeframe::kH12, Timeframe::kMo1}) {
        auto r = data::KrakenInterval(tf);
        ASSERT_FALSE(r.has_value());
        EXPECT_EQ(r.error().category, types::ErrorCategory::kUnsupported);
    }
}

TEST(BinanceUSIntervals, SupportedAndNot) {
    EXPECT_EQ(*data::BinanceUSInterval(Timeframe::kM3), "3m");
    EXPECT_EQ(*data::BinanceUSInterval(Timeframe::kH12), "12h");
    EXPECT_EQ(*data::BinanceUSInterval(Timeframe::kMo1), "1M");
    for (const auto tf : {Timeframe::kM10, Timeframe::kD15}) {
        EXPECT_FALSE(data::BinanceUSInterval(tf).has_value());
    }
}

TEST(SchwabFrequencies, SupportedAndNot) {
    auto m15 = data::SchwabFrequencyFor(Timeframe::kM15);
    ASSERT_OK(m15);
    EXPECT_EQ(m15->frequency_type, "minute");
    EXPECT_EQ(m15->frequency, 15);
    EXPECT_EQ(data::SchwabFrequencyFor(Timeframe::kW1)->frequency_type,
              "weekly");
    for (const auto tf :
         {Timeframe::kM3, Timeframe::kH1, Timeframe::kH4, Timeframe::kD3}) {
        EXPECT_FALSE(data::SchwabFrequencyFor(tf).has_value());
    }
}

TEST(Time, Iso8601RoundTripAndFractions) {
    auto t = types::ParseIso8601("2023-09-25T10:00:00.123456Z");
    ASSERT_OK(t);
    EXPECT_EQ(types::FormatIso8601(*t), "2023-09-25T10:00:00.123Z");
    EXPECT_EQ(
        types::ToEpochSeconds(*types::ParseIso8601("1970-01-02T00:00:00Z")),
        86400);
    EXPECT_EQ(
        types::ToEpochMillis(*types::ParseIso8601("2024-02-29T12:34:56.5Z")),
        1709210096500);
    for (const char* bad : {"", "2023-09-25", "2023-13-01T00:00:00Z",
                            "2023-09-25T10:00:00", "garbage"}) {
        EXPECT_FALSE(types::ParseIso8601(bad).has_value()) << bad;
    }
}

}  // namespace
