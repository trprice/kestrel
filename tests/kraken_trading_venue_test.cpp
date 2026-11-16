#include "kestrel/trading/kraken_trading_venue.hpp"

#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "kestrel/backtest/replay_data_source.hpp"
#include "kestrel/util/url.hpp"
#include "test_helpers.hpp"

namespace {

using namespace kestrel;
using kestrel::testing::D;
using namespace std::chrono_literals;

// Official Kraken documentation test private key (base64).
constexpr const char* kTestSecret =
    "kQH5HW/8p1uGOVjbgWA7FunAmGO8lsSUXNsu3eow76sz84Q18fWxnyRzBHCd3pd5nE9qa99HAZ"
    "tuZuj6F1huXg==";
constexpr const char* kTestKey = "kraken-secret-key-value";

class FakeHttp : public net::HttpClient {
   public:
    void Push(std::string body, int status = 200) {
        std::lock_guard lock(mMutex);
        mScript.push_back(net::HttpResponse{status, std::move(body), {}});
    }
    void PushError(types::Error error) {
        std::lock_guard lock(mMutex);
        mScript.push_back(std::unexpected(std::move(error)));
    }

    std::vector<net::HttpRequest> Requests() const {
        std::lock_guard lock(mMutex);
        return mRequests;
    }
    void ClearRequests() {
        std::lock_guard lock(mMutex);
        mRequests.clear();
    }

    types::Result<net::HttpResponse> Send(const net::HttpRequest& r) override {
        std::lock_guard lock(mMutex);
        mRequests.push_back(r);
        if (mScript.empty()) {
            return types::MakeError(types::ErrorCategory::kInternal,
                                    "unscripted request to " + r.url);
        }
        auto next = std::move(mScript.front());
        mScript.pop_front();
        return next;
    }

   private:
    mutable std::mutex mMutex;
    std::deque<types::Result<net::HttpResponse>> mScript;
    std::vector<net::HttpRequest> mRequests;
};

// Decodes a simple application/x-www-form-urlencoded body for assertions.
std::map<std::string, std::string> ParseForm(std::string_view body) {
    std::map<std::string, std::string> out;
    while (!body.empty()) {
        const auto amp = body.find('&');
        const auto part = body.substr(0, amp);
        const auto eq = part.find('=');
        if (eq != std::string_view::npos) {
            out.emplace(util::UrlDecode(part.substr(0, eq)),
                        util::UrlDecode(part.substr(eq + 1)));
        }
        if (amp == std::string_view::npos) {
            break;
        }
        body.remove_prefix(amp + 1);
    }
    return out;
}

void ExpectNoSecretsIn(std::string_view text) {
    EXPECT_EQ(text.find(kTestKey), std::string_view::npos) << text;
    EXPECT_EQ(text.find(kTestSecret), std::string_view::npos) << text;
}

class KrakenTradingVenueTest : public ::testing::Test {
   protected:
    KrakenTradingVenueTest() {
        data->SetSymbolInfo(
            {"BTC/USD", D("0.1"), D("0.0001"), D("0.0001"), D("0.5")});
        data->SetSymbolInfo(
            {"ETH/USD", D("0.01"), D("0.001"), D("0.001"), D("0.5")});
    }

    std::unique_ptr<trading::KrakenTradingVenue> Make(
        std::vector<types::Symbol> symbols = {"BTC/USD"},
        config::KrakenCredentials creds = {kTestKey, kTestSecret}) {
        trading::KrakenTradingVenue::Options opts;
        opts.rest_base_url = "https://api.kraken.test";
        opts.counter_per_second = 10'000;
        opts.counter_burst = 10'000;
        opts.orders_per_second = 10'000;
        opts.order_burst = 10'000;
        opts.retry.initial_delay = 5ms;
        opts.retry.jitter = 0;
        opts.clock = [this] { return now; };
        opts.sleeper = [this](std::chrono::milliseconds d) {
            sleeps.push_back(d);
        };
        return std::make_unique<trading::KrakenTradingVenue>(
            std::move(creds), data, std::move(symbols), http, std::move(opts));
    }

    std::shared_ptr<FakeHttp> http = std::make_shared<FakeHttp>();
    std::shared_ptr<backtest::ReplayDataSource> data =
        std::make_shared<backtest::ReplayDataSource>();
    types::Timestamp now = types::FromEpochMillis(1'700'000'000'123);
    std::vector<std::chrono::milliseconds> sleeps;
};

TEST(KrakenSignature, MatchesOfficialKrakenTestVector) {
    auto sign = trading::SignKrakenRequest(
        "/0/private/AddOrder", "1616492376594",
        "nonce=1616492376594&ordertype=limit&pair=XBTUSD&price=37500&type=buy&"
        "volume=1.25",
        kTestSecret);
    ASSERT_OK(sign);
    EXPECT_EQ(
        *sign,
        "4/dpxb3iT4tp/ZCVEwSnEsLxx0bqyhLpdfOpc6fn7OR8+UClSV5n9E6aSS8MPtnRfp32bA"
        "b0nmbRn6H8ndwLUQ==");
}

TEST(KrakenSignature, RejectsEmptyOrMalformedSecret) {
    EXPECT_EQ(
        trading::SignKrakenRequest("/0/private/BalanceEx", "1", "nonce=1", "")
            .error()
            .category,
        types::ErrorCategory::kAuth);
    EXPECT_EQ(trading::SignKrakenRequest("/0/private/BalanceEx", "1", "nonce=1",
                                         "not-base64!")
                  .error()
                  .category,
              types::ErrorCategory::kAuth);
}

TEST_F(KrakenTradingVenueTest,
       SignsEveryRequestWithStrictlyIncreasingNoncesAcrossThreads) {
    auto venue = Make();
    constexpr int kThreads = 4;
    constexpr int kPerThread = 10;
    for (int i = 0; i < kThreads * kPerThread; ++i) {
        http->Push(R"({"error":[],"result":{}})");
    }

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < kPerThread; ++i) {
                EXPECT_TRUE(venue->GetBalances().has_value());
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }

    const auto seen = http->Requests();
    ASSERT_EQ(seen.size(), static_cast<std::size_t>(kThreads * kPerThread));
    std::uint64_t prev = 0;
    for (const auto& req : seen) {
        EXPECT_EQ(req.method, net::HttpMethod::kPost);
        EXPECT_EQ(req.url, "https://api.kraken.test/0/private/BalanceEx");
        EXPECT_EQ(req.headers.at("Content-Type"),
                  "application/x-www-form-urlencoded");
        EXPECT_EQ(req.headers.at("API-Key"), kTestKey);
        const auto form = ParseForm(req.body);
        const auto nonce = std::stoull(form.at("nonce"));
        EXPECT_GT(nonce, prev);
        prev = nonce;
        auto expected = trading::SignKrakenRequest(
            "/0/private/BalanceEx", form.at("nonce"), req.body, kTestSecret);
        ASSERT_OK(expected);
        EXPECT_EQ(req.headers.at("API-Sign"), *expected);
    }
}

