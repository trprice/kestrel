#include "kestrel/data/schwab_data_source.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <functional>

#include "kestrel/types/market_hours.hpp"
#include "test_helpers.hpp"

namespace kestrel::data {
namespace {

using namespace std::chrono_literals;
using types::Timeframe;

types::Timestamp At(const char* iso) { return *types::ParseIso8601(iso); }
std::string Ms(const char* iso) {
    return std::to_string(types::ToEpochMillis(At(iso)));
}

// Answers every request with `reply` and keeps the requests.
class FakeHttp : public net::HttpClient {
   public:
    std::function<types::Result<net::HttpResponse>(const net::HttpRequest&)>
        reply;
    std::vector<net::HttpRequest> requests;

    types::Result<net::HttpResponse> Send(const net::HttpRequest& r) override {
        requests.push_back(r);
        return reply(r);
    }
};

// A candle as Schwab sends it, opening at `iso`.
std::string Candle(const char* iso, const char* close = "100.5") {
    return R"({"open":100,"high":101.25,"low":99.5,"close":)" +
           std::string(close) + R"(,"volume":1200,"datetime":)" + Ms(iso) + "}";
}

std::string Candles(const std::vector<std::string>& candles) {
    std::string body = R"({"symbol":"SPY","empty":false,"candles":[)";
    for (std::size_t i = 0; i < candles.size(); ++i) {
        body += (i ? "," : "") + candles[i];
    }
    return body + "]}";
}

class SchwabDataSourceTest : public ::testing::Test {
   protected:
    void SetUp() override {
        mDir = std::filesystem::temp_directory_path() /
               ("kestrel-schwab-data-" +
                std::string(::testing::UnitTest::GetInstance()
                                ->current_test_info()
                                ->name()));
        std::filesystem::remove_all(mDir);
        std::filesystem::create_directories(mDir);
        const config::SchwabTokenStore store(mDir / "tokens.json");
        const auto now = types::Now();
        ASSERT_OK(store.Save({"access", "refresh", now + 1h, now + 72h}));
        mAuth = std::make_shared<SchwabAuth>(
            config::SchwabCredentials{"key", "secret"}, store,
            std::make_shared<FakeHttp>(), "https://127.0.0.1:8182");
        ASSERT_OK(mAuth->Load());
        mHttp = std::make_shared<FakeHttp>();
        mHttp->reply = [](const net::HttpRequest&) {
            return types::Result<net::HttpResponse>(
                net::HttpResponse{200, Candles({}), {}});
        };
    }
    void TearDown() override { std::filesystem::remove_all(mDir); }

    SchwabDataSource Source(bool extended_hours = false) {
        SchwabDataSource::Options options;
        options.extended_hours = extended_hours;
        options.base_url = "https://api.test/marketdata/v1";
        options.clock = [this] { return mNow; };
        options.sleeper = [this](std::chrono::milliseconds d) {
            mSlept.push_back(d);
        };
        return SchwabDataSource(
            std::make_shared<SchwabApi>(mAuth, mHttp,
                                        SchwabApi::Options{1000, 1000}),
            std::move(options));
    }
    void Reply(std::string body) {
        mHttp->reply = [body](const net::HttpRequest&) {
            return types::Result<net::HttpResponse>(
                net::HttpResponse{200, body, {}});
        };
    }

