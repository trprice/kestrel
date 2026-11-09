#include "kestrel/data/schwab_streamer.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <functional>
#include <thread>

#include "kestrel/data/schwab_data_source.hpp"
#include "kestrel/util/json.hpp"
#include "test_helpers.hpp"

namespace kestrel::data {
namespace {

using namespace std::chrono_literals;
using nlohmann::json;
using Event = SchwabStreamerSocket::Event;

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

types::Result<net::HttpResponse> Ok(std::string body) {
    return net::HttpResponse{200, std::move(body), {}};
}

constexpr const char* kUserPreference = R"({
  "accounts": [],
  "streamerInfo": [{
    "streamerSocketUrl": "wss://streamer.test/ws",
    "schwabClientCustomerId": "customer",
    "schwabClientCorrelId": "correl",
    "schwabClientChannel": "N9",
    "schwabClientFunctionId": "APIAPP"}]})";

// What the fake sockets saw, shared with the test. Events are fired from the
// test's thread, standing in for the socket's. A socket may be started and
// stopped on the streamer's resume thread, hence the atomics; the handler is
// set before `started` counts it.
struct Wire {
    std::atomic<int> started = 0;
    std::atomic<int> stopped = 0;
    std::atomic<int> closes = 0;  // also from the streamer's timer thread
    bool reconnecting = true;
    std::string url;
    SchwabStreamerSocket::Handler handler;
    std::vector<json> sent;  // each request sent, unwrapped

    void Fire(Event event, const std::string& text = "") {
        handler(event, text);
    }
    void Respond(const std::string& service, const std::string& command,
                 int code, const std::string& msg = "") {
        Fire(Event::kMessage,
             json{{"response",
                   {{{"service", service},
                     {"command", command},
                     {"requestid", "1"},
                     {"content", {{"code", code}, {"msg", msg}}}}}}}
                 .dump());
    }
    void Data(const std::string& content) {
        Fire(Event::kMessage,
             R"({"data":[{"service":"LEVELONE_EQUITIES","timestamp":1,)"
             R"("command":"SUBS","content":)" +
                 content + "}]}");
    }
    void Chart(const std::string& content) {
        Fire(Event::kMessage,
             R"({"data":[{"service":"CHART_EQUITY","timestamp":1,)"
             R"("command":"SUBS","content":)" +
                 content + "}]}");
    }
    // Opens and logs in.
    void Connect() {
        Fire(Event::kOpen);
        Respond("ADMIN", "LOGIN", 0, "server=s;status=NP");
    }
    const json& Last() const { return sent.back(); }
};

class FakeSocket : public SchwabStreamerSocket {
   public:
    explicit FakeSocket(std::shared_ptr<Wire> wire) : mWire(std::move(wire)) {}
    void Start(const std::string& url, Handler handler) override {
        mWire->url = url;
        mWire->handler = std::move(handler);
        ++mWire->started;
    }
    void Send(const std::string& text) override {
        mWire->sent.push_back(json::parse(text)["requests"][0]);
    }
    void Close() override { ++mWire->closes; }
    void StopReconnecting() override { mWire->reconnecting = false; }
    void Stop() override { ++mWire->stopped; }

   private:
    std::shared_ptr<Wire> mWire;
};

class SchwabStreamerTest : public ::testing::Test {
   protected:
    void SetUp() override {
        mDir = std::filesystem::temp_directory_path() /
               ("kestrel-schwab-streamer-" +
                std::string(::testing::UnitTest::GetInstance()
                                ->current_test_info()
                                ->name()));
        std::filesystem::remove_all(mDir);
        std::filesystem::create_directories(mDir);
        const config::SchwabTokenStore store(mDir / "tokens.json");
        const auto now = types::Now();
        ASSERT_OK(store.Save({"access", "refresh", now + 1h, now + 72h}));
        mTokenHttp = std::make_shared<FakeHttp>();
        mTokenHttp->reply = [](const net::HttpRequest&) {
            return Ok(R"({"expires_in":1800,"token_type":"Bearer",)"
                      R"("refresh_token":"refresh","access_token":"renewed"})");
        };
        mAuth = std::make_shared<SchwabAuth>(
            config::SchwabCredentials{"key", "secret"}, store, mTokenHttp,
            "https://127.0.0.1:8182");
        ASSERT_OK(mAuth->Load());
        mHttp = std::make_shared<FakeHttp>();
        mHttp->reply = [](const net::HttpRequest&) {
            return Ok(kUserPreference);
        };
        mApi = std::make_shared<SchwabApi>(mAuth, mHttp,
                                           SchwabApi::Options{1000, 1000});
        mStreamer = MakeStreamer({});
    }

    std::shared_ptr<SchwabStreamer> MakeStreamer(
        std::chrono::milliseconds takeover_reconnect,
        std::chrono::milliseconds silence_timeout = std::chrono::seconds(60)) {
        return std::make_shared<SchwabStreamer>(
            mApi, "https://api.test/trader/v1",
            [this] { return std::make_unique<FakeSocket>(mWire); },
            takeover_reconnect, [this] { return mNow; }, silence_timeout);
    }

