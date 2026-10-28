#include "kestrel/net/timed_http_client.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>

namespace kestrel::net {
namespace {

using std::chrono::microseconds;
using std::chrono::milliseconds;

// Answers with a scripted result and moves the fake clock by `takes`.
class FakeInner : public HttpClient {
   public:
    types::Result<HttpResponse> Send(const HttpRequest& request) override {
        last = request;
        now += takes;
        if (fail) {
            types::Error error;
            error.category = types::ErrorCategory::kRateLimit;
            error.message = "slow down";
            error.http_status = fail_status;
            return std::unexpected(error);
        }
        HttpResponse response;
        response.status = 200;
        return response;
    }

    std::chrono::steady_clock::time_point now{};
    milliseconds takes{10};
    bool fail = false;
    int fail_status = 429;
    HttpRequest last;
};

struct Fixture {
    std::shared_ptr<FakeInner> inner = std::make_shared<FakeInner>();
    std::shared_ptr<LatencyStats> stats = std::make_shared<LatencyStats>();
    TimedHttpClient client{inner, stats, milliseconds(500),
                           [this] { return inner->now; }};
};

HttpRequest Get(const std::string& url) {
    HttpRequest request;
    request.url = url;
    return request;
}

TEST(LatencyStats, ReportsCountsAndPercentilesPerEndpoint) {
    LatencyStats stats;
    for (int ms = 1; ms <= 100; ++ms) {
        stats.Record("GET a", microseconds(ms * 1000), true, 200);
    }
    stats.Record("GET b", microseconds(7000), true, 200);

    const auto rows = stats.Summarize(false);
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0].endpoint, "GET a");
    EXPECT_EQ(rows[0].count, 100u);
    EXPECT_DOUBLE_EQ(rows[0].min_ms, 1);
    EXPECT_DOUBLE_EQ(rows[0].p50_ms, 50);
    EXPECT_DOUBLE_EQ(rows[0].p95_ms, 95);
    EXPECT_DOUBLE_EQ(rows[0].p99_ms, 99);
    EXPECT_DOUBLE_EQ(rows[0].max_ms, 100);
    EXPECT_EQ(rows[1].endpoint, "GET b");
    EXPECT_DOUBLE_EQ(rows[1].p50_ms, 7);
    EXPECT_DOUBLE_EQ(rows[1].p99_ms, 7);
}

TEST(LatencyStats, CountsErrorsAndRateLimits) {
    LatencyStats stats;
    stats.Record("GET a", milliseconds(5), true, 200);
    stats.Record("GET a", milliseconds(5), false, 500);
    stats.Record("GET a", milliseconds(5), false, 429);
    stats.Record("GET a", milliseconds(5), false, 418);
    stats.Record("GET a", milliseconds(5), false, 0);  // transport failure

    const auto rows = stats.Summarize(false);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].count, 5u);
    EXPECT_EQ(rows[0].errors, 4u);
    EXPECT_EQ(rows[0].rate_limited, 2u);
}

TEST(LatencyStats, ResetStartsANewWindow) {
    LatencyStats stats;
    stats.Record("GET a", milliseconds(5), true, 200);
    EXPECT_EQ(stats.Summarize(true).size(), 1u);
    EXPECT_TRUE(stats.Summarize(false).empty());
    stats.Record("GET a", milliseconds(9), true, 200);
    const auto rows = stats.Summarize(false);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].count, 1u);
    EXPECT_DOUBLE_EQ(rows[0].min_ms, 9);
}

TEST(LatencyStats, WindowsAndTotalsAreIndependent) {
    LatencyStats stats;
    stats.Record("GET a", milliseconds(5), true, 200);
    stats.Record("GET a", milliseconds(7), true, 200);
    auto first = stats.TakeWindow();
    ASSERT_EQ(first.size(), 1u);
    EXPECT_EQ(first[0].count, 2u);
    EXPECT_TRUE(stats.TakeWindow().empty());  // taken: the window starts anew

    stats.Record("GET a", milliseconds(9), false, 500);
    auto second = stats.TakeWindow();
    ASSERT_EQ(second.size(), 1u);
    EXPECT_EQ(second[0].count, 1u);
    EXPECT_EQ(second[0].errors, 1u);
    EXPECT_DOUBLE_EQ(second[0].min_ms, 9);

    // The run's totals were not disturbed by taking windows...
    auto totals = stats.Summarize(true);
    ASSERT_EQ(totals.size(), 1u);
    EXPECT_EQ(totals[0].count, 3u);
    // ...and resetting them does not disturb the window.
    stats.Record("GET a", milliseconds(2), true, 200);
    EXPECT_EQ(stats.Summarize(false)[0].count, 1u);
    stats.Summarize(true);
    EXPECT_EQ(stats.TakeWindow()[0].count, 1u);
}