    std::filesystem::path mDir;
    std::shared_ptr<SchwabAuth> mAuth;
    std::shared_ptr<FakeHttp> mHttp;
    types::Timestamp mNow = At("2026-11-04T01:00:00Z");
    std::vector<std::chrono::milliseconds> mSlept;
};

types::Result<net::HttpResponse> Status(int status, std::string body = "") {
    return std::unexpected(net::ErrorFromHttpStatus(
        net::HttpResponse{status, std::move(body), {}}));
}

TEST_F(SchwabDataSourceTest, TrimsTheDaySchwabSendsToTheRange) {
    // Schwab answers a few minutes with the whole day.
    Reply(Candles(
        {Candle("2026-11-03T14:30:00Z"), Candle("2026-11-03T14:31:00Z"),
         Candle("2026-11-03T14:32:00Z"), Candle("2026-11-03T14:33:00Z")}));
    auto bars =
        Source().GetBars("SPY", Timeframe::kM1, At("2026-11-03T14:31:00Z"),
                         At("2026-11-03T14:33:00Z"));
    ASSERT_OK(bars);
    ASSERT_EQ(bars->size(), 2u);
    EXPECT_EQ((*bars)[0].open_time, At("2026-11-03T14:31:00Z"));
    EXPECT_EQ((*bars)[0].high, types::Decimal(10125, 2));
    EXPECT_EQ((*bars)[0].close, types::Decimal(1005, 1));
    EXPECT_EQ((*bars)[0].volume, types::Decimal::FromInt(1200));
    EXPECT_EQ((*bars)[1].open_time, At("2026-11-03T14:32:00Z"));

    ASSERT_EQ(mHttp->requests.size(), 1u);
    const auto& r = mHttp->requests[0];
    EXPECT_EQ(r.url, "https://api.test/marketdata/v1/pricehistory");
    EXPECT_EQ(r.query.at("symbol"), "SPY");
    EXPECT_EQ(r.query.at("periodType"), "day");
    EXPECT_EQ(r.query.at("frequencyType"), "minute");
    EXPECT_EQ(r.query.at("frequency"), "1");
    // A day further either side.
    EXPECT_EQ(r.query.at("startDate"), Ms("2026-11-02T14:31:00Z"));
    EXPECT_EQ(r.query.at("endDate"), Ms("2026-11-04T14:33:00Z"));
    // Schwab's default is true, so it is always sent.
    EXPECT_EQ(r.query.at("needExtendedHoursData"), "false");
    EXPECT_EQ(r.headers.at("Authorization"), "Bearer access");
}

TEST_F(SchwabDataSourceTest, AsksForExtendedHoursWhenSet) {
    ASSERT_OK(Source(true).GetBars("SPY", Timeframe::kM5,
                                   At("2026-11-03T00:00:00Z"),
                                   At("2026-11-04T00:00:00Z")));
    ASSERT_EQ(mHttp->requests.size(), 1u);
    EXPECT_EQ(mHttp->requests[0].query.at("needExtendedHoursData"), "true");
    EXPECT_EQ(mHttp->requests[0].query.at("frequency"), "5");
}

TEST_F(SchwabDataSourceTest, PagesMinuteBarsTenDaysAtATime) {
    // Every page also carries a bar from the next page's days, as the padded
    // requests overlap; each bar is kept once.
    Reply(
        Candles({Candle("2026-10-05T14:30:00Z"), Candle("2026-10-16T14:30:00Z"),
                 Candle("2026-10-26T14:30:00Z")}));
    auto bars =
        Source().GetBars("SPY", Timeframe::kM1, At("2026-10-05T00:00:00Z"),
                         At("2026-10-30T00:00:00Z"));
    ASSERT_OK(bars);
    ASSERT_EQ(mHttp->requests.size(), 3u);
    EXPECT_EQ(mHttp->requests[0].query.at("startDate"),
              Ms("2026-10-04T00:00:00Z"));
    EXPECT_EQ(mHttp->requests[0].query.at("endDate"),
              Ms("2026-10-16T00:00:00Z"));
    EXPECT_EQ(mHttp->requests[1].query.at("startDate"),
              Ms("2026-10-14T00:00:00Z"));
    EXPECT_EQ(mHttp->requests[2].query.at("endDate"),
              Ms("2026-10-31T00:00:00Z"));
    ASSERT_EQ(bars->size(), 3u);
    EXPECT_EQ((*bars)[0].open_time, At("2026-10-05T14:30:00Z"));
    EXPECT_EQ((*bars)[2].open_time, At("2026-10-26T14:30:00Z"));
}

TEST_F(SchwabDataSourceTest,
       DailyBarsOpenAtMidnightUtcAndTheFormingOneIsDropped) {
    // Schwab stamps them midnight Central: 06:00 UTC in winter, 05:00 in
    // summer (and 05:00 for the newest).
    mNow = At("2026-11-03T18:00:00Z");  // Tuesday's session is open
    Reply(
        Candles({Candle("2026-10-30T05:00:00Z"), Candle("2026-11-02T06:00:00Z"),
                 Candle("2026-11-03T05:00:00Z")}));
    auto bars =
        Source().GetBars("SPY", Timeframe::kD1, At("2026-10-30T00:00:00Z"),
                         At("2026-11-04T00:00:00Z"));
    ASSERT_OK(bars);
    ASSERT_EQ(bars->size(), 2u);
    EXPECT_EQ((*bars)[0].open_time, At("2026-10-30T00:00:00Z"));
    EXPECT_EQ((*bars)[1].open_time, At("2026-11-02T00:00:00Z"));
    ASSERT_EQ(mHttp->requests.size(), 1u);
    EXPECT_EQ(mHttp->requests[0].query.at("periodType"), "year");
    EXPECT_EQ(mHttp->requests[0].query.at("frequencyType"), "daily");
    EXPECT_FALSE(mHttp->requests[0].query.contains("needExtendedHoursData"));
}

TEST_F(SchwabDataSourceTest, AMonthlyBarClosesAtTheNextMonth) {
    mNow = At("2026-11-03T18:00:00Z");
    Reply(Candles(
        {Candle("2026-10-01T05:00:00Z"), Candle("2026-11-01T05:00:00Z")}));
    auto bars =
        Source().GetBars("SPY", Timeframe::kMo1, At("2026-01-01T00:00:00Z"),
                         At("2026-12-01T00:00:00Z"));
    ASSERT_OK(bars);
    ASSERT_EQ(bars->size(), 1u);  // November is still forming
    EXPECT_EQ((*bars)[0].open_time, At("2026-10-01T00:00:00Z"));
    EXPECT_EQ(mHttp->requests[0].query.at("frequencyType"), "monthly");
}

TEST_F(SchwabDataSourceTest, ReadsPricesExactlyAndMapsShareClasses) {
    Reply(Candles({Candle("2026-11-02T06:00:00Z", "559.5779")}));
    auto bars =
        Source().GetBars("BRK.B", Timeframe::kD1, At("2026-11-02T00:00:00Z"),
                         At("2026-11-03T00:00:00Z"));
    ASSERT_OK(bars);
    ASSERT_EQ(bars->size(), 1u);
    EXPECT_EQ((*bars)[0].close, types::Decimal(5595779, 4));
    EXPECT_EQ(mHttp->requests[0].query.at("symbol"), "BRK/B");
}

TEST_F(SchwabDataSourceTest, AnEmptyAnswerIsNoBars) {
    // What Schwab sends for an unknown symbol, or days before its history.
    Reply(R"({"symbol":"NOSUCH","empty":true,"candles":[]})");
    auto bars =
        Source().GetBars("NOSUCH", Timeframe::kM1, At("2026-11-03T14:30:00Z"),
                         At("2026-11-03T15:00:00Z"));
    ASSERT_OK(bars);
    EXPECT_TRUE(bars->empty());
}

TEST_F(SchwabDataSourceTest, ReportsErrorsAndMalformedAnswers) {
    mHttp->reply = [](const net::HttpRequest&) {
        return types::Result<net::HttpResponse>(
            std::unexpected(net::ErrorFromHttpStatus(net::HttpResponse{
                400,
                R"({"errors":[{"status":"400","title":"Bad Request",)"
                R"("detail":"When periodType=day valid values for )"
                R"(frequencyType are: minute"}]})",
                {}})));
    };
    auto refused =
        Source().GetBars("SPY", Timeframe::kM1, At("2026-11-03T14:30:00Z"),
                         At("2026-11-03T15:00:00Z"));
    ASSERT_FALSE(refused.has_value());
    EXPECT_NE(refused.error().message.find("schwab pricehistory SPY"),
              std::string::npos);
    EXPECT_EQ(refused.error().http_status, 400);

    for (
        const char* body :
        {"[]", R"({"candles":{}})",
         R"({"candles":[{"open":1,"high":1,"low":1,"close":1,"volume":1}]})",
         R"({"candles":[{"open":1,"high":1,"low":1,"volume":1,"datetime":1793629800000}]})"}) {
        Reply(body);
        auto bad =
            Source().GetBars("SPY", Timeframe::kM1, At("2026-11-03T14:30:00Z"),
                             At("2026-11-03T15:00:00Z"));
        ASSERT_FALSE(bad.has_value()) << body;
        EXPECT_EQ(bad.error().category, types::ErrorCategory::kParse) << body;
    }
}