TEST_F(KrakenTradingVenueTest,
       InvalidCredentialsFailWithoutNetworkCallOrSecretLeak) {
    auto bad_secret = Make({"BTC/USD"}, {kTestKey, "bad-secret-value"});
    auto r1 = bad_secret->GetBalances();
    ASSERT_FALSE(r1);
    EXPECT_EQ(r1.error().category, types::ErrorCategory::kAuth);
    EXPECT_EQ(r1.error().ToString().find("bad-secret-value"),
              std::string::npos);
    ExpectNoSecretsIn(r1.error().ToString());

    auto empty_key = Make({"BTC/USD"}, {"", kTestSecret});
    auto r2 = empty_key->GetBalances();
    ASSERT_FALSE(r2);
    EXPECT_EQ(r2.error().category, types::ErrorCategory::kAuth);
    ExpectNoSecretsIn(r2.error().ToString());
    EXPECT_TRUE(http->Requests().empty());
}

TEST_F(KrakenTradingVenueTest, PlacesMarketLimitPostOnlyFokAndStopOrders) {
    auto venue = Make();

    // 1. Limit post-only buy: price rounded down to 0.1 tick, quantity down to
    // 0.0001 lot.
    http->Push(
        R"({"error":[],"result":{"descr":{"order":"buy"},"txid":["O1"]}})");
    types::OrderRequest limit;
    limit.symbol = "BTC/USD";
    limit.side = types::Side::kBuy;
    limit.type = types::OrderType::kLimit;
    limit.quantity = D("0.12349");
    limit.limit_price = D("50000.19");
    limit.post_only = true;
    auto ack1 = venue->PlaceOrder(limit);
    ASSERT_OK(ack1);
    EXPECT_EQ(ack1->order_id, "O1");
    EXPECT_EQ(ack1->state, types::OrderState::kNew);
    EXPECT_LE(ack1->client_order_id.size(), 18u);
    EXPECT_EQ(ack1->client_order_id.rfind("kst", 0), 0u);

    auto f1 = ParseForm(http->Requests().back().body);
    EXPECT_EQ(f1.at("pair"), "BTC/USD");
    EXPECT_EQ(f1.at("type"), "buy");
    EXPECT_EQ(f1.at("ordertype"), "limit");
    EXPECT_EQ(f1.at("price"), "50000.1");
    EXPECT_EQ(f1.at("volume"), "0.1234");
    EXPECT_EQ(f1.at("timeinforce"), "GTC");
    EXPECT_EQ(f1.at("oflags"), "post,fciq");
    EXPECT_EQ(f1.at("cl_ord_id"), ack1->client_order_id);

    // 2. IOC limit sell with a caller-supplied UUID client_order_id.
    http->Push(R"({"error":[],"result":{"txid":["O2"]}})");
    types::OrderRequest ioc;
    ioc.symbol = "BTC/USD";
    ioc.side = types::Side::kSell;
    ioc.type = types::OrderType::kLimit;
    ioc.quantity = D("0.01");
    ioc.limit_price = D("51000.01");  // sell limit rounds up to 51000.1
    ioc.time_in_force = types::TimeInForce::kIoc;
    ioc.client_order_id = "6d1b345e-2821-40e2-ad83-4ecb18a06876";
    auto ack2 = venue->PlaceOrder(ioc);
    ASSERT_OK(ack2);
    EXPECT_EQ(ack2->order_id, "O2");
    EXPECT_EQ(ack2->client_order_id, ioc.client_order_id);
    auto f2 = ParseForm(http->Requests().back().body);
    EXPECT_EQ(f2.at("price"), "51000.1");
    EXPECT_EQ(f2.at("timeinforce"), "IOC");
    // A sell's fee is taken in the quote asset, as the fills report it.
    EXPECT_EQ(f2.at("oflags"), "fciq");

    // 3. FOK limit buy.
    http->Push(R"({"error":[],"result":{"txid":["O-FOK"]}})");
    types::OrderRequest fok = ioc;
    fok.side = types::Side::kBuy;
    fok.time_in_force = types::TimeInForce::kFok;
    fok.client_order_id = "kst-fok-1";
    ASSERT_OK(venue->PlaceOrder(fok));
    EXPECT_EQ(ParseForm(http->Requests().back().body).at("timeinforce"), "FOK");

    // 4. Stop-loss sell: trigger goes in `price`.
    http->Push(R"({"error":[],"result":{"txid":["O3"]}})");
    types::OrderRequest stop;
    stop.symbol = "BTC/USD";
    stop.side = types::Side::kSell;
    stop.type = types::OrderType::kStop;
    stop.quantity = D("0.05");
    stop.stop_price = D("48999.96");  // nearest tick: 49000.0
    auto ack3 = venue->PlaceOrder(stop);
    ASSERT_OK(ack3);
    auto f3 = ParseForm(http->Requests().back().body);
    EXPECT_EQ(f3.at("ordertype"), "stop-loss");
    EXPECT_EQ(f3.at("price"), "49000.0");
    EXPECT_EQ(f3.at("volume"), "0.0500");
    EXPECT_EQ(f3.at("oflags"), "fciq");

    // 5. Market buy.
    http->Push(R"({"error":[],"result":{"txid":["O4"]}})");
    types::OrderRequest market;
    market.symbol = "BTC/USD";
    market.side = types::Side::kBuy;
    market.type = types::OrderType::kMarket;
    market.quantity = D("0.01");
    ASSERT_OK(venue->PlaceOrder(market));
    auto f4 = ParseForm(http->Requests().back().body);
    EXPECT_EQ(f4.at("ordertype"), "market");
    EXPECT_FALSE(f4.contains("price"));
    EXPECT_EQ(f4.at("oflags"), "fciq");
}

