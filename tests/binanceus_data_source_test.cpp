#include "kestrel/data/binanceus_data_source.hpp"

#include <deque>

#include "kestrel/data/binanceus_quote_stream.hpp"
#include "kestrel/util/json.hpp"
#include "test_helpers.hpp"

namespace {

using namespace kestrel;
using kestrel::testing::D;
using namespace std::chrono_literals;

class FakeHttp : public net::HttpClient {
   public:
    void Push(std::string body) {
        mScript.push_back(net::HttpResponse{200, std::move(body), {}});
    }
    void PushError(types::Error e) {
        mScript.push_back(std::unexpected(std::move(e)));
    }
    std::vector<net::HttpRequest> requests;

    types::Result<net::HttpResponse> Send(const net::HttpRequest& r) override {
        requests.push_back(r);
        if (mScript.empty()) {
            return types::MakeError(types::ErrorCategory::kInternal,
                                    "unscripted request");
        }
        auto next = std::move(mScript.front());
        mScript.pop_front();
        return next;
    }

   private:
    std::deque<types::Result<net::HttpResponse>> mScript;
};

constexpr std::int64_t kHourMs = 3'600'000;
constexpr std::int64_t kBase = 1'700'000'000'000 / kHourMs * kHourMs;

std::string Kline(std::int64_t t) {
    return "[" + std::to_string(t) +
           ",\"100.0\",\"110.0\",\"90.0\",\"105.0\",\"12.5\"," +
           std::to_string(t + kHourMs - 1) +
           ",\"1300.0\",42,\"1\",\"2\",\"0\"]";
}

std::string Klines(const std::vector<std::int64_t>& times) {
    std::string rows;
    for (const auto t : times) {
        rows += (rows.empty() ? "" : ",") + Kline(t);
    }
    return "[" + rows + "]";
}

struct Fixture {
    std::shared_ptr<FakeHttp> http = std::make_shared<FakeHttp>();
    types::Timestamp now =
        types::FromEpochMillis(kBase + 10 * kHourMs + 600'000);
    data::BinanceUSDataSource MakeSource() {
        data::BinanceUSDataSource::Options o;
        o.clock = [this] { return now; };
        o.requests_per_second = 1000;
        o.request_burst = 1000;
        o.retry.initial_delay = 10ms;
        o.retry.jitter = 0;
        o.sleeper = [](std::chrono::milliseconds) {};
        return data::BinanceUSDataSource(http, o);
    }
};

TEST(BinanceUSGetBars, ParsesRowsAndExcludesFormingBar) {
    Fixture f;
    f.http->Push(Klines({kBase, kBase + kHourMs, kBase + 10 * kHourMs}));
    auto src = f.MakeSource();
    auto bars = src.GetBars("BTC/USD", types::Timeframe::kH1,
                            types::FromEpochMillis(kBase),
                            types::FromEpochMillis(kBase + 11 * kHourMs));
    ASSERT_OK(bars);
    ASSERT_EQ(bars->size(), 2u);  // the +10h bar is still forming
    EXPECT_EQ((*bars)[0].open_time, types::FromEpochMillis(kBase));
    EXPECT_EQ((*bars)[0].open, D("100.0"));
    EXPECT_EQ((*bars)[0].volume, D("12.5"));

    const auto& q = f.http->requests.at(0).query;
    EXPECT_EQ(q.at("symbol"), "BTCUSD");
    EXPECT_EQ(q.at("interval"), "1h");
    EXPECT_EQ(q.at("startTime"), std::to_string(kBase));
    EXPECT_EQ(q.at("endTime"), std::to_string(kBase + 11 * kHourMs - 1));
}

TEST(BinanceUSGetBars, PagesWhenAPageIsFull) {
    Fixture f;
    f.now = types::FromEpochMillis(kBase + 5000 * kHourMs);
    std::vector<std::int64_t> first, second{kBase + 1000 * kHourMs};
    for (int i = 0; i < 1000; ++i) {
        first.push_back(kBase + i * kHourMs);
    }
    f.http->Push(Klines(first));
    f.http->Push(Klines(second));
    auto src = f.MakeSource();
    auto bars = src.GetBars("BTC/USD", types::Timeframe::kH1,
                            types::FromEpochMillis(kBase),
                            types::FromEpochMillis(kBase + 2000 * kHourMs));
    ASSERT_OK(bars);
    EXPECT_EQ(bars->size(), 1001u);
    ASSERT_EQ(f.http->requests.size(), 2u);
    EXPECT_EQ(f.http->requests[1].query.at("startTime"),
              std::to_string(kBase + 999 * kHourMs + 1));
}

TEST(BinanceUSGetBars, RejectsUnsupportedTimeframeWithoutCallingOut) {
    Fixture f;
    auto src = f.MakeSource();
    auto bars = src.GetBars("BTC/USD", types::Timeframe::kM10,
                            types::FromEpochMillis(kBase),
                            types::FromEpochMillis(kBase + kHourMs));
    ASSERT_FALSE(bars);
    EXPECT_EQ(bars.error().category, types::ErrorCategory::kUnsupported);
    EXPECT_TRUE(f.http->requests.empty());
}

TEST(BinanceUSGetBars, RetriesRateLimits) {
    Fixture f;
    f.http->PushError(
        types::Error{types::ErrorCategory::kRateLimit, "slow down", 429});
    f.http->Push(Klines({kBase}));
    auto src = f.MakeSource();
    auto bars = src.GetBars("BTC/USD", types::Timeframe::kH1,
                            types::FromEpochMillis(kBase),
                            types::FromEpochMillis(kBase + kHourMs));
    ASSERT_OK(bars);
    EXPECT_EQ(bars->size(), 1u);
    EXPECT_EQ(f.http->requests.size(), 2u);
}

TEST(BinanceUSGetBars, InvalidSymbolIsNotFoundAndNotRetried) {
    Fixture f;
    f.http->PushError(types::Error{
        types::ErrorCategory::kRejected,
        "HTTP 400: {\"code\":-1121,\"msg\":\"Invalid symbol.\"}", 400});
    auto src = f.MakeSource();
    auto bars = src.GetBars("FOO/USD", types::Timeframe::kH1,
                            types::FromEpochMillis(kBase),
                            types::FromEpochMillis(kBase + kHourMs));
    ASSERT_FALSE(bars);
    EXPECT_EQ(bars.error().category, types::ErrorCategory::kNotFound);
    EXPECT_EQ(f.http->requests.size(), 1u);
}

TEST(BinanceUSGetBars, MalformedResponseIsAParseError) {
    Fixture f;
    f.http->Push(R"([[1,"2"]])");
    auto src = f.MakeSource();
    auto bars = src.GetBars("BTC/USD", types::Timeframe::kH1,
                            types::FromEpochMillis(kBase),
                            types::FromEpochMillis(kBase + kHourMs));
    ASSERT_FALSE(bars);
    EXPECT_EQ(bars.error().category, types::ErrorCategory::kParse);
}

TEST(BinanceUSSymbolInfo, ParsedFromExchangeInfoAndCached) {
    Fixture f;
    f.http->Push(R"({"symbols":[{"symbol":"BTCUSD","filters":[
        {"filterType":"PRICE_FILTER","tickSize":"0.01000000"},
        {"filterType":"LOT_SIZE","minQty":"0.00010000","stepSize":"0.00001000"}
    ]}]})");
    auto src = f.MakeSource();
    auto info = src.GetSymbolInfo("BTC/USD");
    ASSERT_OK(info);
    EXPECT_EQ(info->tick_size, D("0.01"));
    EXPECT_EQ(info->lot_size, D("0.00001"));
    EXPECT_EQ(info->min_order_size, D("0.0001"));
    ASSERT_OK(src.GetSymbolInfo("BTC/USD"));
    EXPECT_EQ(f.http->requests.size(), 1u);  // second call served from cache
    EXPECT_EQ(f.http->requests[0].query.at("symbol"), "BTCUSD");
}

