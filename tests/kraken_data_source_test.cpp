#include <deque>

#include "kestrel/data/kraken_data_source.hpp"
#include "kestrel/data/kraken_quote_stream.hpp"
#include "kestrel/util/json.hpp"
#include "test_helpers.hpp"

namespace {

using namespace kestrel;
using kestrel::testing::D;
using namespace std::chrono_literals;

// Scripted HTTP client: pops one canned outcome per request.
class FakeHttp : public net::HttpClient {
 public:
  void Push(std::string body) { script_.push_back(net::HttpResponse{200, std::move(body), {}}); }
  void PushError(types::Error e) { script_.push_back(std::unexpected(std::move(e))); }
  std::vector<net::HttpRequest> requests;

  types::Result<net::HttpResponse> Send(const net::HttpRequest& r) override {
    requests.push_back(r);
    if (script_.empty()) return types::MakeError(types::ErrorCategory::kInternal, "unscripted request");
    auto next = std::move(script_.front());
    script_.pop_front();
    return next;
  }

 private:
  std::deque<types::Result<net::HttpResponse>> script_;
};

constexpr std::int64_t kHour = 3600;
constexpr std::int64_t kBase = 1'700'000'000 / kHour * kHour;  // an hour boundary

std::string Candle(std::int64_t t, const char* o = "100.0", const char* h = "110.0", const char* l = "90.0",
                   const char* c = "105.0") {
  return "[" + std::to_string(t) + ",\"" + o + "\",\"" + h + "\",\"" + l + "\",\"" + c +
         "\",\"102.5\",\"12.34567890\",42]";
}

std::string Ohlc(const std::vector<std::int64_t>& times) {
  std::string rows;
  for (const auto t : times) rows += (rows.empty() ? "" : ",") + Candle(t);
  return R"({"error":[],"result":{"XXBTZUSD":[)" + rows + R"(],"last":)" + std::to_string(times.back()) + "}}";
}

struct Fixture {
  std::shared_ptr<FakeHttp> http = std::make_shared<FakeHttp>();
  types::Timestamp now = types::FromEpochSeconds(kBase + 10 * kHour + 600);  // 10 h 10 m after kBase
  std::vector<std::chrono::milliseconds> sleeps;
  data::KrakenDataSource MakeSource() {
    data::KrakenDataSource::Options o;
    o.clock = [this] { return now; };
    o.requests_per_second = 1000;
    o.request_burst = 1000;
    o.retry.initial_delay = 10ms;
    o.retry.jitter = 0;
    o.sleeper = [this](std::chrono::milliseconds d) { sleeps.push_back(d); };
    return data::KrakenDataSource(http, o);
  }
};

TEST(KrakenGetBars, HalfOpenRangeAndExcludesFormingBar) {
  Fixture f;
  // Bars at +0h .. +10h; the +10h bar is still forming (now is +10h10m).
  f.http->Push(Ohlc({kBase, kBase + kHour, kBase + 2 * kHour, kBase + 3 * kHour, kBase + 10 * kHour}));
  auto source = f.MakeSource();

  // [+1h, +3h) must return exactly the +1h and +2h bars.
  auto bars = source.GetBars("BTC/USD", types::Timeframe::kH1, types::FromEpochSeconds(kBase + kHour),
                             types::FromEpochSeconds(kBase + 3 * kHour));
  ASSERT_OK(bars);
  ASSERT_EQ(bars->size(), 2u);
  EXPECT_EQ(types::ToEpochSeconds((*bars)[0].open_time), kBase + kHour);
  EXPECT_EQ(types::ToEpochSeconds((*bars)[1].open_time), kBase + 2 * kHour);
  EXPECT_EQ((*bars)[0].open, D("100.0"));
  EXPECT_EQ((*bars)[0].volume, D("12.34567890"));

  ASSERT_EQ(f.http->requests.size(), 1u);
  EXPECT_EQ(f.http->requests[0].query.at("pair"), "BTC/USD");
  EXPECT_EQ(f.http->requests[0].query.at("interval"), "60");
  EXPECT_EQ(f.http->requests[0].query.at("since"), std::to_string(kBase + kHour));
}

TEST(KrakenGetBars, NeverReturnsTheFormingBarEvenIfInRange) {
  Fixture f;
  f.http->Push(Ohlc({kBase + 8 * kHour, kBase + 9 * kHour, kBase + 10 * kHour}));
  auto source = f.MakeSource();
  auto bars = source.GetBars("BTC/USD", types::Timeframe::kH1, types::FromEpochSeconds(kBase + 8 * kHour),
                             types::FromEpochSeconds(kBase + 20 * kHour));
  ASSERT_OK(bars);
  ASSERT_EQ(bars->size(), 2u);
  EXPECT_EQ(types::ToEpochSeconds(bars->back().open_time), kBase + 9 * kHour);
}

TEST(KrakenGetBars, PagesWhenARequestDoesNotReachTheEnd) {
  Fixture f;
  f.http->Push(Ohlc({kBase, kBase + kHour, kBase + 2 * kHour}));
  f.http->Push(Ohlc({kBase + 3 * kHour, kBase + 4 * kHour, kBase + 10 * kHour}));
  auto source = f.MakeSource();
  auto bars = source.GetBars("BTC/USD", types::Timeframe::kH1, types::FromEpochSeconds(kBase),
                             types::FromEpochSeconds(kBase + 6 * kHour));
  ASSERT_OK(bars);
  EXPECT_EQ(bars->size(), 5u);  // +0h .. +4h
  ASSERT_EQ(f.http->requests.size(), 2u);
  EXPECT_EQ(f.http->requests[1].query.at("since"), std::to_string(kBase + 2 * kHour + 1));
}

TEST(KrakenGetBars, ErrorsWhenRangeIsOlderThanKrakenRetains) {
  Fixture f;
  std::vector<std::int64_t> times;
  for (int i = 0; i < 720; ++i) times.push_back(kBase - 700 * kHour + i * kHour);
  f.http->Push(Ohlc(times));
  auto source = f.MakeSource();
  auto bars = source.GetBars("BTC/USD", types::Timeframe::kH1, types::FromEpochSeconds(kBase - 5000 * kHour),
                             types::FromEpochSeconds(kBase));
  ASSERT_FALSE(bars.has_value());
  EXPECT_EQ(bars.error().category, types::ErrorCategory::kUnsupported);
}

TEST(KrakenGetBars, RejectsUnsupportedTimeframeWithoutCallingOut) {
  Fixture f;
  auto source = f.MakeSource();
  auto bars = source.GetBars("BTC/USD", types::Timeframe::kM3, types::FromEpochSeconds(kBase),
                             types::FromEpochSeconds(kBase + kHour));
  ASSERT_FALSE(bars.has_value());
  EXPECT_EQ(bars.error().category, types::ErrorCategory::kUnsupported);
  EXPECT_TRUE(f.http->requests.empty());
}

TEST(KrakenGetBars, RetriesRateLimitsAndNetworkErrors) {
  Fixture f;
  f.http->PushError({types::ErrorCategory::kNetwork, "connection reset", std::nullopt, std::nullopt, std::nullopt});
  f.http->Push(R"({"error":["EAPI:Rate limit exceeded"]})");
  f.http->Push(Ohlc({kBase, kBase + kHour}));
  auto source = f.MakeSource();
  auto bars = source.GetBars("BTC/USD", types::Timeframe::kH1, types::FromEpochSeconds(kBase),
                             types::FromEpochSeconds(kBase + 2 * kHour));
  ASSERT_OK(bars);
  EXPECT_EQ(bars->size(), 2u);
  EXPECT_EQ(f.http->requests.size(), 3u);
  EXPECT_EQ(f.sleeps, (std::vector{10ms, 20ms}));  // exponential backoff
}

TEST(KrakenGetBars, UnknownPairIsNotRetried) {
  Fixture f;
  f.http->Push(R"({"error":["EQuery:Unknown asset pair"]})");
  auto source = f.MakeSource();
  auto bars = source.GetBars("FAKE/USD", types::Timeframe::kH1, types::FromEpochSeconds(kBase),
                             types::FromEpochSeconds(kBase + kHour));
  ASSERT_FALSE(bars.has_value());
  EXPECT_EQ(bars.error().category, types::ErrorCategory::kNotFound);
  EXPECT_EQ(bars.error().provider_code, "EQuery:Unknown asset pair");
  EXPECT_EQ(f.http->requests.size(), 1u);
}

TEST(KrakenGetBars, MalformedResponseIsAParseError) {
  Fixture f;
  f.http->Push("not json");
  auto source = f.MakeSource();
  auto bars = source.GetBars("BTC/USD", types::Timeframe::kH1, types::FromEpochSeconds(kBase),
                             types::FromEpochSeconds(kBase + kHour));
  ASSERT_FALSE(bars.has_value());
  EXPECT_EQ(bars.error().category, types::ErrorCategory::kParse);
}

TEST(KrakenSymbolInfo, ParsedFromAssetPairsAndCached) {
  Fixture f;
  f.http->Push(R"({"error":[],"result":{"XXBTZUSD":{"altname":"XBTUSD","pair_decimals":1,"lot_decimals":8,
                  "tick_size":"0.1","ordermin":"0.00005","costmin":"0.5"}}})");
  auto source = f.MakeSource();
  auto info = source.GetSymbolInfo("BTC/USD");
  ASSERT_OK(info);
  EXPECT_EQ(info->tick_size, D("0.1"));
  EXPECT_EQ(info->lot_size, D("0.00000001"));
  EXPECT_EQ(info->min_order_size, D("0.00005"));
  ASSERT_OK(source.GetSymbolInfo("BTC/USD"));
  EXPECT_EQ(f.http->requests.size(), 1u);  // second call served from cache
}

