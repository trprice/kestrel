#include "kestrel/trading/binanceus_api.hpp"

#include <gtest/gtest.h>

#include <chrono>

#include "binanceus_trading_test_support.hpp"
#include "test_helpers.hpp"

namespace kestrel::trading {
namespace {

using kestrel::testing::QueryValue;
using kestrel::testing::RoutedHttp;
using std::chrono::seconds;

// The example from Binance's API documentation.
constexpr const char* kDocSecret =
    "NhqPtmdSJYdKjVHjA7PZj4Mge3R5YNiP1e3UZjInClVN65XAbvqqM6A7H5fATj0j";
constexpr const char* kDocSignature =
    "c8db56825ae71d6d79447849e617115f4a920fa2acdcab2b053c4b2838bd6b71";

constexpr std::int64_t kLocalMs = 1'700'000'000'000;

struct Fixture {
    std::shared_ptr<RoutedHttp> http = std::make_shared<RoutedHttp>();
    types::Timestamp local = types::FromEpochMillis(kLocalMs);
    std::chrono::steady_clock::time_point steady{};
    std::vector<std::chrono::milliseconds> sleeps;

    BinanceUSApi::Options Options() {
        BinanceUSApi::Options options;
        options.clock = [this] { return local; };
        options.steady = [this] { return steady; };
        options.sleeper = [this](std::chrono::milliseconds d) {
            sleeps.push_back(d);
        };
        options.weight_per_second = 100000;  // never throttle in a test
        options.weight_burst = 100000;
        return options;
    }
    BinanceUSApi Api(const char* secret = "secret") {
        return BinanceUSApi({"the-key", secret}, http, Options());
    }
    void ServerTime(std::int64_t ms) {
        http->Push("/api/v3/time",
                   "{\"serverTime\":" + std::to_string(ms) + "}");
    }
};

TEST(BinanceUSApi, SignsLikeTheDocumentationExample) {
    Fixture f;
    auto api = f.Api(kDocSecret);
    const auto query = api.SignedQuery({{"symbol", "LTCBTC"},
                                        {"side", "BUY"},
                                        {"type", "LIMIT"},
                                        {"timeInForce", "GTC"},
                                        {"quantity", "1"},
                                        {"price", "0.1"}},
                                       1499827319559);
    EXPECT_EQ(query,
              "symbol=LTCBTC&side=BUY&type=LIMIT&timeInForce=GTC&quantity=1&"
              "price=0.1&recvWindow=5000&timestamp=1499827319559&signature=" +
                  std::string(kDocSignature));
}

TEST(BinanceUSApi, SignsTheEncodedQueryItSends) {
    Fixture f;
    auto api = f.Api(kDocSecret);
    const auto query = api.SignedQuery({{"newClientOrderId", "a b&c"}}, 1);
    EXPECT_NE(query.find("newClientOrderId=a%20b%26c&"), std::string::npos);
}

TEST(BinanceUSApi, EncodesEverythingButUnreservedCharacters) {
    EXPECT_EQ(UrlEncode("AZaz09-._~"), "AZaz09-._~");
    EXPECT_EQ(UrlEncode("a b/c+d=e&f%"), "a%20b%2Fc%2Bd%3De%26f%25");
}

TEST(BinanceUSApi, SendsTheKeyAndTheSignedQueryVerbatim) {
    Fixture f;
    f.ServerTime(kLocalMs);
    f.http->Push("/api/v3/account", "{\"balances\":[]}");
    auto api = f.Api();

    auto body = api.Signed(net::HttpMethod::kGet, "/api/v3/account",
                           {{"symbol", "ZECUSDT"}}, 10, true);
    ASSERT_OK(body);

    const auto sent = f.http->To("/api/v3/account");
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0].url, "https://api.binance.us/api/v3/account");
    EXPECT_EQ(sent[0].headers.at("X-MBX-APIKEY"), "the-key");
    EXPECT_TRUE(sent[0].query.empty());
    EXPECT_EQ(QueryValue(sent[0].raw_query, "symbol"), "ZECUSDT");
    EXPECT_EQ(QueryValue(sent[0].raw_query, "recvWindow"), "5000");
    EXPECT_EQ(QueryValue(sent[0].raw_query, "timestamp"),
              std::to_string(kLocalMs));
    // The signature is the last parameter and covers the rest.
    const auto at = sent[0].raw_query.rfind("&signature=");
    ASSERT_NE(at, std::string::npos);
    EXPECT_EQ(sent[0].raw_query,
              api.SignedQuery({{"symbol", "ZECUSDT"}}, kLocalMs));
}