    void TakeOver() {
        mWire->Fire(Event::kMessage,
                    R"({"notify":[{"service":"Invalid Service","timestamp":1,)"
                    R"("content":{"code":12,"msg":"This Streamer was )"
                    R"(disconnected because of another connection"}}]})");
    }
    void TearDown() override { std::filesystem::remove_all(mDir); }

    // Subscribes `keys`, appending each quote to `out`.
    std::unique_ptr<Subscription> Subscribe(std::vector<std::string> keys,
                                            std::vector<types::Quote>& out) {
        auto sub = mStreamer->SubscribeQuotes(
            keys, [&out](const types::Quote& q) { out.push_back(q); });
        EXPECT_TRUE(sub.has_value()) << sub.error().ToString();
        return sub ? std::move(*sub) : nullptr;
    }

    std::filesystem::path mDir;
    std::shared_ptr<FakeHttp> mTokenHttp;
    std::shared_ptr<SchwabAuth> mAuth;
    std::shared_ptr<FakeHttp> mHttp;
    std::shared_ptr<SchwabApi> mApi;
    std::shared_ptr<Wire> mWire = std::make_shared<Wire>();
    std::shared_ptr<SchwabStreamer> mStreamer;
    types::Timestamp mNow = *types::ParseIso8601("2026-11-04T20:00:00Z");
};

types::Timestamp At(const char* iso) { return *types::ParseIso8601(iso); }

// A CHART_EQUITY item as Schwab sends it (fields one higher than documented).
std::string ChartItem(const std::string& key, const char* iso,
                      const std::string& ohlcv = R"("2":10,"3":12,"4":9,)"
                                                 R"("5":11,"6":100)") {
    return R"({"seq":1,"key":")" + key + R"(","1":224,)" + ohlcv + R"(,"7":)" +
           std::to_string(types::ToEpochMillis(At(iso))) + R"(,"8":20762})";
}

types::Bar Minute(const char* iso, const char* open, const char* high,
                  const char* low, const char* close, const char* volume) {
    return {At(iso),         testing::D(open),  testing::D(high),
            testing::D(low), testing::D(close), testing::D(volume)};
}

std::vector<SchwabKeyedQuote> Merge(SchwabLevelOneMerger& merger,
                                    const std::string& content) {
    return merger.Apply(*util::ParseJson(content),
                        *types::ParseIso8601("2026-11-04T20:00:00Z"));
}

TEST(SchwabLevelOneMerger, PartialUpdatesAccumulate) {
    SchwabLevelOneMerger merger;
    auto first = Merge(merger, R"([{"key":"SPY","delayed":false,
        "1":767.78,"2":767.8,"3":767.7842,"4":320,"5":680,"9":100,
        "32":"Normal","34":1793823953003,"35":1793823952844}])");
    ASSERT_EQ(first.size(), 1u);
    const auto& q = first[0].quote;
    EXPECT_EQ(first[0].key, "SPY");
    EXPECT_EQ(q.symbol, "SPY");
    EXPECT_EQ(q.venue, "schwab");
    EXPECT_EQ(*q.bid, testing::D("767.78"));
    EXPECT_EQ(*q.ask, testing::D("767.8"));
    EXPECT_EQ(*q.last, testing::D("767.7842"));
    EXPECT_EQ(*q.bid_size, testing::D("320"));  // shares, not lots
    EXPECT_EQ(*q.ask_size, testing::D("680"));
    EXPECT_EQ(*q.last_size, testing::D("100"));
    // The later of the quote and trade times.
    EXPECT_EQ(*q.exchange_time, types::FromEpochMillis(1793823953003));
    EXPECT_FALSE(q.last_taker_side);

    auto second =
        Merge(merger, R"([{"key":"SPY","2":767.79,"35":1793823954000}])");
    ASSERT_EQ(second.size(), 1u);
    EXPECT_EQ(*second[0].quote.bid, testing::D("767.78"));
    EXPECT_EQ(*second[0].quote.ask, testing::D("767.79"));
    EXPECT_EQ(*second[0].quote.exchange_time,
              types::FromEpochMillis(1793823954000));
}

TEST(SchwabLevelOneMerger, ZeroIsNoPrice) {
    SchwabLevelOneMerger merger;
    auto quotes = Merge(merger, R"([{"key":"AAPL","1":352.34,"2":0,"3":0}])");
    ASSERT_EQ(quotes.size(), 1u);
    EXPECT_TRUE(quotes[0].quote.bid);
    EXPECT_FALSE(quotes[0].quote.ask);
    EXPECT_FALSE(quotes[0].quote.last);
}