TEST_F(SchwabDataSourceTest, RetriesAPageThatFailsTransiently) {
    // Two pages of minute bars; the second fails twice (429, then 503).
    int calls = 0;
    mHttp->reply = [&calls](const net::HttpRequest&) {
        ++calls;
        if (calls == 2) {
            auto limited =
                net::ErrorFromHttpStatus(net::HttpResponse{429, "", {}});
            limited.retry_after = std::chrono::seconds(3);
            return types::Result<net::HttpResponse>(std::unexpected(limited));
        }
        if (calls == 3) {
            return Status(503);
        }
        return types::Result<net::HttpResponse>(net::HttpResponse{
            200, Candles({Candle("2026-10-20T14:30:00Z")}), {}});
    };
    auto bars =
        Source().GetBars("SPY", Timeframe::kM1, At("2026-10-15T00:00:00Z"),
                         At("2026-11-03T00:00:00Z"));
    ASSERT_OK(bars);
    EXPECT_EQ(bars->size(), 1u);
    EXPECT_EQ(calls, 4);
    ASSERT_EQ(mSlept.size(), 2u);
    EXPECT_GE(mSlept[0], std::chrono::seconds(3));  // Retry-After honoured
}

TEST_F(SchwabDataSourceTest,
       GivesUpAfterTheLastAttemptAndNeverRetriesARefusal) {
    int calls = 0;
    mHttp->reply = [&calls](const net::HttpRequest&) {
        ++calls;
        return Status(500);
    };
    auto failed =
        Source().GetBars("SPY", Timeframe::kM1, At("2026-11-03T14:30:00Z"),
                         At("2026-11-03T15:00:00Z"));
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(calls, util::RetryPolicy{}.max_attempts);

    calls = 0;
    mHttp->reply = [&calls](const net::HttpRequest&) {
        ++calls;
        return Status(400);
    };
    EXPECT_FALSE(Source().GetBars("SPY", Timeframe::kM1,
                                  At("2026-11-03T14:30:00Z"),
                                  At("2026-11-03T15:00:00Z")));
    EXPECT_EQ(calls, 1);
}