TEST_F(KrakenTradingVenueTest, RejectsUnsupportedAndInvalidOrdersLocally) {
    auto venue = Make();

    types::OrderRequest day;
    day.symbol = "BTC/USD";
    day.quantity = D("0.01");
    day.time_in_force = types::TimeInForce::kDay;
    EXPECT_EQ(venue->PlaceOrder(day).error().category,
              types::ErrorCategory::kUnsupported);

    types::OrderRequest ioc_stop;
    ioc_stop.symbol = "BTC/USD";
    ioc_stop.type = types::OrderType::kStop;
    ioc_stop.stop_price = D("49000");
    ioc_stop.quantity = D("0.01");
    ioc_stop.time_in_force = types::TimeInForce::kIoc;
    EXPECT_EQ(venue->PlaceOrder(ioc_stop).error().category,
              types::ErrorCategory::kUnsupported);

    types::OrderRequest bad_client_id;
    bad_client_id.symbol = "BTC/USD";
    bad_client_id.quantity = D("0.01");
    bad_client_id.client_order_id = "this-id-is-longer-than-18-chars";
    EXPECT_EQ(venue->PlaceOrder(bad_client_id).error().category,
              types::ErrorCategory::kInvalidArgument);

    types::OrderRequest below_costmin;
    below_costmin.symbol = "BTC/USD";
    below_costmin.type = types::OrderType::kLimit;
    below_costmin.limit_price = D("1000");
    below_costmin.quantity = D("0.0001");  // 0.10 < costmin 0.5
    EXPECT_EQ(venue->PlaceOrder(below_costmin).error().category,
              types::ErrorCategory::kRejected);

    EXPECT_TRUE(http->Requests().empty());
}

TEST_F(KrakenTradingVenueTest, TestOrderSendsValidateTrueAndPropagatesErrors) {
    auto venue = Make();
    http->Push(R"({"error":[],"result":{"descr":{"order":"buy"}}})");

    types::OrderRequest req;
    req.symbol = "BTC/USD";
    req.quantity = D("0.01");
    ASSERT_OK(venue->TestOrder(req));
    ASSERT_EQ(http->Requests().size(), 1u);
    EXPECT_EQ(ParseForm(http->Requests()[0].body).at("validate"), "true");

    http->Push(R"({"error":["EOrder:Insufficient funds"]})");
    auto err = venue->TestOrder(req);
    ASSERT_FALSE(err);
    EXPECT_EQ(err.error().category, types::ErrorCategory::kInsufficientFunds);
    ExpectNoSecretsIn(err.error().ToString());
}

TEST_F(KrakenTradingVenueTest,
       PlaceOrderNeverRetriesAndRecoversUnknownOutcome) {
    auto venue = Make();

    // 1. Rate-limit error on AddOrder is NOT retried.
    http->Push(R"({"error":["EOrder:Rate limit exceeded"]})");
    types::OrderRequest req;
    req.symbol = "BTC/USD";
    req.quantity = D("0.01");
    req.client_order_id = "kst-rate-1";
    auto rate = venue->PlaceOrder(req);
    ASSERT_FALSE(rate);
    EXPECT_EQ(rate.error().category, types::ErrorCategory::kRateLimit);
    EXPECT_EQ(http->Requests().size(), 1u);

    // 2. Timeout on AddOrder: ignores manual orders without cl_ord_id, ignores
    // other cl_ord_ids, ignores an old closed order from hours earlier that
    // reused the same cl_ord_id, and finds the new order filled (`closetm`) on
    // the second look.
    http->ClearRequests();
    http->PushError({types::ErrorCategory::kTimeout, "timed out", std::nullopt,
                     std::nullopt, std::nullopt});
    // Look 1: OpenOrders has a manual order (no cl_ord_id) and another order;
    // ClosedOrders has an old order with the same cl_ord_id from an hour ago!
    http->Push(R"({"error":[],"result":{"open":{
        "O-MANUAL":{"status":"open","opentm":1700000000.1,
                    "descr":{"pair":"XBTUSD","type":"buy","ordertype":"limit","price":"48000.0"},
                    "vol":"0.5","vol_exec":"0"},
        "O-OTHER":{"cl_ord_id":"other-id","status":"open","opentm":1700000000.1,
                   "descr":{"pair":"XBTUSD","type":"buy","ordertype":"limit","price":"48000.0"},
                   "vol":"0.5","vol_exec":"0"}
    }}})");
    http->Push(R"({"error":[],"result":{"closed":{
        "O-STALE":{"cl_ord_id":"kst-timeout-1","status":"closed",
                   "opentm":1699996000.0,"closetm":1699996005.0,
                   "descr":{"pair":"XBTUSD","type":"buy","ordertype":"market","price":"0"},
                   "vol":"0.01","vol_exec":"0.01","price":"49000.0"}
    }}})");
    // Look 2: empty in OpenOrders, found filled in ClosedOrders with `closetm`.
    http->Push(R"({"error":[],"result":{"open":{}}})");
    http->Push(R"({"error":[],"result":{"closed":{
        "O-FILLED":{"cl_ord_id":"kst-timeout-1","status":"closed",
                    "opentm":1700000000.25,"closetm":1700000001.5,
                    "descr":{"pair":"XBTUSD","type":"buy","ordertype":"market",
                             "price":"0","price2":"0"},
                    "vol":"0.01","vol_exec":"0.01","cost":"500.0","price":"50000.0"}
    }}})");

    req.client_order_id = "kst-timeout-1";
    auto recovered = venue->PlaceOrder(req);
    ASSERT_OK(recovered);
    EXPECT_EQ(recovered->order_id, "O-FILLED");
    EXPECT_EQ(recovered->client_order_id, "kst-timeout-1");
    EXPECT_EQ(recovered->state, types::OrderState::kFilled);
    EXPECT_EQ(recovered->time, types::FromEpochMillis(1'700'000'001'500));
    EXPECT_EQ(sleeps, std::vector<std::chrono::milliseconds>{1000ms});
    ASSERT_EQ(http->Requests().size(), 5u);
    EXPECT_EQ(http->Requests()[0].url,
              "https://api.kraken.test/0/private/AddOrder");
    EXPECT_EQ(http->Requests()[1].url,
              "https://api.kraken.test/0/private/OpenOrders");
    EXPECT_EQ(http->Requests()[2].url,
              "https://api.kraken.test/0/private/ClosedOrders");
    EXPECT_EQ(ParseForm(http->Requests()[2].body).at("start"), "1699999700");

    // Generated cl_ord_id omits `start` and recovers even when the local clock
    // runs 10 minutes ahead of Kraken's opentm.
    http->ClearRequests();
    http->Push(R"({"error":[],"result":{"descr":{"order":"buy"}}})");
    types::OrderRequest probe;
    probe.symbol = "BTC/USD";
    probe.quantity = D("0.01");
    ASSERT_OK(venue->TestOrder(probe));
    const std::string first_gen =
        ParseForm(http->Requests()[0].body).at("cl_ord_id");
    ASSERT_EQ(first_gen.size(), 9u);  // "kst" + 5-char tag + "1"
    const std::string next_gen = first_gen.substr(0, 8) + "2";

    http->ClearRequests();
    http->PushError({types::ErrorCategory::kTimeout, "timed out", std::nullopt,
                     std::nullopt, std::nullopt});
    http->Push(R"({"error":[],"result":{"open":{}}})");
    http->Push(R"({"error":[],"result":{"closed":{
        "O-SKEW":{"cl_ord_id":")" +
               next_gen + R"(","status":"closed",
                  "opentm":1699999400.0,"closetm":1699999401.0,
                  "descr":{"pair":"XBTUSD","type":"buy","ordertype":"market","price":"0"},
                  "vol":"0.01","vol_exec":"0.01","price":"50000.0"}
    }}})");
    auto from_skew = venue->PlaceOrder(probe);
    ASSERT_OK(from_skew);
    EXPECT_EQ(from_skew->order_id, "O-SKEW");
    EXPECT_FALSE(ParseForm(http->Requests()[2].body).contains("start"));

    // 3. Non-JSON 200 (proxy HTML) and missing-txid 200 both trigger lookup!
    http->ClearRequests();
    http->Push("<html>gateway hiccup</html>", 200);
    http->Push(R"({"error":[],"result":{"open":{
        "O-PROXY":{"cl_ord_id":"kst-proxy-1","status":"open","opentm":1700000000.5,
                   "descr":{"pair":"XBTUSD","type":"buy","ordertype":"limit","price":"50000.0"},
                   "vol":"0.01","vol_exec":"0"}
    }}})");
    req.type = types::OrderType::kLimit;
    req.limit_price = D("50000.0");
    req.client_order_id = "kst-proxy-1";
    auto from_html = venue->PlaceOrder(req);
    ASSERT_OK(from_html);
    EXPECT_EQ(from_html->order_id, "O-PROXY");

    http->ClearRequests();
    http->Push(R"({"error":[],"result":{"descr":{"order":"buy"}}})", 200);
    http->Push(R"({"error":[],"result":{"open":{
        "O-NOTXID":{"cl_ord_id":"kst-notxid-1","status":"open","opentm":1700000000.5,
                    "descr":{"pair":"XBTUSD","type":"buy","ordertype":"limit","price":"50000.0"},
                    "vol":"0.01","vol_exec":"0"}
    }}})");
    req.client_order_id = "kst-notxid-1";
    auto from_notxid = venue->PlaceOrder(req);
    ASSERT_OK(from_notxid);
    EXPECT_EQ(from_notxid->order_id, "O-NOTXID");

    // 4. EService:Unavailable triggers lookup, and if lookup itself fails with
    // auth/network error, reports that failure clearly.
    http->ClearRequests();
    http->Push(R"({"error":["EService:Unavailable"]})");
    http->Push(R"({"error":["EAPI:Invalid nonce"]})");
    req.client_order_id = "kst-unavail-1";
    auto unavail = venue->PlaceOrder(req);
    ASSERT_FALSE(unavail);
    EXPECT_EQ(unavail.error().category, types::ErrorCategory::kServer);
    EXPECT_NE(unavail.error().message.find(
                  "order kst-unavail-1 could not be looked up either"),
              std::string::npos);
    ExpectNoSecretsIn(unavail.error().ToString());
}