TEST(SchwabLevelOneMerger, ShareClassesAndUnknownSymbols) {
    SchwabLevelOneMerger merger;
    auto quotes = Merge(merger, R"([
        {"key":"BRK/B","1":578.78,"32":"Normal"},
        {"key":"ZZZZQ","1":0,"2":0,"32":"Unknown","34":0}])");
    ASSERT_EQ(quotes.size(), 1u);
    EXPECT_EQ(quotes[0].key, "BRK/B");
    EXPECT_EQ(quotes[0].quote.symbol, "BRK.B");
    // An unknown key stays silent, even without field 32.
    EXPECT_TRUE(Merge(merger, R"([{"key":"ZZZZQ","8":0}])").empty());
}

TEST(SchwabLevelOneMerger, ResetAndForgetDropTheState) {
    SchwabLevelOneMerger merger;
    Merge(merger, R"([{"key":"SPY","1":1},{"key":"AAPL","1":2}])");
    merger.Forget("SPY");
    EXPECT_FALSE(Merge(merger, R"([{"key":"SPY","2":3}])")[0].quote.bid);
    EXPECT_TRUE(Merge(merger, R"([{"key":"AAPL","2":3}])")[0].quote.bid);
    merger.Reset();
    EXPECT_FALSE(Merge(merger, R"([{"key":"AAPL","2":3}])")[0].quote.bid);
}

TEST(SchwabStreamerInfo, ParsedFromUserPreference) {
    auto info = ParseSchwabStreamerInfo(kUserPreference);
    ASSERT_OK(info);
    EXPECT_EQ(info->socket_url, "wss://streamer.test/ws");
    EXPECT_EQ(info->customer_id, "customer");
    EXPECT_EQ(info->correl_id, "correl");
    EXPECT_EQ(info->channel, "N9");
    EXPECT_EQ(info->function_id, "APIAPP");

    auto missing = ParseSchwabStreamerInfo(R"({"accounts":[]})");
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().category, types::ErrorCategory::kParse);
}

TEST_F(SchwabStreamerTest, LogsInThenSubscribes) {
    std::vector<types::Quote> quotes;
    auto sub = Subscribe({"SPY", "AAPL"}, quotes);
    ASSERT_EQ(mHttp->requests.size(), 1u);
    EXPECT_EQ(mHttp->requests[0].url,
              "https://api.test/trader/v1/userPreference");
    EXPECT_EQ(mWire->started, 1);
    EXPECT_EQ(mWire->url, "wss://streamer.test/ws");

    mWire->Fire(Event::kOpen);
    ASSERT_EQ(mWire->sent.size(), 1u);
    const json login = mWire->Last();
    EXPECT_EQ(login["service"], "ADMIN");
    EXPECT_EQ(login["command"], "LOGIN");
    EXPECT_EQ(login["SchwabClientCustomerId"], "customer");
    EXPECT_EQ(login["SchwabClientCorrelId"], "correl");
    EXPECT_EQ(login["parameters"]["Authorization"], "access");
    EXPECT_EQ(login["parameters"]["SchwabClientChannel"], "N9");
    EXPECT_EQ(login["parameters"]["SchwabClientFunctionId"], "APIAPP");

    // Nothing else is sent until the login succeeds.
    mWire->Respond("ADMIN", "LOGIN", 0, "server=s;status=NP");
    ASSERT_EQ(mWire->sent.size(), 2u);
    const auto& subs = mWire->Last();
    EXPECT_EQ(subs["service"], "LEVELONE_EQUITIES");
    EXPECT_EQ(subs["command"], "SUBS");
    EXPECT_EQ(subs["parameters"]["keys"], "AAPL,SPY");
    EXPECT_EQ(subs["parameters"]["fields"], kSchwabLevelOneFields);
    EXPECT_NE(login["requestid"], subs["requestid"]);

    mWire->Data(R"([{"key":"SPY","1":767.78,"2":767.8}])");
    ASSERT_EQ(quotes.size(), 1u);
    EXPECT_EQ(quotes[0].symbol, "SPY");
    EXPECT_EQ(quotes[0].received_time, mNow);
}

TEST_F(SchwabStreamerTest, SubscriptionsShareTheConnection) {
    std::vector<types::Quote> a, b;
    auto first = Subscribe({"SPY", "AAPL"}, a);
    mWire->Connect();
    auto second = Subscribe({"SPY", "MSFT"}, b);
    EXPECT_EQ(mWire->started, 1);
    EXPECT_EQ(mHttp->requests.size(), 1u);
    EXPECT_EQ(mWire->Last()["command"], "ADD");
    EXPECT_EQ(mWire->Last()["parameters"]["keys"], "MSFT");

    mWire->Data(R"([{"key":"SPY","1":1},{"key":"AAPL","1":2},
                    {"key":"MSFT","1":3}])");
    EXPECT_EQ(a.size(), 2u);
    EXPECT_EQ(b.size(), 2u);

    // Only the keys nobody else wants are dropped, and a stopped
    // subscription hears nothing more.
    first->Stop();
    EXPECT_EQ(mWire->Last()["command"], "UNSUBS");
    EXPECT_EQ(mWire->Last()["parameters"]["keys"], "AAPL");
    EXPECT_FALSE(mWire->Last()["parameters"].contains("fields"));
    mWire->Data(R"([{"key":"SPY","1":4}])");
    EXPECT_EQ(a.size(), 2u);
    EXPECT_EQ(b.size(), 3u);
    EXPECT_EQ(mWire->stopped, 0);

    // The last one closes the connection, and the next one connects again.
    second.reset();
    EXPECT_EQ(mWire->stopped, 1);
    auto third = Subscribe({"SPY"}, a);
    EXPECT_EQ(mWire->started, 2);
    EXPECT_EQ(mHttp->requests.size(), 2u);
}