TEST(LatencyStats, KeepsExactTotalsWhenOnlyTheRecentSamplesFit) {
    LatencyStats stats(10);
    stats.Record("GET a", milliseconds(1000), true, 200);  // will fall out
    for (int i = 0; i < 20; ++i) {
        stats.Record("GET a", milliseconds(2), true, 200);
    }
    const auto rows = stats.Summarize(false);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].count, 21u);
    EXPECT_DOUBLE_EQ(rows[0].max_ms, 1000);  // totals still see it
    EXPECT_DOUBLE_EQ(rows[0].p99_ms, 2);     // percentiles use the recent ones
}

TEST(TimedHttpClient, RecordsTheElapsedTimeOfASuccessfulCall) {
    Fixture f;
    f.inner->takes = milliseconds(42);
    auto response = f.client.Send(Get("https://x.test/api/v3/time"));
    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(response->status, 200);

    const auto rows = f.stats->Summarize(false);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].endpoint, "GET https://x.test/api/v3/time");
    EXPECT_EQ(rows[0].count, 1u);
    EXPECT_EQ(rows[0].errors, 0u);
    EXPECT_DOUBLE_EQ(rows[0].p50_ms, 42);
}

TEST(TimedHttpClient, RecordsFailuresAndPassesTheErrorThrough) {
    Fixture f;
    f.inner->fail = true;
    auto response = f.client.Send(Get("https://x.test/api/v3/order"));
    ASSERT_FALSE(response.has_value());
    EXPECT_EQ(response.error().category, types::ErrorCategory::kRateLimit);

    const auto rows = f.stats->Summarize(false);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].errors, 1u);
    EXPECT_EQ(rows[0].rate_limited, 1u);
}

TEST(TimedHttpClient, KeysByMethodAndUrlButNeverTheQuery) {
    Fixture f;
    HttpRequest request;
    request.method = HttpMethod::kPost;
    request.url = "https://x.test/api/v3/order";
    request.query = {{"signature", "SECRET"}, {"timestamp", "1"}};
    request.headers = {{"X-MBX-APIKEY", "KEY"}};
    request.body = "quantity=1";
    ASSERT_TRUE(f.client.Send(request).has_value());

    EXPECT_EQ(f.inner->last.query.at("signature"), "SECRET");  // sent as is
    const auto rows = f.stats->Summarize(false);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].endpoint, "POST https://x.test/api/v3/order");
    EXPECT_EQ(rows[0].endpoint.find("SECRET"), std::string::npos);
    EXPECT_EQ(rows[0].endpoint.find("KEY"), std::string::npos);
}

TEST(TimedHttpClient, SeparatesMethodsOnTheSameUrl) {
    Fixture f;
    HttpRequest get = Get("https://x.test/api/v3/order");
    HttpRequest del = get;
    del.method = HttpMethod::kDelete;
    ASSERT_TRUE(f.client.Send(get).has_value());
    ASSERT_TRUE(f.client.Send(del).has_value());
    EXPECT_EQ(f.stats->Summarize(false).size(), 2u);
}

TEST(TimedHttpClient, EachAttemptIsItsOwnSample) {
    Fixture f;
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(f.client.Send(Get("https://x.test/a")).has_value());
    }
    EXPECT_EQ(f.stats->Summarize(false)[0].count, 3u);
}

TEST(TimedHttpClient, ASlowCallIsStillRecordedAndReturned) {
    Fixture f;
    f.inner->takes = milliseconds(900);  // over the 500 ms threshold
    auto response = f.client.Send(Get("https://x.test/slow"));
    ASSERT_TRUE(response.has_value());
    EXPECT_DOUBLE_EQ(f.stats->Summarize(false)[0].max_ms, 900);
}

TEST(HttpLatency, IsOneProcessWideInstance) {
    EXPECT_EQ(&HttpLatency(), &HttpLatency());
    EXPECT_NE(MakeTimedHttpClient(), nullptr);
}

}  // namespace
}  // namespace kestrel::net