// As /markets answered on Nov 5 2026.
constexpr const char* kOpenDay =
    R"({"equity":{"EQ":{"date":"2026-11-05","marketType":"EQUITY",)"
    R"("product":"EQ","productName":"equity","isOpen":true,"sessionHours":)"
    R"({"preMarket":[{"start":"2026-11-05T07:00:00-05:00","end":)"
    R"("2026-11-05T09:30:00-05:00"}],"regularMarket":[{"start":)"
    R"("2026-11-05T09:30:00-05:00","end":"2026-11-05T16:00:00-05:00"}],)"
    R"("postMarket":[{"start":"2026-11-05T16:00:00-05:00","end":)"
    R"("2026-11-05T20:00:00-05:00"}]}}}})";
constexpr const char* kClosedDay =
    R"({"equity":{"equity":{"date":"2026-11-26","marketType":"EQUITY",)"
    R"("product":"equity","isOpen":false}}})";

TEST(SchwabCalendar, ParsesTheMarketHours) {
    auto open = SchwabCalendar::ParseMarketHours(kOpenDay);
    ASSERT_OK(open);
    ASSERT_TRUE(*open);
    EXPECT_EQ((*open)->pre_open, At("2026-11-05T12:00:00Z"));
    EXPECT_EQ((*open)->open, At("2026-11-05T14:30:00Z"));
    EXPECT_EQ((*open)->close, At("2026-11-05T21:00:00Z"));
    EXPECT_EQ((*open)->post_close, At("2026-11-06T01:00:00Z"));
    // The same as the rules have it.
    EXPECT_EQ(**open, *types::UsEquityCalendar().On(std::chrono::year_month_day{
                          std::chrono::year{2026}, std::chrono::November,
                          std::chrono::day{5}}));

    auto closed = SchwabCalendar::ParseMarketHours(kClosedDay);
    ASSERT_OK(closed);
    EXPECT_FALSE(*closed);

    for (const char* bad :
         {"[]", R"({"option":{}})", R"({"equity":{"EQ":{"isOpen":"yes"}}})",
          R"({"equity":{"EQ":{"isOpen":true,"sessionHours":{}}}})"}) {
        EXPECT_FALSE(SchwabCalendar::ParseMarketHours(bad)) << bad;
    }
}