TEST_F(SchwabStreamerTest, SubscribingBeforeTheLoginWaitsForIt) {
    std::vector<types::Quote> quotes;
    auto first = Subscribe({"SPY"}, quotes);
    mWire->Fire(Event::kOpen);
    auto second = Subscribe({"AAPL"}, quotes);
    EXPECT_EQ(mWire->sent.size(), 1u);  // the LOGIN only
    mWire->Respond("ADMIN", "LOGIN", 0);
    EXPECT_EQ(mWire->Last()["command"], "SUBS");
    EXPECT_EQ(mWire->Last()["parameters"]["keys"], "AAPL,SPY");
}

TEST_F(SchwabStreamerTest, ReconnectsAndSubscribesAfresh) {
    std::vector<types::Quote> quotes;
    auto sub = Subscribe({"SPY"}, quotes);
    mWire->Connect();
    mWire->Data(R"([{"key":"SPY","1":1,"2":2}])");
    mWire->Fire(Event::kClose, "1006 gone");

    mWire->Connect();
    EXPECT_EQ(mWire->Last()["command"], "SUBS");
    EXPECT_EQ(mWire->Last()["parameters"]["keys"], "SPY");
    // The old bid does not leak into the fresh snapshot.
    mWire->Data(R"([{"key":"SPY","2":3}])");
    ASSERT_EQ(quotes.size(), 2u);
    EXPECT_FALSE(quotes[1].bid);
}

TEST_F(SchwabStreamerTest, ADeniedLoginRenewsTheTokenOnce) {
    std::vector<types::Quote> quotes;
    auto sub = Subscribe({"SPY"}, quotes);
    mWire->Fire(Event::kOpen);
    mWire->Respond("ADMIN", "LOGIN", 3, "Login Denied.: token is invalid");
    EXPECT_EQ(mWire->closes, 1);
    EXPECT_TRUE(mWire->reconnecting);

    mWire->Fire(Event::kClose, "1000");
    mWire->Fire(Event::kOpen);
    EXPECT_EQ(mWire->Last()["parameters"]["Authorization"], "renewed");
    EXPECT_EQ(mTokenHttp->requests.size(), 1u);

    // Denied again: renewing will not help, so stop.
    mWire->Respond("ADMIN", "LOGIN", 3, "Login Denied.");
    EXPECT_FALSE(mWire->reconnecting);
    std::vector<types::Quote> more;
    auto refused = mStreamer->SubscribeQuotes(
        {"AAPL"}, [&more](const types::Quote& q) { more.push_back(q); });
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().category, types::ErrorCategory::kRejected);
}

TEST_F(SchwabStreamerTest, AnotherConnectionTakingOverStopsTheStreamer) {
    std::vector<types::Quote> quotes;
    auto sub = Subscribe({"SPY"}, quotes);
    mWire->Connect();
    TakeOver();
    EXPECT_FALSE(mWire->reconnecting);
    // A reconnect that IXWebSocket had already begun does not log in.
    const auto sent = mWire->sent.size();
    mWire->Fire(Event::kOpen);
    EXPECT_EQ(mWire->sent.size(), sent);
    auto refused =
        mStreamer->SubscribeQuotes({"AAPL"}, [](const types::Quote&) {});
    ASSERT_FALSE(refused);
    EXPECT_NE(refused.error().message.find("another streamer connection"),
              std::string::npos);
    EXPECT_NE(refused.error().message.find("takeover_reconnect_ms"),
              std::string::npos);
}

// Waits up to five seconds for `done`.
template <typename Done>
bool Eventually(Done done) {
    for (int i = 0; i < 500 && !done(); ++i) {
        std::this_thread::sleep_for(10ms);
    }
    return done();
}