TEST_F(KrakenTradingVenueTest,
       CancelOrderResolvesFilledClosedTimeoutAndZeroCount) {
    auto venue = Make();

    // Direct cancel succeeds.
    http->Push(R"({"error":[],"result":{"count":1}})");
    auto c1 = venue->CancelOrder("O-OPEN");
    ASSERT_OK(c1);
    EXPECT_EQ(*c1, trading::CancelOutcome::kCancelled);

    // EOrder:Unknown order -> QueryOrders shows closed & filled ->
    // kAlreadyFilled.
    http->Push(R"({"error":["EOrder:Unknown order"]})");
    http->Push(R"({"error":[],"result":{"O-DONE":{
        "status":"closed","opentm":1700000000,"closetm":1700000005,
        "descr":{"pair":"XBTUSD","type":"sell","ordertype":"limit","price":"52000.0"},
        "vol":"0.1","vol_exec":"0.1","cost":"5200.0"
    }}})");
    auto c2 = venue->CancelOrder("O-DONE");
    ASSERT_OK(c2);
    EXPECT_EQ(*c2, trading::CancelOutcome::kAlreadyFilled);

    // count: 0 without pending -> QueryOrders shows post-only kRejected ->
    // kAlreadyClosed.
    http->Push(R"({"error":[],"result":{"count":0}})");
    http->Push(R"({"error":[],"result":{"O-CANC":{
        "status":"canceled","reason":"Post only order","opentm":1700000000,
        "descr":{"pair":"XBTUSD","type":"buy","ordertype":"limit","price":"50000.0"},
        "vol":"0.1","vol_exec":"0"
    }}})");
    auto c3 = venue->CancelOrder("O-CANC");
    ASSERT_OK(c3);
    EXPECT_EQ(*c3, trading::CancelOutcome::kAlreadyClosed);

    // Timeout on CancelOrder -> QueryOrders shows canceled -> kCancelled.
    http->PushError({types::ErrorCategory::kTimeout, "timed out", std::nullopt,
                     std::nullopt, std::nullopt});
    http->Push(R"({"error":[],"result":{"O-TIMED":{
        "status":"canceled","opentm":1700000000,"closetm":1700000002,
        "descr":{"pair":"XBTUSD","type":"buy","ordertype":"limit","price":"50000.0"},
        "vol":"0.1","vol_exec":"0"
    }}})");
    auto c4 = venue->CancelOrder("O-TIMED");
    ASSERT_OK(c4);
    EXPECT_EQ(*c4, trading::CancelOutcome::kCancelled);

    // Timeout on CancelOrder -> QueryOrders shows still open -> propagates
    // timeout error!
    http->PushError({types::ErrorCategory::kTimeout, "timed out", std::nullopt,
                     std::nullopt, std::nullopt});
    http->Push(R"({"error":[],"result":{"O-STILL":{
        "status":"open","opentm":1700000000,
        "descr":{"pair":"XBTUSD","type":"buy","ordertype":"limit","price":"50000.0"},
        "vol":"0.1","vol_exec":"0"
    }}})");
    EXPECT_EQ(venue->CancelOrder("O-STILL").error().category,
              types::ErrorCategory::kTimeout);

    // EOrder:Invalid order -> QueryOrders also invalid -> kNotFound.
    http->Push(R"({"error":["EOrder:Invalid order"]})");
    http->Push(R"({"error":["EOrder:Invalid order"]})");
    auto c5 = venue->CancelOrder("O-NOPE");
    ASSERT_FALSE(c5);
    EXPECT_EQ(c5.error().category, types::ErrorCategory::kNotFound);

    // Rejects empty or comma-separated IDs without calling out.
    const auto before = http->Requests().size();
    EXPECT_EQ(venue->CancelOrder("").error().category,
              types::ErrorCategory::kInvalidArgument);
    EXPECT_EQ(venue->CancelOrder("O1,O2").error().category,
              types::ErrorCategory::kInvalidArgument);
    EXPECT_EQ(http->Requests().size(), before);
}