TEST(BinanceUSSymbolInfo, ReadsTheMinimumNotionalFromEitherFilter) {
    for (const char* filter : {"MIN_NOTIONAL", "NOTIONAL"}) {
        Fixture f;
        f.http->Push(std::string(R"({"symbols":[{"symbol":"ZECUSDT","filters":[
            {"filterType":"PRICE_FILTER","tickSize":"0.01000000"},
            {"filterType":"LOT_SIZE","minQty":"0.00100000","stepSize":"0.00100000"},
            {"filterType":")") +
                     filter +
                     R"(","minNotional":"1.00000000","applyToMarket":true}
        ]}]})");
        auto src = f.MakeSource();
        auto info = src.GetSymbolInfo("ZEC/USDT");
        ASSERT_OK(info);
        EXPECT_EQ(info->min_notional, D("1")) << filter;
    }
    Fixture f;  // neither: no minimum
    f.http->Push(R"({"symbols":[{"symbol":"ZECUSDT","filters":[
        {"filterType":"PRICE_FILTER","tickSize":"0.01000000"},
        {"filterType":"LOT_SIZE","minQty":"0.00100000","stepSize":"0.00100000"}
    ]}]})");
    auto src = f.MakeSource();
    EXPECT_TRUE(src.GetSymbolInfo("ZEC/USDT")->min_notional.IsZero());
}