// Schwab sends heartbeats whether anything trades or not, so a logged-in
// connection that hears nothing at all is dead though open: it is closed
// (and reconnects by itself). One that hears heartbeats is left alone.
TEST_F(SchwabStreamerTest, ASilentConnectionIsClosedButHeartbeatsKeepItOpen) {
    mStreamer = MakeStreamer({}, 300ms);
    std::vector<types::Quote> quotes;
    auto sub = Subscribe({"SPY"}, quotes);
    mWire->Connect();
    for (int i = 0; i < 10; ++i) {  // 500 ms of heartbeats every 50 ms
        std::this_thread::sleep_for(50ms);
        mWire->Fire(Event::kMessage, R"({"notify":[{"heartbeat":"1"}]})");
    }
    EXPECT_EQ(mWire->closes, 0);
    // Then silence.
    EXPECT_TRUE(Eventually([&] { return mWire->closes == 1; }));
    EXPECT_TRUE(mWire->reconnecting);  // its own reconnect does the rest
    // Logged out by it: nothing more until the next login.
    std::this_thread::sleep_for(500ms);
    EXPECT_EQ(mWire->closes, 1);
    mWire->Connect();
    EXPECT_EQ(mWire->Last()["command"], "SUBS");
}

// STOP_STREAMING (code 30) would leave the connection up and silent: it is
// closed, and reconnected after a wait rather than at once.
TEST_F(SchwabStreamerTest, AStopStreamingNoticeReconnectsAfterAWait) {
    std::vector<types::Quote> quotes;
    auto sub = Subscribe({"SPY"}, quotes);
    mWire->Connect();
    mWire->Fire(Event::kMessage,
                R"({"notify":[{"service":"ADMIN","timestamp":1,)"
                R"("content":{"code":30,"msg":"Stop streaming"}}]})");
    EXPECT_EQ(mWire->closes, 1);
    EXPECT_FALSE(mWire->reconnecting);
    // Not halted: new subscriptions are still taken.
    std::vector<types::Quote> more;
    EXPECT_NE(Subscribe({"AAPL"}, more), nullptr);
    // A reconnect under way does not log in before the wait is over.
    const auto sent = mWire->sent.size();
    mWire->Fire(Event::kOpen);
    EXPECT_EQ(mWire->sent.size(), sent);
    EXPECT_EQ(mWire->started, 1);  // the new connection comes after 30 s
}

TEST_F(SchwabStreamerTest, WithADelayATakeoverReconnectsAfterIt) {
    mStreamer = MakeStreamer(50ms);
    std::vector<types::Quote> quotes;
    auto sub = Subscribe({"SPY"}, quotes);
    mWire->Connect();
    TakeOver();
    EXPECT_FALSE(mWire->reconnecting);  // IXWebSocket's own reconnect is off

    // A reconnect already under way does not log in during the wait.
    const auto old_handler = mWire->handler;
    const auto sent = mWire->sent.size();
    mWire->Fire(Event::kOpen);
    EXPECT_EQ(mWire->sent.size(), sent);
    EXPECT_EQ(mWire->closes, 1);
    // Subscribing meanwhile works, and waits for the login.
    std::vector<types::Quote> more;
    auto second = Subscribe({"AAPL"}, more);
    EXPECT_EQ(mWire->sent.size(), sent);

    ASSERT_TRUE(Eventually([&] { return mWire->started == 2; }));
    EXPECT_TRUE(Eventually([&] { return mWire->stopped == 1; }));
    mWire->Connect();
    EXPECT_EQ(mWire->sent[sent]["command"], "LOGIN");
    EXPECT_EQ(mWire->Last()["command"], "SUBS");
    EXPECT_EQ(mWire->Last()["parameters"]["keys"], "AAPL,SPY");

    // The old socket's events no longer count.
    const auto now_sent = mWire->sent.size();
    old_handler(Event::kOpen, "");
    EXPECT_EQ(mWire->sent.size(), now_sent);
}

TEST_F(SchwabStreamerTest, StoppingDuringTheWaitCancelsTheReconnect) {
    mStreamer = MakeStreamer(50ms);
    std::vector<types::Quote> quotes;
    auto sub = Subscribe({"SPY"}, quotes);
    mWire->Connect();
    TakeOver();
    sub.reset();
    EXPECT_EQ(mWire->stopped, 1);
    std::this_thread::sleep_for(150ms);
    EXPECT_EQ(mWire->started, 1);
}

TEST_F(SchwabStreamerTest, HeartbeatsAndFailedCommandsAreNotQuotes) {
    std::vector<types::Quote> quotes;
    auto sub = Subscribe({"SPY"}, quotes);
    mWire->Connect();
    mWire->Fire(Event::kMessage, R"({"notify":[{"heartbeat":"1"}]})");
    mWire->Respond("LEVELONE_EQUITIES", "SUBS", 22, "failed");
    mWire->Fire(Event::kMessage, "not json");
    EXPECT_TRUE(quotes.empty());
    EXPECT_TRUE(mWire->reconnecting);
}

TEST_F(SchwabStreamerTest, AFailedUserPreferenceIsReturned) {
    mHttp->reply = [](const net::HttpRequest&) {
        return types::Result<net::HttpResponse>(
            net::HttpResponse{403, R"({"errors":[]})", {}});
    };
    auto sub = mStreamer->SubscribeQuotes({"SPY"}, [](const types::Quote&) {});
    ASSERT_FALSE(sub);
    EXPECT_NE(sub.error().message.find("userPreference"), std::string::npos);
    EXPECT_EQ(mWire->started, 0);
}