TEST(KrakenErrors, Classification) {
  using types::ErrorCategory;
  EXPECT_EQ(data::ClassifyKrakenError("EAPI:Rate limit exceeded"), ErrorCategory::kRateLimit);
  EXPECT_EQ(data::ClassifyKrakenError("EGeneral:Too many requests"), ErrorCategory::kRateLimit);
  EXPECT_EQ(data::ClassifyKrakenError("EService:Unavailable"), ErrorCategory::kServer);
  EXPECT_EQ(data::ClassifyKrakenError("EAPI:Invalid key"), ErrorCategory::kAuth);
  EXPECT_EQ(data::ClassifyKrakenError("EOrder:Insufficient funds"), ErrorCategory::kInsufficientFunds);
  EXPECT_EQ(data::ClassifyKrakenError("EOrder:Something else"), ErrorCategory::kRejected);
}

nlohmann::json Msg(const char* text) { return *util::ParseJson(text); }

TEST(KrakenTickerMerger, PartialUpdatesAccumulate) {
  data::KrakenTickerMerger merger;
  const auto t1 = types::FromEpochSeconds(100);
  const auto t2 = types::FromEpochSeconds(101);

  auto q1 = merger.Apply(Msg(R"({"channel":"ticker","type":"snapshot","data":[{"symbol":"BTC/USD",
      "bid":50000.1,"bid_qty":0.5,"ask":50000.2,"ask_qty":1.25,"last":50000.15}]})"), t1);
  ASSERT_EQ(q1.size(), 1u);
  EXPECT_EQ(q1[0].symbol, "BTC/USD");
  EXPECT_EQ(q1[0].venue, "kraken");
  EXPECT_EQ(*q1[0].bid, D("50000.1"));
  EXPECT_EQ(*q1[0].ask_size, D("1.25"));
  EXPECT_FALSE(q1[0].last_size.has_value());  // not in Kraken's ticker
  EXPECT_FALSE(q1[0].exchange_time.has_value());

  // An update carrying only a new bid keeps everything else.
  auto q2 = merger.Apply(Msg(R"({"channel":"ticker","type":"update","data":[{"symbol":"BTC/USD","bid":50000.3,
      "timestamp":"2024-01-02T03:04:05.678901Z"}]})"), t2);
  ASSERT_EQ(q2.size(), 1u);
  EXPECT_EQ(*q2[0].bid, D("50000.3"));
  EXPECT_EQ(*q2[0].bid_size, D("0.5"));
  EXPECT_EQ(*q2[0].ask, D("50000.2"));
  EXPECT_EQ(*q2[0].last, D("50000.15"));
  EXPECT_EQ(q2[0].received_time, t2);
  EXPECT_EQ(types::FormatIso8601(*q2[0].exchange_time), "2024-01-02T03:04:05.678Z");

  // Other symbols are tracked independently; legacy codes are normalized.
  auto q3 = merger.Apply(Msg(R"({"channel":"ticker","type":"update","data":[{"symbol":"XDG/USD","ask":0.1}]})"), t2);
  ASSERT_EQ(q3.size(), 1u);
  EXPECT_EQ(q3[0].symbol, "DOGE/USD");
  EXPECT_FALSE(q3[0].bid.has_value());

  merger.Reset();
  auto q4 = merger.Apply(Msg(R"({"channel":"ticker","type":"update","data":[{"symbol":"BTC/USD","bid":1}]})"), t2);
  EXPECT_FALSE(q4[0].ask.has_value());  // state cleared on reconnect
}

TEST(KrakenTickerMerger, IgnoresNonTickerMessages) {
  data::KrakenTickerMerger merger;
  const auto t = types::FromEpochSeconds(1);
  EXPECT_TRUE(merger.Apply(Msg(R"({"channel":"heartbeat"})"), t).empty());
  EXPECT_TRUE(merger.Apply(Msg(R"({"method":"subscribe","success":true})"), t).empty());
  EXPECT_TRUE(merger.Apply(Msg(R"([1,2,3])"), t).empty());
}

TEST(QuoteMerge, OverlaysOnlyPresentFields) {
  types::Quote base;
  base.symbol = "X";
  base.bid = D("1");
  base.ask = D("2");
  types::Quote update;
  update.ask = D("3");
  update.received_time = types::FromEpochSeconds(5);
  base.MergeFrom(update);
  EXPECT_EQ(*base.bid, D("1"));
  EXPECT_EQ(*base.ask, D("3"));
  EXPECT_EQ(base.symbol, "X");
  EXPECT_EQ(base.received_time, types::FromEpochSeconds(5));
}

}  // namespace