TEST_F(KrakenTradingVenueTest,
       CancelAllCoversConfiguredSymbolsOnlyAndContinuesOnFailure) {
    auto venue = Make({"BTC/USD"});

    http->Push(R"({"error":[],"result":{"open":{
        "O-BTC-1":{"status":"open","opentm":1700000000,
                   "descr":{"pair":"XBTUSD","type":"buy","ordertype":"limit","price":"49000.0"},
                   "vol":"0.1","vol_exec":"0"},
        "O-BTC-2":{"status":"open","opentm":1700000001,
                   "descr":{"pair":"BTC/USD","type":"sell","ordertype":"stop-loss","price":"48000.0"},
                   "vol":"0.1","vol_exec":"0"},
        "O-ETH":{"status":"open","opentm":1700000002,
                 "descr":{"pair":"XETHZUSD","type":"buy","ordertype":"limit","price":"2000.0"},
                 "vol":"1.0","vol_exec":"0"},
        "O-EXOTIC":{"status":"open","opentm":1700000003,
                    "descr":{"pair":"BONKSOL","type":"buy","ordertype":"limit","price":"0.01"},
                    "vol":"100","vol_exec":"0"}
    }}})");
    http->Push(R"({"error":[],"result":{"count":1}})");   // O-BTC-1 cancelled
    http->Push(R"({"error":["EOrder:Unknown order"]})");  // O-BTC-2 filled!
    http->Push(R"({"error":[],"result":{"O-BTC-2":{
        "status":"closed","opentm":1700000001,"closetm":1700000004,
        "descr":{"pair":"BTC/USD","type":"sell","ordertype":"stop-loss","price":"48000.0"},
        "vol":"0.1","vol_exec":"0.1","price":"47990.0"
    }}})");

    auto cancelled = venue->CancelAll();
    ASSERT_OK(cancelled);
    EXPECT_EQ(*cancelled, std::vector<std::string>{"O-BTC-1"});

    // Partial failure: first cancel fails, second still runs and succeeds.
    http->Push(R"({"error":[],"result":{"open":{
        "O-A":{"status":"open","opentm":1700000000,
               "descr":{"pair":"XBTUSD","type":"buy","ordertype":"limit","price":"49000.0"},
               "vol":"0.1","vol_exec":"0"},
        "O-B":{"status":"open","opentm":1700000001,
               "descr":{"pair":"XBTUSD","type":"sell","ordertype":"limit","price":"51000.0"},
               "vol":"0.1","vol_exec":"0"}
    }}})");
    http->Push(R"({"error":["EOrder:Rate limit exceeded"]})");
    http->Push(R"({"error":[],"result":{"count":1}})");
    auto partial = venue->CancelAll();
    ASSERT_FALSE(partial);
    EXPECT_EQ(partial.error().category, types::ErrorCategory::kRateLimit);
    EXPECT_NE(
        partial.error().message.find("1 of 2 cancels failed; 1 cancelled"),
        std::string::npos);

    // Unscoped venue (empty symbols) treats an unrecognised pair as kParse.
    auto unscoped = Make({});
    http->Push(R"({"error":[],"result":{"open":{
        "O-EXOTIC":{"status":"open","opentm":1700000003,
                    "descr":{"pair":"BONKSOL","type":"buy","ordertype":"limit","price":"0.01"},
                    "vol":"100","vol_exec":"0"}
    }}})");
    EXPECT_EQ(unscoped->GetOpenOrders().error().category,
              types::ErrorCategory::kParse);
}

TEST_F(KrakenTradingVenueTest, ParsesOpenAndClosedOrdersViaGetOrder) {
    auto venue = Make();
    http->Push(R"({"error":[],"result":{"O-STOP":{
        "cl_ord_id":"kst-stop-1","status":"open","opentm":1700000010.5,
        "descr":{"pair":"XXBTZUSD","type":"sell","ordertype":"stop-loss",
                 "price":"48500.5","price2":"0"},
        "vol":"0.25","vol_exec":"0.10","cost":"4850.05","avg_price":"48500.5"
    }}})");
    auto stop = venue->GetOrder("O-STOP");
    ASSERT_OK(stop);
    EXPECT_EQ(stop->symbol, "BTC/USD");
    EXPECT_EQ(stop->side, types::Side::kSell);
    EXPECT_EQ(stop->type, types::OrderType::kStop);
    EXPECT_EQ(stop->state, types::OrderState::kPartiallyFilled);
    EXPECT_EQ(stop->quantity, D("0.25"));
    EXPECT_EQ(stop->filled_quantity, D("0.10"));
    EXPECT_EQ(stop->stop_price, D("48500.5"));
    EXPECT_FALSE(stop->limit_price.has_value());
    EXPECT_EQ(stop->average_fill_price, D("48500.5"));
    EXPECT_EQ(stop->created_time, types::FromEpochMillis(1'700'000'010'500));
    EXPECT_EQ(stop->updated_time, stop->created_time);

    // A post-only order Kraken cancelled because it would have taken is
    // refused, not cancelled; any other cancel stays a cancel.
    http->Push(R"({"error":[],"result":{"O-POST":{
        "status":"canceled","reason":"Post only order","opentm":1700000020,
        "closetm":1700000020.01,
        "descr":{"pair":"XBTUSD","type":"buy","ordertype":"limit",
                 "price":"50000.0"},
        "vol":"0.1","vol_exec":"0"
    }}})");
    auto post = venue->GetOrder("O-POST");
    ASSERT_OK(post);
    EXPECT_EQ(post->state, types::OrderState::kRejected);
    EXPECT_EQ(post->reason, "Post only order");
    http->Push(R"({"error":[],"result":{"O-USER":{
        "status":"canceled","reason":"User requested","opentm":1700000020,
        "descr":{"pair":"XBTUSD","type":"buy","ordertype":"limit",
                 "price":"50000.0"},
        "vol":"0.1","vol_exec":"0"
    }}})");
    auto user = venue->GetOrder("O-USER");
    ASSERT_OK(user);
    EXPECT_EQ(user->state, types::OrderState::kCancelled);

    // Defensive guard: a partially filled canceled order is never kRejected.
    http->Push(R"({"error":[],"result":{"O-PARTIAL-CANC":{
        "status":"canceled","reason":"Post only order","opentm":1700000020,
        "descr":{"pair":"XBTUSD","type":"buy","ordertype":"limit",
                 "price":"50000.0"},
        "vol":"0.1","vol_exec":"0.05"
    }}})");
    auto partial_canc = venue->GetOrder("O-PARTIAL-CANC");
    ASSERT_OK(partial_canc);
    EXPECT_EQ(partial_canc->state, types::OrderState::kCancelled);
}