TEST_F(SchwabStreamerTest, TheDataSourceMapsSymbols) {
    SchwabDataSource::Options options;
    options.trader_url = "https://api.test/trader/v1";
    options.make_socket = [this] {
        return std::make_unique<FakeSocket>(mWire);
    };
    SchwabDataSource source(mApi, options);
    EXPECT_FALSE(source.StreamQuotes({}, [](const types::Quote&) {}));
    EXPECT_FALSE(source.StreamQuotes({"SPY"}, nullptr));

    std::vector<types::Quote> quotes;
    auto sub = source.StreamQuotes(
        {"brk.b", "SPY"},
        [&quotes](const types::Quote& q) { quotes.push_back(q); });
    ASSERT_OK(sub);
    mWire->Connect();
    EXPECT_EQ(mWire->Last()["parameters"]["keys"], "BRK/B,SPY");
    mWire->Data(R"([{"key":"BRK/B","1":578.78}])");
    ASSERT_EQ(quotes.size(), 1u);
    EXPECT_EQ(quotes[0].symbol, "BRK.B");
}

TEST(SchwabChartBars, ParsedFromALiveMessage) {
    // As CHART_EQUITY sent it on Nov 5 2026 (volume may be fractional).
    const auto bars = ParseSchwabChartBars(*util::ParseJson(R"([
        {"seq":1767,"key":"SPY","1":225,"2":744.93,"3":745.11,"4":743.2,
         "5":743.2,"6":244814.775721,"7":1793893500000,"8":20762},
        {"seq":167,"key":"BRK/B","1":225,"2":582.42,"3":583.2,"4":582.42}])"));
    ASSERT_EQ(bars.size(), 1u);  // BRK/B has no close, volume or time
    EXPECT_EQ(bars[0].key, "SPY");
    const auto& bar = bars[0].bar;
    EXPECT_EQ(bar.open_time, At("2026-11-05T15:45:00Z"));
    EXPECT_EQ(bar.open, testing::D("744.93"));
    EXPECT_EQ(bar.high, testing::D("745.11"));
    EXPECT_EQ(bar.low, testing::D("743.2"));
    EXPECT_EQ(bar.close, testing::D("743.2"));
    EXPECT_EQ(bar.volume, testing::D("244814.775721"));
}

TEST(SchwabBarAggregator, OneMinuteBarsPassThroughOnce) {
    SchwabBarAggregator one(1min);
    const auto bar = Minute("2026-11-05T15:44:00Z", "1", "2", "1", "2", "5");
    const auto out = one.Add(bar);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].open_time, bar.open_time);
    EXPECT_EQ(out[0].close, bar.close);
    // Sent again (as on every subscribe), even after a reconnect: ignored.
    EXPECT_TRUE(one.Add(bar).empty());
    one.Reset();
    EXPECT_TRUE(one.Add(bar).empty());
    EXPECT_EQ(
        one.Add(Minute("2026-11-05T15:45:00Z", "2", "2", "2", "2", "1")).size(),
        1u);
}

TEST(SchwabBarAggregator, BuildsLongerBarsOnlyFromWholeIntervals) {
    SchwabBarAggregator five(5min);
    // Joined at 15:43: 15:40's bar would lack 15:40 to 15:42.
    EXPECT_TRUE(
        five.Add(Minute("2026-11-05T15:43:00Z", "1", "1", "1", "1", "1"))
            .empty());
    EXPECT_TRUE(
        five.Add(Minute("2026-11-05T15:44:00Z", "1", "1", "1", "1", "1"))
            .empty());
    // 15:45 to 15:49, with no trades at 15:47: it closes with its last minute.
    EXPECT_TRUE(
        five.Add(Minute("2026-11-05T15:45:00Z", "10", "12", "9", "11", "100"))
            .empty());
    EXPECT_TRUE(
        five.Add(Minute("2026-11-05T15:46:00Z", "11", "15", "10", "14", "50"))
            .empty());
    EXPECT_TRUE(
        five.Add(Minute("2026-11-05T15:48:00Z", "14", "14", "8", "9", "25.5"))
            .empty());
    const auto out =
        five.Add(Minute("2026-11-05T15:49:00Z", "9", "10", "9", "10", "4.5"));
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].open_time, At("2026-11-05T15:45:00Z"));
    EXPECT_EQ(out[0].open, testing::D("10"));
    EXPECT_EQ(out[0].high, testing::D("15"));
    EXPECT_EQ(out[0].low, testing::D("8"));
    EXPECT_EQ(out[0].close, testing::D("10"));
    EXPECT_EQ(out[0].volume, testing::D("180"));

    // 15:50's last minute has no trades: it closes with 15:55's first.
    EXPECT_TRUE(
        five.Add(Minute("2026-11-05T15:51:00Z", "5", "6", "4", "6", "1"))
            .empty());
    const auto late =
        five.Add(Minute("2026-11-05T15:56:00Z", "7", "7", "7", "7", "1"));
    ASSERT_EQ(late.size(), 1u);
    EXPECT_EQ(late[0].open_time, At("2026-11-05T15:50:00Z"));
    EXPECT_EQ(late[0].close, testing::D("6"));

    // After a reconnect the bar under way is dropped (minutes may be
    // missing), and so is the next partial one.
    five.Reset();
    EXPECT_TRUE(
        five.Add(Minute("2026-11-05T15:59:00Z", "1", "1", "1", "1", "1"))
            .empty());
    EXPECT_TRUE(
        five.Add(Minute("2026-11-05T16:00:00Z", "1", "1", "1", "1", "1"))
            .empty());
    EXPECT_EQ(five.Add(Minute("2026-11-05T16:04:00Z", "1", "1", "1", "1", "1"))
                  .size(),
              1u);
}