TEST(BinanceUSStream, BuildsCombinedStreamUrl) {
    EXPECT_EQ(data::BinanceUSQuoteStream::BuildUrl("wss://h:9443",
                                                   {"BTCUSD", "ETHUSD"}),
              "wss://h:9443/stream?streams=btcusd@bookTicker/btcusd@trade/"
              "ethusd@bookTicker/ethusd@trade");
}

TEST(BinanceUSStreamMerger, BookAndTradeAccumulate) {
    data::BinanceUSStreamMerger merger;
    auto t = types::FromEpochMillis(5);
    auto parse = [](const char* s) { return *util::ParseJson(s); };

    auto q1 = merger.Apply(
        parse(R"({"stream":"btcusd@bookTicker","data":{"u":1,"s":"BTCUSD",
            "b":"100.10","B":"1.5","a":"100.20","A":"2.5"}})"),
        t);
    ASSERT_EQ(q1.size(), 1u);
    EXPECT_EQ(q1[0].symbol, "BTC/USD");
    EXPECT_EQ(q1[0].venue, "binanceus");
    EXPECT_EQ(*q1[0].bid, D("100.10"));
    EXPECT_EQ(*q1[0].ask_size, D("2.5"));
    EXPECT_FALSE(q1[0].last);

    auto q2 = merger.Apply(
        parse(R"({"stream":"btcusd@trade","data":{"e":"trade","s":"BTCUSD",
            "p":"100.15","q":"0.25","T":1700000000123}})"),
        t);
    ASSERT_EQ(q2.size(), 1u);
    EXPECT_EQ(*q2[0].last, D("100.15"));
    EXPECT_EQ(*q2[0].last_size, D("0.25"));
    EXPECT_EQ(*q2[0].bid, D("100.10"));  // retained from the book update
    EXPECT_EQ(*q2[0].exchange_time, types::FromEpochMillis(1700000000123));
}

TEST(BinanceUSStreamMerger, IgnoresUnknownMessages) {
    data::BinanceUSStreamMerger merger;
    auto parse = [](const char* s) { return *util::ParseJson(s); };
    EXPECT_TRUE(merger.Apply(parse(R"({"result":null,"id":1})"), {}).empty());
    EXPECT_TRUE(
        merger.Apply(parse(R"({"stream":"x@depth","data":{"s":"BTCUSD"}})"), {})
            .empty());
}

}  // namespace