class SchwabCalendarTest : public SchwabDataSourceTest {
   protected:
    // Answers each /markets request for its own date: open from 09:30 to
    // `close` (New York, EST), or closed.
    void ReplyOpen(const std::string& close = "16:00") {
        mHttp->reply = [close](const net::HttpRequest& r) {
            const std::string d = r.query.at("date");
            const std::string body =
                R"({"equity":{"EQ":{"isOpen":true,"sessionHours":{)"
                R"("regularMarket":[{"start":")" +
                d + R"(T09:30:00-05:00","end":")" + d + "T" + close +
                R"(:00-05:00"}]}}}})";
            return types::Result<net::HttpResponse>(
                net::HttpResponse{200, body, {}});
        };
    }
    SchwabCalendar Calendar() {
        return SchwabCalendar(std::make_shared<SchwabApi>(
                                  mAuth, mHttp, SchwabApi::Options{1000, 1000}),
                              "https://api.test/marketdata/v1",
                              [this] { return mNow; });
    }
    using year_month_day = std::chrono::year_month_day;
    static std::chrono::year_month_day Date(int y, unsigned m, unsigned d) {
        return std::chrono::year{y} / std::chrono::month{m} /
               std::chrono::day{d};
    }
};

// The calendar answers from the rules at once and asks Schwab in the
// background; WaitForRequests lets a test see the answer land. (The fakes
// are only touched while the calendar's thread is idle.)
TEST_F(SchwabCalendarTest, AsksSchwabOncePerDateInItsWindowInTheBackground) {
    mNow = At("2026-11-05T15:00:00Z");
    Reply(kOpenDay);
    const auto calendar = Calendar();
    ASSERT_TRUE(calendar.On(Date(2026, 11, 5)));  // the rules, at once
    calendar.WaitForRequests();
    ASSERT_TRUE(calendar.On(Date(2026, 11, 5)));  // Schwab's, remembered
    calendar.WaitForRequests();
    ASSERT_EQ(mHttp->requests.size(), 1u);
    EXPECT_EQ(mHttp->requests[0].url, "https://api.test/marketdata/v1/markets");
    EXPECT_EQ(mHttp->requests[0].query.at("markets"), "equity");
    EXPECT_EQ(mHttp->requests[0].query.at("date"), "2026-11-05");

    // The window's edges: 6 days back and 364 ahead are asked about, 7 back
    // and 365 ahead are not.
    ReplyOpen();  // each date's own hours
    for (const auto& date : {Date(2026, 10, 30), Date(2027, 11, 4)}) {
        ASSERT_TRUE(calendar.On(date)) << date;  // trading days by the rules
    }
    calendar.WaitForRequests();
    EXPECT_EQ(mHttp->requests.size(), 3u);
    EXPECT_TRUE(calendar.On(Date(2026, 10, 29)));
    EXPECT_TRUE(calendar.On(Date(2027, 11, 5)));
    calendar.WaitForRequests();
    EXPECT_EQ(mHttp->requests.size(), 3u);
}

TEST_F(SchwabCalendarTest, NeverAsksAboutADayTheRulesClose) {
    mNow = At("2026-11-05T15:00:00Z");
    ReplyOpen();
    const auto calendar = Calendar();
    EXPECT_FALSE(calendar.On(Date(2026, 11, 7)));   // a Saturday
    EXPECT_FALSE(calendar.On(Date(2026, 11, 26)));  // Thanksgiving
    EXPECT_FALSE(calendar.On(Date(2027, 1, 1)));    // Schwab: "open"
    calendar.WaitForRequests();
    EXPECT_TRUE(mHttp->requests.empty());
}