TEST_F(SchwabStreamerTest, BarsAndQuotesShareTheConnection) {
    std::vector<types::Quote> quotes;
    auto quote_sub = Subscribe({"SPY"}, quotes);
    std::vector<std::pair<types::Symbol, types::Bar>> bars;
    auto bar_sub = mStreamer->SubscribeBars(
        {"BRK/B", "SPY"}, types::Timeframe::kM1,
        [&bars](const types::Symbol& symbol, types::Timeframe tf,
                const types::Bar& bar) {
            EXPECT_EQ(tf, types::Timeframe::kM1);
            bars.emplace_back(symbol, bar);
        });
    ASSERT_OK(bar_sub);
    mWire->Connect();
    EXPECT_EQ(mWire->started, 1);
    ASSERT_EQ(mWire->sent.size(), 3u);  // LOGIN, then a SUBS per service
    EXPECT_EQ(mWire->sent[1]["service"], "LEVELONE_EQUITIES");
    EXPECT_EQ(mWire->sent[2]["service"], "CHART_EQUITY");
    EXPECT_EQ(mWire->sent[2]["command"], "SUBS");
    EXPECT_EQ(mWire->sent[2]["parameters"]["keys"], "BRK/B,SPY");
    EXPECT_EQ(mWire->sent[2]["parameters"]["fields"], kSchwabChartFields);

    mWire->Chart("[" + ChartItem("BRK/B", "2026-11-05T15:44:00Z") + "]");
    ASSERT_EQ(bars.size(), 1u);
    EXPECT_EQ(bars[0].first, "BRK.B");
    EXPECT_EQ(bars[0].second.close, testing::D("11"));
    EXPECT_TRUE(quotes.empty());

    // A later subscription on a logged-in connection ADDs its new keys.
    auto more = mStreamer->SubscribeBars(
        {"AAPL", "SPY"}, types::Timeframe::kM5,
        [](const types::Symbol&, types::Timeframe, const types::Bar&) {});
    ASSERT_OK(more);
    EXPECT_EQ(mWire->Last()["service"], "CHART_EQUITY");
    EXPECT_EQ(mWire->Last()["command"], "ADD");
    EXPECT_EQ(mWire->Last()["parameters"]["keys"], "AAPL");

    // Stopping the bars UNSUBS only the chart keys nobody else wants.
    (*bar_sub)->Stop();
    EXPECT_EQ(mWire->Last()["service"], "CHART_EQUITY");
    EXPECT_EQ(mWire->Last()["command"], "UNSUBS");
    EXPECT_EQ(mWire->Last()["parameters"]["keys"], "BRK/B");
    mWire->Chart("[" + ChartItem("BRK/B", "2026-11-05T15:45:00Z") + "]");
    EXPECT_EQ(bars.size(), 1u);
    EXPECT_EQ(mWire->stopped, 0);
}

TEST_F(SchwabStreamerTest, AReconnectDropsTheBarUnderWay) {
    std::vector<types::Bar> bars;
    auto sub = mStreamer->SubscribeBars(
        {"SPY"}, types::Timeframe::kM5,
        [&bars](const types::Symbol&, types::Timeframe, const types::Bar& b) {
            bars.push_back(b);
        });
    ASSERT_OK(sub);
    mWire->Connect();
    // The subscribe's last closed minute, 15:44, ends 15:40's bar, which
    // began before the stream did.
    mWire->Chart("[" + ChartItem("SPY", "2026-11-05T15:44:00Z") + "]");
    mWire->Chart("[" + ChartItem("SPY", "2026-11-05T15:45:00Z") + "]");
    mWire->Chart("[" + ChartItem("SPY", "2026-11-05T15:46:00Z") + "]");
    EXPECT_TRUE(bars.empty());
    mWire->Fire(Event::kClose, "1006 gone");

    mWire->Connect();
    EXPECT_EQ(mWire->Last()["service"], "CHART_EQUITY");
    EXPECT_EQ(mWire->Last()["command"], "SUBS");
    // 15:47 was missed: 15:45's bar is not given.
    mWire->Chart("[" + ChartItem("SPY", "2026-11-05T15:48:00Z") + "]");
    mWire->Chart("[" + ChartItem("SPY", "2026-11-05T15:49:00Z") + "]");
    EXPECT_TRUE(bars.empty());
    for (const char* minute : {"2026-11-05T15:50:00Z", "2026-11-05T15:51:00Z",
                               "2026-11-05T15:52:00Z", "2026-11-05T15:53:00Z",
                               "2026-11-05T15:54:00Z"}) {
        mWire->Chart("[" + ChartItem("SPY", minute) + "]");
    }
    ASSERT_EQ(bars.size(), 1u);
    EXPECT_EQ(bars[0].open_time, At("2026-11-05T15:50:00Z"));
    EXPECT_EQ(bars[0].volume, testing::D("500"));
}