TEST(BinanceUSApi, SyncsTheClockOnceBeforeTheFirstSignedCall) {
    Fixture f;
    f.ServerTime(kLocalMs + 1500);  // the server is 1.5 s ahead
    f.http->Push("/api/v3/account", "{}");
    f.http->Push("/api/v3/account", "{}");
    auto api = f.Api();

    ASSERT_OK(
        api.Signed(net::HttpMethod::kGet, "/api/v3/account", {}, 10, true));
    ASSERT_OK(
        api.Signed(net::HttpMethod::kGet, "/api/v3/account", {}, 10, true));

    EXPECT_EQ(api.ClockOffsetMs(), 1500);
    EXPECT_EQ(f.http->To("/api/v3/time").size(), 1u);
    const auto sent = f.http->To("/api/v3/account");
    ASSERT_EQ(sent.size(), 2u);
    EXPECT_EQ(QueryValue(sent[0].raw_query, "timestamp"),
              std::to_string(kLocalMs + 1500));
    // The time request is not signed.
    EXPECT_TRUE(f.http->To("/api/v3/time")[0].headers.empty());
    EXPECT_EQ(f.http->To("/api/v3/time")[0].raw_query, "");
}

TEST(BinanceUSApi, CarriesOnWithNoOffsetWhenTheClockCannotBeRead) {
    Fixture f;  // no /time script: that request fails
    f.http->Push("/api/v3/account", "{}");
    auto api = f.Api();
    ASSERT_OK(
        api.Signed(net::HttpMethod::kGet, "/api/v3/account", {}, 10, true));
    EXPECT_EQ(api.ClockOffsetMs(), 0);
}

TEST(BinanceUSApi, ResyncsAndRepeatsARequestRefusedForItsTimestamp) {
    Fixture f;
    f.ServerTime(kLocalMs);
    f.http->PushStatus(
        "/api/v3/order", 400,
        "{\"code\":-1021,\"msg\":\"Timestamp outside recvWindow\"}");
    f.ServerTime(kLocalMs + 4000);
    f.http->Push("/api/v3/order", "{\"orderId\":7}");
    auto api = f.Api();

    // A write: the one resend is allowed because the server never ran it.
    auto body =
        api.Signed(net::HttpMethod::kPost, "/api/v3/order", {}, 1, false);
    ASSERT_OK(body);
    EXPECT_EQ(api.ClockOffsetMs(), 4000);
    const auto sent = f.http->To("/api/v3/order");
    ASSERT_EQ(sent.size(), 2u);
    EXPECT_EQ(QueryValue(sent[1].raw_query, "timestamp"),
              std::to_string(kLocalMs + 4000));
}

TEST(BinanceUSApi, DoesNotRetryAWriteAfterATransientFailure) {
    Fixture f;
    f.ServerTime(kLocalMs);
    f.http->PushStatus("/api/v3/order", 503, "unavailable");
    f.http->Push("/api/v3/order", "{}");
    auto api = f.Api();

    auto body =
        api.Signed(net::HttpMethod::kPost, "/api/v3/order", {}, 1, false);
    ASSERT_FALSE(body.has_value());
    EXPECT_EQ(body.error().category, types::ErrorCategory::kServer);
    EXPECT_EQ(f.http->To("/api/v3/order").size(), 1u);
}

TEST(BinanceUSApi, RetriesARead) {
    Fixture f;
    f.ServerTime(kLocalMs);
    f.http->PushStatus("/api/v3/account", 503, "unavailable");
    f.http->Push("/api/v3/account", "{\"ok\":true}");
    auto api = f.Api();

    auto body =
        api.Signed(net::HttpMethod::kGet, "/api/v3/account", {}, 10, true);
    ASSERT_OK(body);
    EXPECT_EQ(f.http->To("/api/v3/account").size(), 2u);
    EXPECT_EQ(f.sleeps.size(), 1u);
}