TEST_F(SchwabCalendarTest, SchwabCanOnlyRestrictADay) {
    mNow = At("2026-11-05T15:00:00Z");
    const auto calendar = Calendar();
    const auto settled = [&](year_month_day date) {
        (void)calendar.On(date);
        calendar.WaitForRequests();
        return calendar.On(date);
    };
    // A day the rules say trades, which Schwab says is closed (an
    // unscheduled closure): closed.
    Reply(kClosedDay);
    EXPECT_FALSE(settled(Date(2026, 11, 10)));
    // An early close Schwab does not know of stays early.
    ReplyOpen();
    const auto early = settled(Date(2026, 12, 24));
    ASSERT_TRUE(early);
    EXPECT_EQ(early->close, At("2026-12-24T18:00:00Z"));
    // An early close only Schwab knows of is followed.
    ReplyOpen("12:00");
    const auto shortened = settled(Date(2026, 12, 1));
    ASSERT_TRUE(shortened);
    EXPECT_EQ(shortened->close, At("2026-12-01T17:00:00Z"));
    // So is a delayed open, each time the more restrictive one: Schwab's
    // 11:00 open, and with no pre- or after-market in its answer, none.
    mHttp->reply = [](const net::HttpRequest& r) {
        const std::string d = r.query.at("date");
        return types::Result<net::HttpResponse>(net::HttpResponse{
            200,
            R"({"equity":{"EQ":{"isOpen":true,"sessionHours":{)"
            R"("regularMarket":[{"start":")" +
                d + R"(T11:00:00-05:00","end":")" + d +
                R"(T16:00:00-05:00"}]}}}})",
            {}});
    };
    const auto delayed = settled(Date(2026, 12, 2));
    ASSERT_TRUE(delayed);
    EXPECT_EQ(delayed->pre_open, At("2026-12-02T16:00:00Z"));
    EXPECT_EQ(delayed->open, At("2026-12-02T16:00:00Z"));
    EXPECT_EQ(delayed->close, At("2026-12-02T21:00:00Z"));
    EXPECT_EQ(delayed->post_close, At("2026-12-02T21:00:00Z"));
}

TEST_F(SchwabCalendarTest, HoursForAnotherDateAreNotUsed) {
    mNow = At("2026-11-05T15:00:00Z");
    Reply(kOpenDay);  // Nov 5's hours, whatever is asked
    const auto calendar = Calendar();
    (void)calendar.On(Date(2026, 11, 10));
    calendar.WaitForRequests();
    const auto day = calendar.On(Date(2026, 11, 10));
    ASSERT_TRUE(day);  // the rules
    EXPECT_EQ(day->open, At("2026-11-10T14:30:00Z"));
}

TEST_F(SchwabCalendarTest, FallsBackToTheRulesAndRetriesLater) {
    mNow = At("2026-11-05T15:00:00Z");
    mHttp->reply = [](const net::HttpRequest&) {
        return types::Result<net::HttpResponse>(std::unexpected(
            net::ErrorFromHttpStatus(net::HttpResponse{503, "", {}})));
    };
    const auto calendar = Calendar();
    EXPECT_TRUE(calendar.On(Date(2026, 11, 6)));  // the rules: a Friday
    calendar.WaitForRequests();
    EXPECT_TRUE(calendar.On(Date(2026, 11, 6)));  // failed: still the rules
    EXPECT_TRUE(calendar.On(Date(2026, 11, 9)));
    calendar.WaitForRequests();
    EXPECT_EQ(mHttp->requests.size(), 1u);  // not asked again for 10 minutes

    mNow += std::chrono::minutes(11);
    Reply(kClosedDay);
    (void)calendar.On(Date(2026, 11, 6));
    calendar.WaitForRequests();
    EXPECT_FALSE(calendar.On(Date(2026, 11, 6)));  // asked again: Schwab's
    EXPECT_EQ(mHttp->requests.size(), 2u);
}

TEST(SchwabCalendar, AMalformedSessionTimeIsAnError) {
    // Absent pre- and after-market sessions are fine; malformed ones are not.
    for (const char* bad :
         {R"({"equity":{"EQ":{"isOpen":true,"sessionHours":{"regularMarket":)"
          R"([{"start":"2026-11-05T09:30:00-05:00","end":)"
          R"("2026-11-05T16:00:00-05:00"}],"preMarket":[{"start":"7am"}]}}}})",
          R"({"equity":{"EQ":{"isOpen":true,"sessionHours":{"regularMarket":)"
          R"([{"start":"2026-11-05T09:30:00-05:00","end":)"
          R"("2026-11-05T16:00:00-05:00"}],"postMarket":[]}}}})"}) {
        EXPECT_FALSE(SchwabCalendar::ParseMarketHours(bad)) << bad;
    }
}

TEST_F(SchwabDataSourceTest, HoursFollowExtendedHours) {
    mHttp->reply = [](const net::HttpRequest& r) {
        return types::Result<net::HttpResponse>(net::HttpResponse{
            200, r.url.ends_with("/markets") ? kOpenDay : Candles({}), {}});
    };
    const auto regular = Source().Hours();
    EXPECT_FALSE(regular->IsAlwaysOpen());
    EXPECT_FALSE(regular->Extended());
    EXPECT_TRUE(Source(true).Hours()->Extended());
}

