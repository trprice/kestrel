#include "kestrel/data/schwab_data_source.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <functional>

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

TEST_F(SchwabDataSourceTest, SymbolInfoUsesUsEquityRules) {
    auto info = Source().GetSymbolInfo("brk.b");
    ASSERT_OK(info);
    EXPECT_EQ(info->symbol, "BRK.B");
    EXPECT_EQ(info->tick_size, types::Decimal(1, 2));
    EXPECT_EQ(info->lot_size, types::Decimal::FromInt(1));
    EXPECT_EQ(info->min_order_size, types::Decimal::FromInt(1));
    EXPECT_TRUE(info->min_notional.IsZero());
    EXPECT_FALSE(Source().GetSymbolInfo("BTC/USD").has_value());
    EXPECT_TRUE(mHttp->requests.empty());  // nothing asked of Schwab
}

}  // namespace
}  // namespace kestrel::data