TEST_F(SchwabStreamerTest, TheDataSourceStreamsBarsInTheSession) {
    SchwabDataSource::Options options;
    options.trader_url = "https://api.test/trader/v1";
    options.make_socket = [this] {
        return std::make_unique<FakeSocket>(mWire);
    };
    SchwabDataSource source(mApi, options);
    const auto ignore = [](const types::Symbol&, types::Timeframe,
                           const types::Bar&) {};
    EXPECT_FALSE(source.StreamBars({}, types::Timeframe::kM1, ignore));
    EXPECT_FALSE(source.StreamBars({"SPY"}, types::Timeframe::kM1, nullptr));
    const auto hourly =
        source.StreamBars({"SPY"}, types::Timeframe::kH1, ignore);
    ASSERT_FALSE(hourly);
    EXPECT_EQ(hourly.error().category, types::ErrorCategory::kUnsupported);

    std::vector<std::pair<types::Symbol, types::Bar>> bars;
    auto sub = source.StreamBars(
        {"brk.b"}, types::Timeframe::kM1,
        [&bars](const types::Symbol& symbol, types::Timeframe,
                const types::Bar& bar) { bars.emplace_back(symbol, bar); });
    ASSERT_OK(sub);
    mWire->Connect();
    EXPECT_EQ(mWire->Last()["parameters"]["keys"], "BRK/B");
    // 09:29 and 16:00 New York time are outside the regular session.
    mWire->Chart("[" + ChartItem("BRK/B", "2026-11-05T14:29:00Z") + "," +
                 ChartItem("BRK/B", "2026-11-05T14:30:00Z") + "," +
                 ChartItem("BRK/B", "2026-11-05T21:00:00Z") + "]");
    ASSERT_EQ(bars.size(), 1u);
    EXPECT_EQ(bars[0].first, "BRK.B");
    EXPECT_EQ(bars[0].second.open_time, At("2026-11-05T14:30:00Z"));
}

TEST_F(SchwabStreamerTest, WithExtendedHoursEveryBarIsStreamed) {
    SchwabDataSource::Options options;
    options.extended_hours = true;
    options.trader_url = "https://api.test/trader/v1";
    options.make_socket = [this] {
        return std::make_unique<FakeSocket>(mWire);
    };
    SchwabDataSource source(mApi, options);
    int bars = 0;
    auto sub = source.StreamBars({"SPY"}, types::Timeframe::kM1,
                                 [&bars](const types::Symbol&, types::Timeframe,
                                         const types::Bar&) { ++bars; });
    ASSERT_OK(sub);
    mWire->Connect();
    mWire->Chart("[" + ChartItem("SPY", "2026-11-05T12:00:00Z") + "," +
                 ChartItem("SPY", "2026-11-05T22:00:00Z") + "]");
    EXPECT_EQ(bars, 2);
}

TEST_F(SchwabStreamerTest, OnlyFixedTimeframesAreBuilt) {
    const auto daily = mStreamer->SubscribeBars(
        {"SPY"}, types::Timeframe::kD1,
        [](const types::Symbol&, types::Timeframe, const types::Bar&) {});
    // A day is whole minutes; only months are not fixed.
    EXPECT_TRUE(daily);
    const auto monthly = mStreamer->SubscribeBars(
        {"SPY"}, types::Timeframe::kMo1,
        [](const types::Symbol&, types::Timeframe, const types::Bar&) {});
    ASSERT_FALSE(monthly);
    EXPECT_EQ(monthly.error().category, types::ErrorCategory::kInvalidArgument);
}

}  // namespace
TEST(SchwabLevelOneMerger, ASymbolUnknownBeforeAReconnectIsTriedAgain) {
    SchwabLevelOneMerger merger;
    EXPECT_TRUE(
        Merge(merger, R"([{"key":"SPY","32":"Unknown","1":0,"2":0}])").empty());
    EXPECT_TRUE(Merge(merger, R"([{"key":"SPY","1":767.78}])").empty());
    merger.Reset();  // a new connection
    EXPECT_EQ(Merge(merger, R"([{"key":"SPY","1":767.78}])").size(), 1u);
}

}  // namespace kestrel::data