TEST_F(SchwabDataSourceTest, RefusesTimeframesSchwabLacksAndBadRanges) {
    EXPECT_FALSE(Source()
                     .GetBars("SPY", Timeframe::kH1, At("2026-11-03T00:00:00Z"),
                              At("2026-11-04T00:00:00Z"))
                     .has_value());
    EXPECT_FALSE(Source()
                     .GetBars("SPY", Timeframe::kM1, At("2026-11-04T00:00:00Z"),
                              At("2026-11-03T00:00:00Z"))
                     .has_value());
    EXPECT_FALSE(Source()
                     .GetBars("BTC/USD", Timeframe::kM1,
                              At("2026-11-03T00:00:00Z"),
                              At("2026-11-04T00:00:00Z"))
                     .has_value());
    EXPECT_TRUE(mHttp->requests.empty());
}

// /quotes as Schwab answered on Nov 5 2026 (cut down).
std::string QuoteBody(const std::string& key, const std::string& type,
                      const std::string& last) {
    return R"({")" + key + R"(":{"assetMainType":")" + type +
           R"(","assetSubType":"COE","symbol":")" + key +
           R"(","quote":{"lastPrice":)" + last +
           R"(,"bidPrice":1.32,"askPrice":1.36}}})";
}

TEST_F(SchwabDataSourceTest, SymbolInfoChecksTheSymbolWithAQuote) {
    Reply(QuoteBody("BRK/B", "EQUITY", "570.5"));
    auto source = Source();
    auto info = source.GetSymbolInfo("brk.b");
    ASSERT_OK(info);
    EXPECT_EQ(info->symbol, "BRK.B");
    EXPECT_EQ(info->tick_size, types::Decimal(1, 2));
    EXPECT_EQ(info->lot_size, types::Decimal::FromInt(1));
    EXPECT_EQ(info->min_order_size, types::Decimal::FromInt(1));
    EXPECT_TRUE(info->min_notional.IsZero());
    ASSERT_EQ(mHttp->requests.size(), 1u);
    EXPECT_EQ(mHttp->requests[0].url, "https://api.test/marketdata/v1/quotes");
    EXPECT_EQ(mHttp->requests[0].query.at("symbols"), "BRK/B");
    // Cached.
    ASSERT_OK(source.GetSymbolInfo("BRK.B"));
    EXPECT_EQ(mHttp->requests.size(), 1u);
    // Not a stock symbol at all: refused before asking.
    EXPECT_FALSE(source.GetSymbolInfo("BTC/USD").has_value());
    EXPECT_EQ(mHttp->requests.size(), 1u);
}

TEST_F(SchwabDataSourceTest, ASubDollarStockQuotesInHundredthsOfACent) {
    Reply(QuoteBody("TINY", "EQUITY", "0.4321"));
    auto info = Source().GetSymbolInfo("TINY");
    ASSERT_OK(info);
    EXPECT_EQ(info->tick_size, types::Decimal(1, 4));
}

TEST_F(SchwabDataSourceTest, UnknownSymbolsAndOtherAssetsAreRefused) {
    Reply(R"({"errors":{"invalidSymbols":["ZZZZQ"]}})");
    auto unknown = Source().GetSymbolInfo("ZZZZQ");
    ASSERT_FALSE(unknown.has_value());
    EXPECT_EQ(unknown.error().category, types::ErrorCategory::kNotFound);

    Reply(QuoteBody("VFIAX", "MUTUAL_FUND", "600"));
    auto fund = Source().GetSymbolInfo("VFIAX");
    ASSERT_FALSE(fund.has_value());
    EXPECT_EQ(fund.error().category, types::ErrorCategory::kUnsupported);

    // A failed request is an error, not cached.
    mHttp->reply = [](const net::HttpRequest&) { return Status(400); };
    auto source = Source();
    EXPECT_FALSE(source.GetSymbolInfo("SPY"));
    Reply(QuoteBody("SPY", "EQUITY", "728.37"));
    EXPECT_TRUE(source.GetSymbolInfo("SPY"));
}

}  // namespace
}  // namespace kestrel::data