TEST(BinanceUSApi, ReportsBinanceErrorCodes) {
    Fixture f;
    f.ServerTime(kLocalMs);
    f.http->PushStatus("/api/v3/order", 400,
                       "{\"code\":-2013,\"msg\":\"Order does not exist.\"}");
    f.http->PushStatus(
        "/api/v3/order", 401,
        "{\"code\":-2015,\"msg\":\"Invalid API-key, IP, or permissions\"}");
    f.http->PushStatus(
        "/api/v3/order", 400,
        "{\"code\":-2010,\"msg\":\"Account has insufficient balance\"}");
    auto api = f.Api();

    auto missing =
        api.Signed(net::HttpMethod::kGet, "/api/v3/order", {}, 4, false);
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().category, types::ErrorCategory::kNotFound);
    EXPECT_EQ(missing.error().provider_code, "-2013");

    auto denied =
        api.Signed(net::HttpMethod::kGet, "/api/v3/order", {}, 4, false);
    ASSERT_FALSE(denied.has_value());
    EXPECT_EQ(denied.error().category, types::ErrorCategory::kAuth);

    auto refused =
        api.Signed(net::HttpMethod::kGet, "/api/v3/order", {}, 4, false);
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().category, types::ErrorCategory::kRejected);
    EXPECT_EQ(refused.error().provider_code, "-2010");
}

TEST(BinanceUSApi, StaysOffTheNetworkWhileBannedAndResumesAfterwards) {
    Fixture f;
    f.ServerTime(kLocalMs);
    f.http->PushStatus("/api/v3/account", 418, "banned",
                       {{"Retry-After", "60"}});
    f.http->Push("/api/v3/account", "{}");
    auto api = f.Api();

    auto banned =
        api.Signed(net::HttpMethod::kGet, "/api/v3/account", {}, 10, true);
    ASSERT_FALSE(banned.has_value());
    EXPECT_EQ(banned.error().http_status, 418);
    EXPECT_EQ(f.http->To("/api/v3/account").size(), 1u);

    f.steady += seconds(30);
    auto during =
        api.Signed(net::HttpMethod::kGet, "/api/v3/account", {}, 10, true);
    ASSERT_FALSE(during.has_value());
    EXPECT_EQ(during.error().category, types::ErrorCategory::kRateLimit);
    EXPECT_EQ(f.http->To("/api/v3/account").size(), 1u);  // no request made

    f.steady += seconds(31);
    ASSERT_OK(
        api.Signed(net::HttpMethod::kGet, "/api/v3/account", {}, 10, true));
    EXPECT_EQ(f.http->To("/api/v3/account").size(), 2u);
}

TEST(BinanceUSApi, ABanWithoutARetryAfterUsesTheDefault) {
    Fixture f;
    f.ServerTime(kLocalMs);
    f.http->PushStatus("/api/v3/account", 418, "banned");
    auto api = f.Api();
    ASSERT_FALSE(
        api.Signed(net::HttpMethod::kGet, "/api/v3/account", {}, 10, true)
            .has_value());
    f.steady += seconds(100);  // inside the 120 s default
    EXPECT_FALSE(
        api.Signed(net::HttpMethod::kGet, "/api/v3/account", {}, 10, true)
            .has_value());
    EXPECT_EQ(f.http->To("/api/v3/account").size(), 1u);
}

TEST(BinanceUSApi, PublicCallsAreNotSigned) {
    Fixture f;
    f.http->Push("/api/v3/ping", "{}");
    auto api = f.Api();
    ASSERT_OK(api.Public("/api/v3/ping", {{"a", "b"}}, 1));
    const auto sent = f.http->To("/api/v3/ping");
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_TRUE(sent[0].headers.empty());
    EXPECT_EQ(sent[0].raw_query, "a=b");
    EXPECT_TRUE(f.http->To("/api/v3/time").empty());
}

TEST(BinanceUSApi, ParsesNumbersWithoutLosingDigits) {
    Fixture f;
    f.http->Push("/api/v3/x", "{\"v\":0.12345678901234}");
    auto api = f.Api();
    auto body = api.Public("/api/v3/x", {}, 1);
    ASSERT_OK(body);
    EXPECT_EQ((*body)["v"].get<std::string>(), "0.12345678901234");
}

}  // namespace
}  // namespace kestrel::trading