TEST_F(KrakenTradingVenueTest,
       GetFillsPagesWithCursorAttachesClientIdAndRejectsStalledCursor) {
    auto venue = Make({"BTC/USD"});

    // Place an order first so its client_order_id is remembered for its fills.
    http->Push(R"({"error":[],"result":{"txid":["O-MINE"]}})");
    types::OrderRequest placed;
    placed.symbol = "BTC/USD";
    placed.quantity = D("0.2");
    placed.client_order_id = "kst-my-order";
    ASSERT_OK(venue->PlaceOrder(placed));

    const auto since = types::FromEpochMillis(1'700'000'100'500);

    // Page 0: ETH/USD trades >= since (filtered out by symbol), `count: null`
    // as Kraken returns in cursor mode, and `cursor.next` pointing to Page 1.
    std::string page0;
    for (int i = 0; i < 50; ++i) {
        if (!page0.empty()) page0 += ",";
        const std::string id = "T-ETH-" + std::to_string(i);
        page0 +=
            "\"" + id +
            R"(":{"ordertxid":"O-ETH","pair":"XETHZUSD","time":1700000105.0,)"
            R"("type":"buy","price":"2000.0","vol":"0.1","fee":"0.5","maker":false})";
    }
    http->Push(R"({"error":[],"result":{"trades":{)" + page0 +
               R"(},"count":null,"cursor":{"next":"cur-page-1"}}})");

    // Page 1 (`cursor=cur-page-1`): two BTC/USD trades >= since, one 100 ms
    // before, and `cursor.next: null` (final page).
    http->Push(R"({"error":[],"result":{"trades":{
        "T-Z-LATER":{"ordertxid":"O-MINE","pair":"XXBTZUSD","time":1700000102.250,
                     "type":"buy","price":"50010.0","vol":"0.12","fee":"1.50","maker":false},
        "T-A-EARLIER":{"ordertxid":"O-MINE","pair":"XBTUSD","time":1700000100.500,
                       "type":"buy","price":"50000.0","vol":"0.08","fee":"0.60","maker":true},
        "T-BEFORE":{"ordertxid":"O-OLD","pair":"XXBTZUSD","time":1700000100.400,
                    "type":"sell","price":"49990.0","vol":"0.05","fee":"0.40","maker":true}
    },"count":null,"cursor":{"next":null}}})");

    auto fills = venue->GetFills(since);
    ASSERT_OK(fills);
    ASSERT_EQ(fills->size(), 2u);

    const auto seen = http->Requests();
    ASSERT_EQ(seen.size(), 3u);
    const auto q0 = ParseForm(seen[1].body);
    const auto q1 = ParseForm(seen[2].body);
    EXPECT_EQ(q0.at("start"), "1700000099");
    EXPECT_EQ(q0.at("limit"), "100");
    EXPECT_EQ(q0.at("with_cursor"), "true");
    EXPECT_FALSE(q0.contains("ofs"));
    EXPECT_FALSE(q0.contains("cursor"));
    EXPECT_EQ(q1.at("start"), "1700000099");
    EXPECT_EQ(q1.at("limit"), "100");
    EXPECT_EQ(q1.at("with_cursor"), "true");
    EXPECT_FALSE(q1.contains("ofs"));
    EXPECT_EQ(q1.at("cursor"), "cur-page-1");

    EXPECT_EQ((*fills)[0].fill_id, "T-A-EARLIER");
    EXPECT_EQ((*fills)[0].order_id, "O-MINE");
    EXPECT_EQ((*fills)[0].client_order_id, "kst-my-order");
    EXPECT_EQ((*fills)[0].symbol, "BTC/USD");
    EXPECT_EQ((*fills)[0].side, types::Side::kBuy);
    EXPECT_EQ((*fills)[0].quantity, D("0.08"));
    EXPECT_EQ((*fills)[0].price, D("50000.0"));
    EXPECT_EQ((*fills)[0].fee, D("0.60"));
    EXPECT_EQ((*fills)[0].fee_asset, "USD");
    EXPECT_EQ((*fills)[0].liquidity, types::Liquidity::kMaker);
    EXPECT_EQ((*fills)[0].time, since);

    EXPECT_EQ((*fills)[1].fill_id, "T-Z-LATER");
    EXPECT_EQ((*fills)[1].liquidity, types::Liquidity::kTaker);

    // Without a next cursor (omitted `cursor` block), stops in one request even
    // when 100 rows came back and `count` is null.
    std::string page100 = page0;
    for (int i = 50; i < 100; ++i) {
        const std::string id = "T-ETH-" + std::to_string(i);
        page100 +=
            ",\"" + id +
            R"(":{"ordertxid":"O-ETH","pair":"XETHZUSD","time":1700000105.0,)"
            R"("type":"buy","price":"2000.0","vol":"0.1","fee":"0.5","maker":false})";
    }
    http->ClearRequests();
    http->Push(R"({"error":[],"result":{"trades":{)" + page100 +
               R"(},"count":null}})");
    ASSERT_OK(venue->GetFills(since));
    EXPECT_EQ(http->Requests().size(), 1u);

    // Once an entire page is older than `since`, stops immediately without
    // chasing `cursor.next` into older history.
    http->ClearRequests();
    http->Push(R"({"error":[],"result":{"trades":{
        "T-OLD":{"ordertxid":"O-MINE","pair":"XBTUSD","time":1700000100.400,
                 "type":"buy","price":"50000.0","vol":"0.08","fee":"0.60","maker":true}
    },"count":null,"cursor":{"next":"cur-older"}}})");
    auto none_new_enough = venue->GetFills(since);
    ASSERT_OK(none_new_enough);
    EXPECT_TRUE(none_new_enough->empty());
    EXPECT_EQ(http->Requests().size(), 1u);

    // An empty trades page stops cleanly even if a trailing cursor is present.
    http->ClearRequests();
    http->Push(
        R"({"error":[],"result":{"trades":{},"count":null,"cursor":{"next":"cur-eof"}}})");
    auto empty_page = venue->GetFills(since);
    ASSERT_OK(empty_page);
    EXPECT_TRUE(empty_page->empty());
    EXPECT_EQ(http->Requests().size(), 1u);

    // 1. Stall via duplicate rows (`!any_new`) with a distinct cursor token ->
    // kParse after 2 requests (never a partial Ok!).
    http->ClearRequests();
    http->Push(R"({"error":[],"result":{"trades":{
        "T-DUP":{"ordertxid":"O-MINE","pair":"XBTUSD","time":1700000105.0,
                 "type":"buy","price":"50000.0","vol":"0.08","fee":"0.60","maker":true}
    },"count":null,"cursor":{"next":"cur-1"}}})");
    http->Push(R"({"error":[],"result":{"trades":{
        "T-DUP":{"ordertxid":"O-MINE","pair":"XBTUSD","time":1700000105.0,
                 "type":"buy","price":"50000.0","vol":"0.08","fee":"0.60","maker":true}
    },"count":null,"cursor":{"next":"cur-2"}}})");
    auto stalled_rows = venue->GetFills(since);
    ASSERT_FALSE(stalled_rows);
    EXPECT_EQ(stalled_rows.error().category, types::ErrorCategory::kParse);
    EXPECT_EQ(http->Requests().size(), 2u);

    // 2. Stall via repeating cursor token (`seen_cursors`) even when trade IDs
    // are new -> kParse after 2 requests.
    http->ClearRequests();
    http->Push(R"({"error":[],"result":{"trades":{
        "T-R1":{"ordertxid":"O-MINE","pair":"XBTUSD","time":1700000105.0,
                "type":"buy","price":"50000.0","vol":"0.08","fee":"0.60","maker":true}
    },"count":null,"cursor":{"next":"cur-repeat"}}})");
    http->Push(R"({"error":[],"result":{"trades":{
        "T-R2":{"ordertxid":"O-MINE","pair":"XBTUSD","time":1700000104.0,
                "type":"buy","price":"50000.0","vol":"0.08","fee":"0.60","maker":true}
    },"count":null,"cursor":{"next":"cur-repeat"}}})");
    auto stalled_cursor = venue->GetFills(since);
    ASSERT_FALSE(stalled_cursor);
    EXPECT_EQ(stalled_cursor.error().category, types::ErrorCategory::kParse);
    EXPECT_EQ(http->Requests().size(), 2u);

    // 3. Malformed `cursor` or `cursor.next` -> kParse.
    http->ClearRequests();
    http->Push(R"({"error":[],"result":{"trades":{
        "T-BAD1":{"ordertxid":"O-MINE","pair":"XBTUSD","time":1700000105.0,
                  "type":"buy","price":"50000.0","vol":"0.08","fee":"0.60","maker":true}
    },"cursor":123}})");
    EXPECT_EQ(venue->GetFills(since).error().category,
              types::ErrorCategory::kParse);

    http->ClearRequests();
    http->Push(R"({"error":[],"result":{"trades":{
        "T-BAD2":{"ordertxid":"O-MINE","pair":"XBTUSD","time":1700000105.0,
                  "type":"buy","price":"50000.0","vol":"0.08","fee":"0.60","maker":true}
    },"cursor":{"next":123}}})");
    EXPECT_EQ(venue->GetFills(since).error().category,
              types::ErrorCategory::kParse);

    // 4. Transient error on Page 2 retries and succeeds; persistent error on
    // Page 2 exhausting retries fails the whole call (never partial Ok!).
    http->ClearRequests();
    http->Push(R"({"error":[],"result":{"trades":{
        "T-P1":{"ordertxid":"O-MINE","pair":"XBTUSD","time":1700000105.0,
                "type":"buy","price":"50000.0","vol":"0.05","fee":"0.40","maker":true}
    },"cursor":{"next":"cur-p2"}}})");
    http->Push(R"({"error":["EService:Unavailable"]})");
    http->Push(R"({"error":[],"result":{"trades":{
        "T-P2":{"ordertxid":"O-MINE","pair":"XBTUSD","time":1700000101.0,
                "type":"buy","price":"49995.0","vol":"0.03","fee":"0.20","maker":true}
    },"cursor":{"next":null}}})");
    auto retried_p2 = venue->GetFills(since);
    ASSERT_OK(retried_p2);
    ASSERT_EQ(retried_p2->size(), 2u);
    EXPECT_EQ((*retried_p2)[0].fill_id, "T-P2");
    EXPECT_EQ((*retried_p2)[1].fill_id, "T-P1");
    ASSERT_EQ(http->Requests().size(), 3u);
    EXPECT_EQ(ParseForm(http->Requests()[2].body).at("cursor"), "cur-p2");

    http->ClearRequests();
    http->Push(R"({"error":[],"result":{"trades":{
        "T-ONLY-P1":{"ordertxid":"O-MINE","pair":"XBTUSD","time":1700000105.0,
                     "type":"buy","price":"50000.0","vol":"0.05","fee":"0.40","maker":true}
    },"cursor":{"next":"cur-fail"}}})");
    const int attempts = util::RetryPolicy{}.max_attempts;
    for (int i = 0; i < attempts; ++i) {
        http->Push(R"({"error":["EService:Unavailable"]})");
    }
    auto failed_p2 = venue->GetFills(since);
    ASSERT_FALSE(failed_p2);
    EXPECT_EQ(failed_p2.error().category, types::ErrorCategory::kServer);
    EXPECT_EQ(http->Requests().size(), static_cast<std::size_t>(1 + attempts));
}

