#include "kestrel/data/recent_bars.hpp"

#include <gtest/gtest.h>

#include "test_helpers.hpp"

namespace kestrel::data {
namespace {

using types::Timeframe;
using types::Timestamp;

Timestamp Hour(int h) { return types::FromEpochSeconds(h * 3600); }

// Hourly bars at the given hours; counts and records its requests.
class HistorySource : public DataSource {
   public:
    std::string_view Name() const override { return "history"; }
    types::Result<std::vector<types::Bar>> GetBars(const types::Symbol&,
                                                   Timeframe, Timestamp start,
                                                   Timestamp end) override {
        requests.emplace_back(start, end);
        std::vector<types::Bar> out;
        for (int h : hours) {
            if (Hour(h) >= start && Hour(h) < end && Hour(h + 1) <= end) {
                types::Bar bar;
                bar.open_time = Hour(h);
                out.push_back(bar);
            }
        }
        return out;
    }
    types::Result<std::unique_ptr<Subscription>> StreamQuotes(
        const std::vector<std::string>&, QuoteCallback) override {
        return types::MakeError(types::ErrorCategory::kNotImplemented, "no");
    }
    types::Result<types::SymbolInfo> GetSymbolInfo(
        const types::Symbol&) override {
        return types::MakeError(types::ErrorCategory::kNotFound, "no");
    }

    std::vector<int> hours;
    std::vector<std::pair<Timestamp, Timestamp>> requests;
};

std::vector<Timestamp> Opens(const std::vector<types::Bar>& bars) {
    std::vector<Timestamp> out;
    for (const auto& bar : bars) {
        out.push_back(bar.open_time);
    }
    return out;
}

TEST(RecentBars, OneRequestWhenNothingIsClosed) {
    HistorySource source;
    for (int h = 0; h < 100; ++h) {
        source.hours.push_back(h);
    }
    auto bars = RecentBars(source, "X", Timeframe::kH1, 3, Hour(100));
    ASSERT_OK(bars);
    EXPECT_EQ(Opens(*bars),
              (std::vector<Timestamp>{Hour(97), Hour(98), Hour(99)}));
    EXPECT_EQ(source.requests.size(), 1u);
}

TEST(RecentBars, ReachesBackAcrossClosedTime) {
    // Bars from 09:00 to 15:00 (7 a day): the last 10 span two days.
    HistorySource source;
    for (int day = 0; day < 30; ++day) {
        for (int h = 9; h < 16; ++h) {
            source.hours.push_back(day * 24 + h);
        }
    }
    const Timestamp now = Hour(29 * 24 + 20);  // day 29, after the close
    auto bars = RecentBars(source, "X", Timeframe::kH1, 10, now);
    ASSERT_OK(bars);
    ASSERT_EQ(bars->size(), 10u);
    EXPECT_EQ(bars->front().open_time, Hour(28 * 24 + 13));
    EXPECT_EQ(bars->back().open_time, Hour(29 * 24 + 15));
    EXPECT_GT(source.requests.size(), 1u);
}

TEST(RecentBars, StopsWhenTheHistoryRunsOut) {
    // Only 4 bars exist; the span stops growing once over a week brought
    // nothing new, long before two years.
    HistorySource source;
    source.hours = {996, 997, 998, 999};
    auto bars = RecentBars(source, "X", Timeframe::kH1, 50, Hour(1000));
    ASSERT_OK(bars);
    EXPECT_EQ(bars->size(), 4u);
    EXPECT_LE(source.requests.size(), 4u);

    // An unknown symbol: no bars at all, a few requests.
    HistorySource empty;
    auto none = RecentBars(empty, "X", Timeframe::kH1, 50, Hour(1000));
    ASSERT_OK(none);
    EXPECT_TRUE(none->empty());
    EXPECT_LE(empty.requests.size(), 4u);
}

}  // namespace
// Refuses a span reaching past the newest `kept` hours, as Kraken refuses one
// past the 720 bars it keeps.
class KeepsOnly : public HistorySource {
   public:
    types::Result<std::vector<types::Bar>> GetBars(const types::Symbol& s,
                                                   Timeframe tf,
                                                   Timestamp start,
                                                   Timestamp end) override {
        if (start < end - std::chrono::hours(kept)) {
            requests.emplace_back(start, end);
            return types::MakeError(types::ErrorCategory::kUnsupported,
                                    "kept only so many");
        }
        return HistorySource::GetBars(s, tf, start, end);
    }
    int kept = 0;
};

TEST(RecentBars, ARetentionLimitGivesWhatTheSourceKeeps) {
    KeepsOnly source;
    for (int h = 0; h < 100; ++h) {
        source.hours.push_back(h);
    }
    source.kept = 10;
    // Ten asked for, with a padding bar: one span too far, then one that fits.
    auto bars = RecentBars(source, "X", Timeframe::kH1, 10, Hour(100));
    ASSERT_OK(bars);
    EXPECT_EQ(bars->size(), 10u);
    EXPECT_EQ(bars->back().open_time, Hour(99));
}

TEST(RecentBars, ARetentionLimitWellShortOfTheCountIsFoundByBisection) {
    KeepsOnly source;
    for (int h = 0; h < 2000; ++h) {
        source.hours.push_back(h);
    }
    source.kept = 720;
    auto bars = RecentBars(source, "X", Timeframe::kH1, 1000, Hour(2000));
    ASSERT_OK(bars);
    EXPECT_EQ(bars->size(), 720u);  // all the source keeps
    EXPECT_LT(source.requests.size(), 16u);
}

TEST(RecentBars, OtherErrorsStillFail) {
    KeepsOnly source;
    source.kept = 0;  // refuses everything
    EXPECT_FALSE(RecentBars(source, "X", Timeframe::kH1, 3, Hour(100)));
}

}  // namespace kestrel::data