TEST_F(KrakenTradingVenueTest,
       GetBalancesNormalizesAssetsComputesAvailableAndRetriesReads) {
    auto venue = Make();
    http->Push(R"({"error":["EAPI:Rate limit exceeded"]})");
    http->Push(R"({"error":[],"result":{
        "ZUSD":{"balance":"10000.50","hold_trade":"1250.25","credit":"500.00","credit_used":"100.00"},
        "XXBT":{"balance":"0.75000000","hold_trade":"0.25000000"},
        "XXBT.F":{"balance":"0.10000000","hold_trade":"0.00000000"},
        "XZEC":{"balance":"0.00000000","hold_trade":"0.00000000"}
    }})");

    auto balances = venue->GetBalances();
    ASSERT_OK(balances);
    ASSERT_EQ(balances->size(), 3u);

    EXPECT_EQ((*balances)[0].asset, "BTC");
    EXPECT_EQ((*balances)[0].total, D("0.75"));
    EXPECT_EQ((*balances)[0].locked, D("0.25"));
    EXPECT_EQ((*balances)[0].available, D("0.50"));

    EXPECT_EQ((*balances)[1].asset, "BTC.F");
    EXPECT_EQ((*balances)[1].total, D("0.10"));
    EXPECT_EQ((*balances)[1].available, D("0.10"));

    EXPECT_EQ((*balances)[2].asset, "USD");
    EXPECT_EQ((*balances)[2].total, D("10000.50"));
    EXPECT_EQ((*balances)[2].locked, D("1250.25"));
    EXPECT_EQ((*balances)[2].available, D("9150.25"));

    auto positions = venue->GetPositions();
    ASSERT_OK(positions);
    EXPECT_TRUE(positions->empty());

    types::OcoRequest oco{"BTC/USD", types::Side::kSell, D("0.1"), D("55000"),
                          D("48000")};
    EXPECT_EQ(venue->PlaceOco(oco).error().category,
              types::ErrorCategory::kNotImplemented);
}

TEST_F(KrakenTradingVenueTest, GetFeesReadsMakerAndTakerTiersPerPair) {
    auto venue = Make();
    http->Push(R"({"error":[],"result":{
        "currency":"ZUSD","volume":"1234.5600",
        "fees":{
            "XXBTZUSD":{"fee":"0.4000","minfee":"0.0800","maxfee":"0.4000",
                        "nextfee":"0.3500","tiervolume":"0.0000",
                        "nextvolume":"10000.0000"},
            "SOLUSD":{"fee":"0.1000","minfee":"0.1000","maxfee":"0.4000",
                      "nextfee":null,"tiervolume":"10000000.0000",
                      "nextvolume":null}
        },
        "fees_maker":{
            "XXBTZUSD":{"fee":"0.2500","minfee":"0.0000","maxfee":"0.2500",
                        "nextfee":"0.2000","tiervolume":"0.0000",
                        "nextvolume":"10000.0000"}
        }
    }})");

    auto fees = venue->GetFees({"SOL/USD", "BTC/USD"});
    ASSERT_OK(fees);
    const auto requests = http->Requests();
    ASSERT_EQ(requests.size(), 1u);
    EXPECT_EQ(requests[0].url, "https://api.kraken.test/0/private/TradeVolume");
    EXPECT_EQ(ParseForm(requests[0].body).at("pair"), "SOL/USD,BTC/USD");

    EXPECT_EQ(fees->volume, D("1234.56"));
    EXPECT_EQ(fees->volume_asset, "USD");
    ASSERT_EQ(fees->pairs.size(), 2u);

    const auto& btc = fees->pairs[0];
    EXPECT_EQ(btc.symbol, "BTC/USD");
    EXPECT_EQ(btc.taker.percent, D("0.40"));
    EXPECT_EQ(btc.taker.next_percent, D("0.35"));
    EXPECT_EQ(btc.taker.next_volume, D("10000"));
    ASSERT_TRUE(btc.maker);
    EXPECT_EQ(btc.maker->percent, D("0.25"));
    EXPECT_EQ(btc.maker->next_percent, D("0.20"));

    const auto& sol = fees->pairs[1];
    EXPECT_EQ(sol.symbol, "SOL/USD");
    EXPECT_EQ(sol.taker.percent, D("0.10"));
    EXPECT_FALSE(sol.taker.next_percent);
    EXPECT_FALSE(sol.taker.next_volume);
    EXPECT_FALSE(sol.maker);
}

TEST_F(KrakenTradingVenueTest, GetFeesRefusesNoPairsAndMalformedReplies) {
    auto venue = Make();
    EXPECT_EQ(venue->GetFees({}).error().category,
              types::ErrorCategory::kInvalidArgument);
    EXPECT_TRUE(http->Requests().empty());

    // A null `fees_maker` is no maker schedule, not an error.
    http->Push(R"({"error":[],"result":{"currency":"ZUSD","volume":"0",
        "fees":{"XXBTZUSD":{"fee":"0.4000","nextfee":"0.3500",
                            "nextvolume":"10000"}},
        "fees_maker":null}})");
    auto no_maker = venue->GetFees({"BTC/USD"});
    ASSERT_OK(no_maker);
    ASSERT_EQ(no_maker->pairs.size(), 1u);
    EXPECT_FALSE(no_maker->pairs[0].maker);

    http->Push(R"({"error":[],"result":{"currency":"ZUSD","volume":"0",
        "fees":{"XXBTZUSD":{"fee":"lots"}}}})");
    EXPECT_EQ(venue->GetFees({"BTC/USD"}).error().category,
              types::ErrorCategory::kParse);

    http->Push(R"({"error":[],"result":{"currency":"ZUSD","volume":"0",
        "fees":null}})");
    EXPECT_EQ(venue->GetFees({"BTC/USD"}).error().category,
              types::ErrorCategory::kParse);

    http->Push(R"({"error":["EGeneral:Permission denied"]})");
    auto denied = venue->GetFees({"BTC/USD"});
    ASSERT_FALSE(denied);
    EXPECT_EQ(denied.error().category, types::ErrorCategory::kAuth);
    for (const auto& request : http->Requests()) {
        ExpectNoSecretsIn(request.body);
    }
}

TEST_F(KrakenTradingVenueTest,
       OrderRateLimitPenalizesOrderLimiterNotCounterLimiter) {
    trading::KrakenTradingVenue::Options opts;
    opts.rest_base_url = "https://api.kraken.test";
    // If mCounterLimiter were penalized, the next read would block 100 s.
    opts.counter_per_second = 0.01;
    opts.counter_burst = 100.0;
    // Penalizing mOrderLimiter drains its 100-token burst so the next order
    // waits one 40 ms refill interval.
    opts.orders_per_second = 25.0;
    opts.order_burst = 100.0;
    opts.clock = [this] { return now; };
    trading::KrakenTradingVenue venue({kTestKey, kTestSecret}, data,
                                      {"BTC/USD"}, http, std::move(opts));

    http->Push(R"({"error":["EOrder:Rate limit exceeded"]})");
    types::OrderRequest req;
    req.symbol = "BTC/USD";
    req.quantity = D("0.01");
    ASSERT_FALSE(venue.PlaceOrder(req));

    http->Push(R"({"error":[],"result":{}})");
    const auto read_start = std::chrono::steady_clock::now();
    ASSERT_OK(venue.GetBalances());
    const auto read_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - read_start);
    EXPECT_LT(read_ms, 500ms);

    http->Push(R"({"error":[],"result":{"txid":["O-NEXT"]}})");
    const auto order_start = std::chrono::steady_clock::now();
    ASSERT_OK(venue.PlaceOrder(req));
    const auto order_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - order_start);
    EXPECT_GE(order_ms, 25ms);
}

}  // namespace
