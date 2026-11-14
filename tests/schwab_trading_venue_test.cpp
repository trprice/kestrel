#include "kestrel/trading/schwab_trading_venue.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <functional>
#include <nlohmann/json.hpp>
#include <set>

#include "test_helpers.hpp"

namespace kestrel::trading {
namespace {

using namespace std::chrono_literals;
using nlohmann::json;
using types::Decimal;
using types::ErrorCategory;
using types::OrderRequest;
using types::OrderState;
using types::OrderType;
using types::Side;
using types::TimeInForce;

Decimal D(std::string_view s) { return *Decimal::Parse(s); }
types::Timestamp At(const char* iso) { return *types::ParseIso8601(iso); }

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

class StubDataSource : public data::DataSource {
   public:
    std::map<types::Symbol, types::SymbolInfo> symbols;

    StubDataSource() {
        symbols["SPY"] =
            types::SymbolInfo{"SPY", D("0.01"), D("1"), D("1"), D("0")};
        symbols["AAPL"] =
            types::SymbolInfo{"AAPL", D("0.01"), D("1"), D("1"), D("0")};
        symbols["BRK.B"] =
            types::SymbolInfo{"BRK.B", D("0.01"), D("1"), D("1"), D("0")};
        symbols["TINY"] =
            types::SymbolInfo{"TINY", D("0.0001"), D("1"), D("1"), D("0")};
    }

    std::string_view Name() const override { return "schwab"; }
    types::Result<std::vector<types::Bar>> GetBars(const types::Symbol&,
                                                   types::Timeframe,
                                                   types::Timestamp,
                                                   types::Timestamp) override {
        return std::vector<types::Bar>{};
    }
    types::Result<types::SymbolInfo> GetSymbolInfo(
        const types::Symbol& symbol) override {
        const auto it = symbols.find(symbol);
        if (it == symbols.end()) {
            return types::MakeError(ErrorCategory::kNotFound,
                                    "unknown symbol " + symbol);
        }
        return it->second;
    }
    types::Result<std::unique_ptr<data::Subscription>> StreamQuotes(
        const std::vector<types::Symbol>&, data::QuoteCallback) override {
        return std::unique_ptr<data::Subscription>{};
    }
    types::Result<std::unique_ptr<data::Subscription>> StreamBars(
        const std::vector<types::Symbol>&, types::Timeframe,
        data::BarCallback) override {
        return std::unique_ptr<data::Subscription>{};
    }
};

types::Result<net::HttpResponse> Ok(
    std::string body, std::map<std::string, std::string> headers = {},
    int status = 200) {
    return net::HttpResponse{status, std::move(body), std::move(headers)};
}

types::Result<net::HttpResponse> Created(std::string location) {
    return Ok("", {{"Location", std::move(location)}}, 201);
}

types::Result<net::HttpResponse> HttpStatus(int status, std::string body = "") {
    return std::unexpected(net::ErrorFromHttpStatus(
        net::HttpResponse{status, std::move(body), {}}));
}

constexpr const char* kSingleAccount =
    R"([{"accountNumber":"12345678","hashValue":"HASH123"}])";

class SchwabTradingVenueTest : public ::testing::Test {
   protected:
    void SetUp() override {
        mDir = std::filesystem::temp_directory_path() /
               ("kestrel-schwab-venue-" +
                std::string(::testing::UnitTest::GetInstance()
                                ->current_test_info()
                                ->name()));
        std::filesystem::remove_all(mDir);
        std::filesystem::create_directories(mDir);
        const config::SchwabTokenStore store(mDir / "tokens.json");
        const auto now = types::Now();
        ASSERT_OK(
            store.Save({"access-tok", "refresh-tok", now + 1h, now + 72h}));
        mAuth = std::make_shared<data::SchwabAuth>(
            config::SchwabCredentials{"app-key", "app-secret"}, store,
            std::make_shared<FakeHttp>(), "https://127.0.0.1:8182");
        ASSERT_OK(mAuth->Load());
        mHttp = std::make_shared<FakeHttp>();
        mData = std::make_shared<StubDataSource>();
    }

    void TearDown() override { std::filesystem::remove_all(mDir); }

    SchwabTradingVenue Venue(std::string account_number = "",
                             std::vector<types::Symbol> symbols = {}) {
        SchwabTradingVenue::Options options;
        options.base_url = "https://api.test/trader/v1";
        options.account_number = std::move(account_number);
        options.symbols = std::move(symbols);
        options.orders_per_second = 1000;
        options.order_burst = 1000;
        options.clock = [this] { return mNow; };
        options.sleeper = [this](std::chrono::milliseconds d) {
            mSlept.push_back(d);
        };
        return SchwabTradingVenue(
            std::make_shared<data::SchwabApi>(
                mAuth, mHttp, data::SchwabApi::Options{1000, 1000}),
            mData, std::move(options));
    }

    std::filesystem::path mDir;
    std::shared_ptr<data::SchwabAuth> mAuth;
    std::shared_ptr<FakeHttp> mHttp;
    std::shared_ptr<StubDataSource> mData;
    types::Timestamp mNow = At("2026-11-05T15:00:00Z");
    std::vector<std::chrono::milliseconds> mSlept;
};

TEST_F(SchwabTradingVenueTest, ResolvesAccountHashOnceAndCachesItAcrossCalls) {
    mHttp->reply = [](const net::HttpRequest& r) {
        if (r.url.ends_with("/accounts/accountNumbers")) {
            return Ok(kSingleAccount);
        }
        return Ok("[]");
    };
    auto venue = Venue();
    ASSERT_OK(venue.GetOpenOrders());
    ASSERT_OK(venue.GetOpenOrders());

    ASSERT_EQ(mHttp->requests.size(), 3u);
    EXPECT_EQ(mHttp->requests[0].url,
              "https://api.test/trader/v1/accounts/accountNumbers");
    EXPECT_EQ(mHttp->requests[1].url,
              "https://api.test/trader/v1/accounts/HASH123/orders");
    EXPECT_EQ(mHttp->requests[2].url,
              "https://api.test/trader/v1/accounts/HASH123/orders");
}

TEST_F(SchwabTradingVenueTest, SelectsConfiguredAccountOrFailsWhenMissing) {
    const std::string two_accounts =
        R"([{"accountNumber":"1111","hashValue":"HASH_ONE"},)"
        R"({"accountNumber":"2222","hashValue":"HASH_TWO"}])";
    mHttp->reply = [&](const net::HttpRequest& r) {
        if (r.url.ends_with("/accounts/accountNumbers")) {
            return Ok(two_accounts);
        }
        return Ok("[]");
    };

    auto second = Venue("2222");
    ASSERT_OK(second.GetOpenOrders());
    ASSERT_EQ(mHttp->requests.size(), 2u);
    EXPECT_EQ(mHttp->requests[1].url,
              "https://api.test/trader/v1/accounts/HASH_TWO/orders");

    auto missing = Venue("87654321").GetOpenOrders();
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().category, ErrorCategory::kNotFound);
    // The account number is masked.
    EXPECT_EQ(missing.error().message.find("87654321"), std::string::npos);
    EXPECT_NE(missing.error().message.find("****4321"), std::string::npos);

    mHttp->reply = [](const net::HttpRequest&) { return Ok("[]"); };
    auto empty = Venue().GetOpenOrders();
    ASSERT_FALSE(empty);
    EXPECT_EQ(empty.error().category, ErrorCategory::kAuth);
}

TEST_F(SchwabTradingVenueTest, PlacesMarketLimitAndStopOrders) {
    int next_id = 1001;
    mHttp->reply = [&](const net::HttpRequest& r) {
        if (r.url.ends_with("/accounts/accountNumbers")) {
            return Ok(kSingleAccount);
        }
        const std::string id = std::to_string(next_id++);
        return Ok(
            "",
            {{"Location",
              "https://api.test/trader/v1/accounts/HASH123/orders/" + id}},
            201);
    };
    auto venue = Venue();

    // 1. Market buy on BRK.B (mapped to BRK/B, GTC mapped to DAY, fractional
    // shares rounded down to lot size 1).
    OrderRequest market;
    market.symbol = "brk.b";
    market.side = Side::kBuy;
    market.type = OrderType::kMarket;
    market.quantity = D("5.9");
    market.client_order_id = "kst-1";
    auto m_ack = venue.PlaceOrder(market);
    ASSERT_OK(m_ack);
    EXPECT_EQ(m_ack->order_id, "1001");
    EXPECT_EQ(m_ack->client_order_id, "kst-1");
    EXPECT_EQ(m_ack->state, OrderState::kNew);
    EXPECT_EQ(m_ack->time, mNow);

    const json m_body = json::parse(mHttp->requests[1].body);
    EXPECT_EQ(m_body["orderType"], "MARKET");
    EXPECT_EQ(m_body["session"], "NORMAL");
    EXPECT_EQ(m_body["duration"], "DAY");
    EXPECT_EQ(m_body["orderStrategyType"], "SINGLE");
    EXPECT_FALSE(m_body.contains("price"));
    EXPECT_FALSE(m_body.contains("stopPrice"));
    ASSERT_EQ(m_body["orderLegCollection"].size(), 1u);
    EXPECT_EQ(m_body["orderLegCollection"][0]["instruction"], "BUY");
    EXPECT_EQ(m_body["orderLegCollection"][0]["quantity"], 5);
    EXPECT_EQ(m_body["orderLegCollection"][0]["instrument"]["symbol"], "BRK/B");
    EXPECT_EQ(m_body["orderLegCollection"][0]["instrument"]["assetType"],
              "EQUITY");

    // 2. Limit sell on SPY (rounded up to tick 0.01, GOOD_TILL_CANCEL).
    OrderRequest limit;
    limit.symbol = "SPY";
    limit.side = Side::kSell;
    limit.type = OrderType::kLimit;
    limit.quantity = D("10");
    limit.limit_price = D("580.121");
    limit.time_in_force = TimeInForce::kGtc;
    auto l_ack = venue.PlaceOrder(limit);
    ASSERT_OK(l_ack);
    EXPECT_EQ(l_ack->order_id, "1002");

    const json l_body = json::parse(mHttp->requests[2].body);
    EXPECT_EQ(l_body["orderType"], "LIMIT");
    EXPECT_EQ(l_body["duration"], "GOOD_TILL_CANCEL");
    EXPECT_EQ(l_body["price"], 580.13);
    EXPECT_EQ(l_body["orderLegCollection"][0]["instruction"], "SELL");
    EXPECT_EQ(l_body["orderLegCollection"][0]["quantity"], 10);
    EXPECT_EQ(l_body["orderLegCollection"][0]["instrument"]["symbol"], "SPY");

    // 3. Stop sell on TINY (sub-dollar tick 0.0001, DAY).
    OrderRequest stop;
    stop.symbol = "TINY";
    stop.side = Side::kSell;
    stop.type = OrderType::kStop;
    stop.quantity = D("100");
    stop.stop_price = D("0.43214");
    stop.time_in_force = TimeInForce::kDay;
    auto s_ack = venue.PlaceOrder(stop);
    ASSERT_OK(s_ack);
    EXPECT_EQ(s_ack->order_id, "1003");

    const json s_body = json::parse(mHttp->requests[3].body);
    EXPECT_EQ(s_body["orderType"], "STOP");
    EXPECT_EQ(s_body["duration"], "DAY");
    EXPECT_EQ(s_body["stopPrice"], 0.4321);
    EXPECT_EQ(s_body["orderLegCollection"][0]["instrument"]["symbol"], "TINY");
}

TEST_F(SchwabTradingVenueTest, RejectsUnsupportedAndInvalidOrderRequests) {
    mHttp->reply = [](const net::HttpRequest&) { return Ok(kSingleAccount); };
    auto venue = Venue();

    // Post-only is not supported by Schwab.
    OrderRequest post_only;
    post_only.symbol = "SPY";
    post_only.side = Side::kBuy;
    post_only.type = OrderType::kLimit;
    post_only.quantity = D("1");
    post_only.limit_price = D("500");
    post_only.post_only = true;
    auto po = venue.PlaceOrder(post_only);
    ASSERT_FALSE(po);
    EXPECT_EQ(po.error().category, ErrorCategory::kUnsupported);

    // Market IOC/FOK and Stop IOC/FOK are unsupported on Schwab.
    OrderRequest mkt_ioc;
    mkt_ioc.symbol = "SPY";
    mkt_ioc.type = OrderType::kMarket;
    mkt_ioc.quantity = D("1");
    mkt_ioc.time_in_force = TimeInForce::kIoc;
    EXPECT_EQ(venue.PlaceOrder(mkt_ioc).error().category,
              ErrorCategory::kUnsupported);

    OrderRequest stop_fok;
    stop_fok.symbol = "SPY";
    stop_fok.type = OrderType::kStop;
    stop_fok.quantity = D("1");
    stop_fok.stop_price = D("490");
    stop_fok.time_in_force = TimeInForce::kFok;
    EXPECT_EQ(venue.PlaceOrder(stop_fok).error().category,
              ErrorCategory::kUnsupported);

    // Crypto pair is rejected before hitting the Trader API.
    OrderRequest crypto;
    crypto.symbol = "BTC/USD";
    crypto.type = OrderType::kMarket;
    crypto.quantity = D("1");
    EXPECT_EQ(venue.PlaceOrder(crypto).error().category,
              ErrorCategory::kUnsupported);

    // Below minimum order size (1 share).
    OrderRequest fractional;
    fractional.symbol = "SPY";
    fractional.type = OrderType::kMarket;
    fractional.quantity = D("0.5");
    EXPECT_EQ(venue.PlaceOrder(fractional).error().category,
              ErrorCategory::kRejected);

    EXPECT_TRUE(mHttp->requests.empty());
}

TEST_F(SchwabTradingVenueTest,
       RecoversPlacedOrderAfterTimeoutWithoutRetryingThePost) {
    int post_calls = 0;
    const std::string orders_json = R"([
      {
        "orderId": 888,
        "status": "WORKING",
        "orderType": "LIMIT",
        "duration": "GOOD_TILL_CANCEL",
        "quantity": 10,
        "filledQuantity": 0,
        "price": 500.00,
        "enteredTime": "2026-11-05T14:00:00+0000",
        "orderLegCollection": [{
          "instruction": "BUY",
          "quantity": 10,
          "instrument": {"symbol": "SPY", "assetType": "EQUITY"}
        }]
      },
      {
        "orderId": 999,
        "status": "WORKING",
        "orderType": "LIMIT",
        "duration": "GOOD_TILL_CANCEL",
        "quantity": 10,
        "filledQuantity": 0,
        "price": 500.00,
        "enteredTime": "2026-11-05T15:00:01+0000",
        "orderLegCollection": [{
          "instruction": "BUY",
          "quantity": 10,
          "instrument": {"symbol": "SPY", "assetType": "EQUITY"}
        }]
      }
    ])";

    // First, let the venue see order 888 so it is in mKnownOrders.
    mHttp->reply = [&](const net::HttpRequest& r) {
        if (r.url.ends_with("/accounts/accountNumbers")) {
            return Ok(kSingleAccount);
        }
        if (r.method == net::HttpMethod::kGet) {
            return Ok(R"([{
              "orderId": 888,
              "status": "WORKING",
              "orderType": "LIMIT",
              "duration": "GOOD_TILL_CANCEL",
              "quantity": 10,
              "filledQuantity": 0,
              "price": 500.00,
              "enteredTime": "2026-11-05T14:00:00+0000",
              "orderLegCollection": [{
                "instruction": "BUY",
                "quantity": 10,
                "instrument": {"symbol": "SPY", "assetType": "EQUITY"}
              }]
            }])");
        }
        return Ok("[]");
    };
    auto venue = Venue();
    ASSERT_OK(venue.GetOpenOrders());

    // Now place a new order of the same shape; POST times out, and GET /orders
    // returns both 888 (known before) and 999 (newly placed).
    mHttp->reply = [&](const net::HttpRequest& r) {
        if (r.method == net::HttpMethod::kPost) {
            ++post_calls;
            return types::Result<net::HttpResponse>(
                types::MakeError(ErrorCategory::kTimeout, "request timed out"));
        }
        return Ok(orders_json);
    };

    OrderRequest req;
    req.symbol = "SPY";
    req.side = Side::kBuy;
    req.type = OrderType::kLimit;
    req.quantity = D("10");
    req.limit_price = D("500");
    req.client_order_id = "kst-recovered";
    auto ack = venue.PlaceOrder(req);
    ASSERT_OK(ack);
    EXPECT_EQ(post_calls, 1);  // POST was never resent
    EXPECT_EQ(ack->order_id, "999");
    EXPECT_EQ(ack->client_order_id, "kst-recovered");
    EXPECT_EQ(ack->state, OrderState::kNew);
}

TEST_F(SchwabTradingVenueTest,
       ReportsUnconfirmedPlacementWhenTimedOutOrderIsNotOnAccount) {
    int post_calls = 0;
    mHttp->reply = [&](const net::HttpRequest& r) {
        if (r.url.ends_with("/accounts/accountNumbers")) {
            return Ok(kSingleAccount);
        }
        if (r.method == net::HttpMethod::kPost) {
            ++post_calls;
            return HttpStatus(503, R"({"message":"Service Unavailable"})");
        }
        return Ok("[]");
    };

    OrderRequest req;
    req.symbol = "SPY";
    req.side = Side::kBuy;
    req.type = OrderType::kMarket;
    req.quantity = D("2");
    auto failed = Venue().PlaceOrder(req);
    ASSERT_FALSE(failed);
    EXPECT_EQ(post_calls, 1);
    EXPECT_EQ(failed.error().category, ErrorCategory::kServer);
    EXPECT_NE(failed.error().message.find("it may have been placed"),
              std::string::npos);
}

TEST_F(SchwabTradingVenueTest, TestOrderUsesPreviewEndpointAndReportsRejects) {
    mHttp->reply = [](const net::HttpRequest& r) {
        if (r.url.ends_with("/accounts/accountNumbers")) {
            return Ok(kSingleAccount);
        }
        EXPECT_EQ(r.url,
                  "https://api.test/trader/v1/accounts/HASH123/previewOrder");
        return Ok(R"({
          "orderId": 0,
          "orderStrategy": {"status": "ACCEPTED"},
          "orderValidationResult": {"rejects": []}
        })");
    };
    auto venue = Venue();
    OrderRequest req;
    req.symbol = "SPY";
    req.side = Side::kBuy;
    req.type = OrderType::kLimit;
    req.quantity = D("5");
    req.limit_price = D("500");
    ASSERT_OK(venue.TestOrder(req));

    mHttp->reply = [](const net::HttpRequest&) {
        return Ok(R"({
          "orderId": 0,
          "orderStrategy": {"status": "REJECTED"},
          "orderValidationResult": {
            "rejects": [{"message": "Insufficient buying power"}]
          }
        })");
    };
    auto rejected = venue.TestOrder(req);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().category, ErrorCategory::kRejected);
    EXPECT_NE(rejected.error().message.find("Insufficient buying power"),
              std::string::npos);
}

TEST_F(SchwabTradingVenueTest, PlacesOcoOrderAndExtractsChildAcknowledgements) {
    mHttp->reply = [](const net::HttpRequest& r) {
        if (r.url.ends_with("/accounts/accountNumbers")) {
            return Ok(kSingleAccount);
        }
        if (r.method == net::HttpMethod::kPost) {
            const json b = json::parse(r.body);
            EXPECT_EQ(b["orderStrategyType"], "OCO");
            EXPECT_EQ(b["childOrderStrategies"].size(), 2u);
            EXPECT_EQ(b["childOrderStrategies"][0]["orderType"], "LIMIT");
            EXPECT_EQ(b["childOrderStrategies"][0]["price"], 550);
            EXPECT_EQ(b["childOrderStrategies"][1]["orderType"], "STOP");
            EXPECT_EQ(b["childOrderStrategies"][1]["stopPrice"], 480);
            return Ok("",
                      {{"Location",
                        "https://api.test/trader/v1/accounts/HASH123/orders/"
                        "5000"}},
                      201);
        }
        if (r.url.ends_with("/orders/5000")) {
            return Ok(R"({
              "orderId": 5000,
              "orderStrategyType": "OCO",
              "status": "WORKING",
              "childOrderStrategies": [
                {
                  "orderId": 5001,
                  "orderStrategyType": "SINGLE",
                  "orderType": "LIMIT",
                  "duration": "GOOD_TILL_CANCEL",
                  "status": "WORKING",
                  "quantity": 10,
                  "filledQuantity": 0,
                  "price": 550,
                  "enteredTime": "2026-11-05T15:00:00+0000",
                  "orderLegCollection": [{
                    "instruction": "SELL",
                    "quantity": 10,
                    "instrument": {"symbol": "SPY", "assetType": "EQUITY"}
                  }]
                },
                {
                  "orderId": 5002,
                  "orderStrategyType": "SINGLE",
                  "orderType": "STOP",
                  "duration": "GOOD_TILL_CANCEL",
                  "status": "AWAITING_STOP_CONDITION",
                  "quantity": 10,
                  "filledQuantity": 0,
                  "stopPrice": 480,
                  "enteredTime": "2026-11-05T15:00:00+0000",
                  "orderLegCollection": [{
                    "instruction": "SELL",
                    "quantity": 10,
                    "instrument": {"symbol": "SPY", "assetType": "EQUITY"}
                  }]
                }
              ]
            })");
        }
        return Ok("[]");
    };

    auto venue = Venue();
    types::OcoRequest oco;
    oco.symbol = "SPY";
    oco.side = Side::kSell;
    oco.quantity = D("10");
    oco.limit_price = D("550");
    oco.stop_price = D("480");
    auto ack = venue.PlaceOco(oco);
    ASSERT_OK(ack);
    EXPECT_EQ(ack->limit.order_id, "5001");
    EXPECT_EQ(ack->limit.state, OrderState::kNew);
    EXPECT_EQ(ack->stop.order_id, "5002");
    EXPECT_EQ(ack->stop.state, OrderState::kNew);
}

TEST_F(SchwabTradingVenueTest, CancelOrderDistinguishesOutcomes) {
    std::string order_state = "FILLED";
    mHttp->reply = [&](const net::HttpRequest& r) {
        if (r.url.ends_with("/accounts/accountNumbers")) {
            return Ok(kSingleAccount);
        }
        if (r.method == net::HttpMethod::kDelete) {
            if (r.url.ends_with("/orders/100")) {
                return Ok("");
            }
            return HttpStatus(
                400, R"({"message":"Order cannot be canceled","errors":[]})");
        }
        if (r.method == net::HttpMethod::kGet &&
            r.url.find("/orders/") != std::string::npos) {
            return Ok(R"({
              "orderId": 101,
              "status": ")" +
                      order_state + R"(",
              "orderType": "LIMIT",
              "duration": "DAY",
              "quantity": 5,
              "filledQuantity": 5,
              "price": 500,
              "enteredTime": "2026-11-05T14:30:00+0000",
              "orderLegCollection": [{
                "instruction": "BUY",
                "quantity": 5,
                "instrument": {"symbol": "SPY", "assetType": "EQUITY"}
              }]
            })");
        }
        return HttpStatus(404);
    };

    auto venue = Venue();
    auto ok = venue.CancelOrder("100");
    ASSERT_OK(ok);
    EXPECT_EQ(*ok, CancelOutcome::kCancelled);

    auto filled = venue.CancelOrder("101");
    ASSERT_OK(filled);
    EXPECT_EQ(*filled, CancelOutcome::kAlreadyFilled);

    order_state = "CANCELED";
    auto closed = venue.CancelOrder("101");
    ASSERT_OK(closed);
    EXPECT_EQ(*closed, CancelOutcome::kAlreadyClosed);

    order_state = "WORKING";
    auto still_working = venue.CancelOrder("101");
    ASSERT_FALSE(still_working);
    EXPECT_EQ(still_working.error().category, ErrorCategory::kRejected);
    EXPECT_NE(still_working.error().message.find("Order cannot be canceled"),
              std::string::npos);
}

TEST_F(SchwabTradingVenueTest,
       GetOpenOrdersKeepsAllActiveStatesAndUnpacksChildOrders) {
    mHttp->reply = [](const net::HttpRequest& r) {
        if (r.url.ends_with("/accounts/accountNumbers")) {
            return Ok(kSingleAccount);
        }
        return Ok(R"([
          {
            "orderId": 1,
            "status": "WORKING",
            "orderType": "LIMIT",
            "duration": "GOOD_TILL_CANCEL",
            "quantity": 10,
            "filledQuantity": 4,
            "price": 225.50,
            "enteredTime": "2026-11-05T14:30:00+0000",
            "orderLegCollection": [{
              "instruction": "BUY",
              "quantity": 10,
              "instrument": {"symbol": "AAPL", "assetType": "EQUITY"}
            }],
            "orderActivityCollection": [{
              "activityType": "EXECUTION",
              "executionType": "FILL",
              "executionLegs": [
                {"legId": 1, "quantity": 4, "price": 225.25,
                 "time": "2026-11-05T14:31:00+0000"}
              ]
            }]
          },
          {
            "orderId": 2,
            "status": "QUEUED",
            "orderType": "STOP",
            "duration": "DAY",
            "quantity": 5,
            "filledQuantity": 0,
            "stopPrice": 560.00,
            "enteredTime": "2026-11-05T14:32:00+0000",
            "orderLegCollection": [{
              "instruction": "SELL",
              "quantity": 5,
              "instrument": {"symbol": "BRK/B", "assetType": "EQUITY"}
            }]
          },
          {
            "orderId": 3,
            "status": "FILLED",
            "orderType": "MARKET",
            "duration": "DAY",
            "quantity": 1,
            "filledQuantity": 1,
            "enteredTime": "2026-11-05T14:33:00+0000",
            "orderLegCollection": [{
              "instruction": "BUY",
              "quantity": 1,
              "instrument": {"symbol": "SPY", "assetType": "EQUITY"}
            }]
          },
          {
            "orderId": 4,
            "status": "WORKING",
            "orderType": "LIMIT",
            "duration": "DAY",
            "quantity": 1,
            "filledQuantity": 0,
            "price": 2.50,
            "enteredTime": "2026-11-05T14:34:00+0000",
            "orderLegCollection": [{
              "instruction": "BUY_TO_OPEN",
              "quantity": 1,
              "instrument": {"symbol": "SPY   261120C00580000", "assetType": "OPTION"}
            }]
          }
        ])");
    };

    auto open = Venue().GetOpenOrders();
    ASSERT_OK(open);
    ASSERT_EQ(open->size(), 2u);

    EXPECT_EQ((*open)[0].order_id, "1");
    EXPECT_EQ((*open)[0].symbol, "AAPL");
    EXPECT_EQ((*open)[0].side, Side::kBuy);
    EXPECT_EQ((*open)[0].type, OrderType::kLimit);
    EXPECT_EQ((*open)[0].time_in_force, TimeInForce::kGtc);
    EXPECT_EQ((*open)[0].quantity, D("10"));
    EXPECT_EQ((*open)[0].filled_quantity, D("4"));
    EXPECT_EQ((*open)[0].limit_price, D("225.5"));
    EXPECT_EQ((*open)[0].average_fill_price, D("225.25"));
    EXPECT_EQ((*open)[0].state, OrderState::kPartiallyFilled);
    EXPECT_EQ((*open)[0].created_time, At("2026-11-05T14:30:00Z"));
    EXPECT_EQ((*open)[0].updated_time, At("2026-11-05T14:31:00Z"));

    EXPECT_EQ((*open)[1].order_id, "2");
    EXPECT_EQ((*open)[1].symbol, "BRK.B");
    EXPECT_EQ((*open)[1].side, Side::kSell);
    EXPECT_EQ((*open)[1].type, OrderType::kStop);
    EXPECT_EQ((*open)[1].time_in_force, TimeInForce::kDay);
    EXPECT_EQ((*open)[1].stop_price, D("560"));
    EXPECT_EQ((*open)[1].state, OrderState::kNew);
}

TEST_F(SchwabTradingVenueTest,
       GetFillsExtractsExecutionsFiltersByTimeAndClampsEpochSince) {
    mHttp->reply = [](const net::HttpRequest& r) {
        if (r.url.ends_with("/accounts/accountNumbers")) {
            return Ok(kSingleAccount);
        }
        if (r.url.ends_with("/transactions")) {
            return Ok("[]");
        }
        // Even when GetFills(Timestamp{}) is called, fromEnteredTime must be
        // clamped within 1 year of `mNow` (2026-11-05).
        EXPECT_GE(*types::ParseIso8601(r.query.at("fromEnteredTime")),
                  At("2025-11-05T00:00:00Z"));
        return Ok(R"([
          {
            "orderId": 700,
            "status": "FILLED",
            "orderType": "MARKET",
            "duration": "DAY",
            "quantity": 10,
            "filledQuantity": 10,
            "enteredTime": "2026-11-05T14:30:00+0000",
            "orderLegCollection": [{
              "legId": 1,
              "instruction": "BUY",
              "quantity": 10,
              "instrument": {"symbol": "BRK/B", "assetType": "EQUITY"}
            }],
            "orderActivityCollection": [
              {
                "activityId": 9001,
                "activityType": "EXECUTION",
                "executionType": "FILL",
                "executionLegs": [
                  {"legId": 1, "quantity": 6, "price": 560.10,
                   "time": "2026-11-05T14:30:02+0000"},
                  {"legId": 1, "quantity": 4, "price": 560.20,
                   "time": "2026-11-05T14:30:01+0000"}
                ]
              },
              {
                "activityId": 9000,
                "activityType": "EXECUTION",
                "executionType": "FILL",
                "executionLegs": [
                  {"legId": 1, "quantity": 2, "price": 559.00,
                   "time": "2026-11-05T13:00:00+0000"}
                ]
              }
            ]
          }
        ])");
    };

    auto venue = Venue();
    auto fills = venue.GetFills(At("2026-11-05T14:00:00Z"));
    ASSERT_OK(fills);
    ASSERT_EQ(fills->size(), 2u);
    // Sorted chronologically by fill.time.
    EXPECT_EQ((*fills)[0].fill_id, "700:9001:1");
    EXPECT_EQ((*fills)[0].order_id, "700");
    EXPECT_EQ((*fills)[0].symbol, "BRK.B");
    EXPECT_EQ((*fills)[0].side, Side::kBuy);
    EXPECT_EQ((*fills)[0].quantity, D("4"));
    EXPECT_EQ((*fills)[0].price, D("560.2"));
    EXPECT_EQ((*fills)[0].fee, D("0"));
    EXPECT_EQ((*fills)[0].fee_asset, "USD");
    EXPECT_EQ((*fills)[0].time, At("2026-11-05T14:30:01Z"));

    EXPECT_EQ((*fills)[1].fill_id, "700:9001:0");
    EXPECT_EQ((*fills)[1].quantity, D("6"));
    EXPECT_EQ((*fills)[1].price, D("560.1"));
    EXPECT_EQ((*fills)[1].time, At("2026-11-05T14:30:02Z"));

    // Epoch 0 since also succeeds and returns all 3 fills.
    auto all_fills = venue.GetFills(types::Timestamp{});
    ASSERT_OK(all_fills);
    EXPECT_EQ(all_fills->size(), 3u);
}

TEST_F(SchwabTradingVenueTest, ParsesPositionsAndBalancesFromAccountEndpoint) {
    mHttp->reply = [](const net::HttpRequest& r) {
        if (r.url.ends_with("/accounts/accountNumbers")) {
            return Ok(kSingleAccount);
        }
        EXPECT_EQ(r.url, "https://api.test/trader/v1/accounts/HASH123");
        EXPECT_EQ(r.query.at("fields"), "positions");
        return Ok(R"({
          "securitiesAccount": {
            "type": "MARGIN",
            "accountNumber": "12345678",
            "positions": [
              {
                "longQuantity": 15,
                "shortQuantity": 0,
                "averageLongPrice": 570.25,
                "averagePrice": 570.00,
                "instrument": {"symbol": "BRK/B", "assetType": "EQUITY"}
              },
              {
                "longQuantity": 0,
                "shortQuantity": 5,
                "averageShortPrice": 230.50,
                "instrument": {"symbol": "AAPL", "assetType": "EQUITY"}
              },
              {
                "longQuantity": 1000,
                "shortQuantity": 0,
                "averagePrice": 1.00,
                "instrument": {"symbol": "SWVXX", "assetType": "CASH_EQUIVALENT"}
              }
            ],
            "currentBalances": {
              "cashBalance": 12000.50,
              "availableFunds": 9500.25
            }
          }
        })");
    };

    auto venue = Venue();
    auto positions = venue.GetPositions();
    ASSERT_OK(positions);
    ASSERT_EQ(positions->size(), 2u);
    EXPECT_EQ((*positions)[0].symbol, "BRK.B");
    EXPECT_EQ((*positions)[0].quantity, D("15"));
    EXPECT_EQ((*positions)[0].average_price, D("570.25"));
    EXPECT_EQ((*positions)[0].realized_pnl, D("0"));

    EXPECT_EQ((*positions)[1].symbol, "AAPL");
    EXPECT_EQ((*positions)[1].quantity, D("-5"));
    EXPECT_EQ((*positions)[1].average_price, D("230.5"));

    auto balances = venue.GetBalances();
    ASSERT_OK(balances);
    ASSERT_EQ(balances->size(), 2u);
    EXPECT_EQ((*balances)[0].asset, "USD");
    EXPECT_EQ((*balances)[0].total, D("12000.50"));
    EXPECT_EQ((*balances)[0].available, D("9500.25"));
    EXPECT_EQ((*balances)[0].locked, D("2500.25"));

    EXPECT_EQ((*balances)[1].asset, "BRK.B");
    EXPECT_EQ((*balances)[1].total, D("15"));
    EXPECT_EQ((*balances)[1].available, D("15"));
    EXPECT_EQ((*balances)[1].locked, D("0"));
}

TEST_F(SchwabTradingVenueTest, RetriesTransientGetFailures) {
    int attempts = 0;
    mHttp->reply = [&](const net::HttpRequest& r) {
        if (r.url.ends_with("/accounts/accountNumbers")) {
            return Ok(kSingleAccount);
        }
        ++attempts;
        if (attempts < 3) {
            return HttpStatus(503);
        }
        return Ok(
            R"({"securitiesAccount":{"currentBalances":{"totalCash":500,"cashAvailableForTrading":500}}})");
    };

    auto balances = Venue().GetBalances();
    ASSERT_OK(balances);
    EXPECT_EQ(attempts, 3);
    ASSERT_EQ(balances->size(), 1u);
    EXPECT_EQ((*balances)[0].total, D("500"));
}

TEST_F(SchwabTradingVenueTest,
       DefaultOptionsSleeperDoesNotThrowOnTransientGetRetry) {
    int attempts = 0;
    mHttp->reply = [&](const net::HttpRequest& r) {
        if (r.url.ends_with("/accounts/accountNumbers")) {
            return Ok(kSingleAccount);
        }
        ++attempts;
        if (attempts == 1) {
            return HttpStatus(503);
        }
        return Ok(
            R"({"securitiesAccount":{"currentBalances":{"totalCash":250,"cashAvailableForTrading":250}}})");
    };

    // Construct with default Options::sleeper (not overridden by the test
    // fixture) and a 1ms retry delay so production retry sleeping is exercised.
    SchwabTradingVenue::Options opts;
    opts.base_url = "https://api.test/trader/v1";
    opts.retry.max_attempts = 2;
    opts.retry.initial_delay = std::chrono::milliseconds(1);
    SchwabTradingVenue venue(
        std::make_shared<data::SchwabApi>(mAuth, mHttp,
                                          data::SchwabApi::Options{1000, 1000}),
        mData, std::move(opts));

    auto balances = venue.GetBalances();
    ASSERT_OK(balances);
    EXPECT_EQ(attempts, 2);
    ASSERT_EQ(balances->size(), 1u);
    EXPECT_EQ((*balances)[0].total, D("250"));
}

TEST_F(SchwabTradingVenueTest,
       HandlesSpecEnumsSkipsUnsupportedOrdersInListsAndFailsOnMalformedJson) {
    mHttp->reply = [](const net::HttpRequest& r) {
        if (r.url.ends_with("/accounts/accountNumbers")) {
            return Ok(kSingleAccount);
        }
        if (r.url.ends_with("/orders/804")) {
            return Ok(R"({
              "orderId": 804,
              "status": "WORKING",
              "orderType": "CABINET",
              "duration": "DAY",
              "quantity": 1,
              "filledQuantity": 0,
              "enteredTime": "2026-11-05T14:30:00+0000",
              "orderLegCollection": [{
                "instruction": "BUY",
                "quantity": 1,
                "instrument": {"symbol": "SPY", "assetType": "EQUITY"}
              }]
            })");
        }
        return Ok(R"([
          {
            "orderId": 801,
            "status": "PENDING_ACKNOWLEDGEMENT",
            "orderType": "LIMIT",
            "duration": "END_OF_WEEK",
            "quantity": 10,
            "filledQuantity": 0,
            "price": 500,
            "enteredTime": "2026-11-05T14:30:00+0000",
            "orderLegCollection": [{
              "instruction": "BUY",
              "quantity": 10,
              "instrument": {"symbol": "SPY", "assetType": "COLLECTIVE_INVESTMENT"}
            }]
          },
          {
            "orderId": 802,
            "status": "PENDING_RECALL",
            "orderType": "LIMIT",
            "duration": "NEXT_END_OF_MONTH",
            "quantity": 8,
            "filledQuantity": 3,
            "price": 230,
            "enteredTime": "2026-11-05T14:31:00+0000",
            "orderLegCollection": [{
              "instruction": "SELL_SHORT_EXEMPT",
              "quantity": 8,
              "instrument": {"symbol": "AAPL", "assetType": "EQUITY"}
            }]
          },
          {
            "orderId": 803,
            "status": "UNKNOWN",
            "orderType": "LIMIT",
            "duration": "DAY",
            "quantity": 5,
            "filledQuantity": 0,
            "price": 100,
            "orderLegCollection": [{
              "instruction": "BUY",
              "quantity": 5,
              "instrument": {"symbol": "SPY", "assetType": "EQUITY"}
            }]
          },
          {
            "orderId": 808,
            "status": "FUTURE_UNRECOGNIZED_STATUS",
            "orderType": "LIMIT",
            "duration": "DAY",
            "quantity": 5,
            "filledQuantity": 0,
            "price": 100,
            "orderLegCollection": [{
              "instruction": "BUY",
              "quantity": 5,
              "instrument": {"symbol": "SPY", "assetType": "EQUITY"}
            }]
          },
          {
            "orderId": 804,
            "status": "WORKING",
            "orderType": "CABINET",
            "duration": "DAY",
            "quantity": 1,
            "filledQuantity": 0,
            "orderLegCollection": [{
              "instruction": "BUY",
              "quantity": 1,
              "instrument": {"symbol": "SPY", "assetType": "EQUITY"}
            }]
          },
          {
            "orderId": 805,
            "status": "WORKING",
            "orderType": "MARKET",
            "duration": "DAY",
            "quantity": 10,
            "filledQuantity": 0,
            "orderLegCollection": [{
              "instruction": "EXCHANGE",
              "quantity": 10,
              "instrument": {"symbol": "SPY", "assetType": "EQUITY"}
            }]
          },
          {
            "orderId": 806,
            "status": "REPLACED",
            "orderType": "LIMIT",
            "duration": "END_OF_MONTH",
            "quantity": 4,
            "filledQuantity": 0,
            "price": 495,
            "orderLegCollection": [{
              "instruction": "BUY",
              "quantity": 4,
              "instrument": {"symbol": "SPY", "assetType": "COLLECTIVE_INVESTMENT"}
            }],
            "replacingOrderCollection": [{
              "orderId": 807,
              "status": "WORKING",
              "orderType": "LIMIT",
              "duration": "END_OF_MONTH",
              "quantity": 4,
              "filledQuantity": 0,
              "price": 496,
              "orderLegCollection": [{
                "instruction": "BUY",
                "quantity": 4,
                "instrument": {"symbol": "SPY", "assetType": "COLLECTIVE_INVESTMENT"}
              }]
            }]
          }
        ])");
    };

    auto venue = Venue();
    auto open = venue.GetOpenOrders();
    ASSERT_OK(open);
    ASSERT_EQ(open->size(), 4u);

    EXPECT_EQ((*open)[0].order_id, "801");
    EXPECT_EQ((*open)[0].symbol, "SPY");
    EXPECT_EQ((*open)[0].side, Side::kBuy);
    EXPECT_EQ((*open)[0].time_in_force, TimeInForce::kGtc);
    EXPECT_EQ((*open)[0].state, OrderState::kNew);

    EXPECT_EQ((*open)[1].order_id, "802");
    EXPECT_EQ((*open)[1].symbol, "AAPL");
    EXPECT_EQ((*open)[1].side, Side::kSell);
    EXPECT_EQ((*open)[1].time_in_force, TimeInForce::kGtc);
    EXPECT_EQ((*open)[1].state, OrderState::kPartiallyFilled);

    // Order 803 has status "UNKNOWN" and is conservatively treated as active.
    EXPECT_EQ((*open)[2].order_id, "803");
    EXPECT_EQ((*open)[2].state, OrderState::kNew);

    EXPECT_EQ((*open)[3].order_id, "807");
    EXPECT_EQ((*open)[3].symbol, "SPY");
    EXPECT_EQ((*open)[3].limit_price, D("496"));
    EXPECT_EQ((*open)[3].state, OrderState::kNew);

    // Asking directly for an unsupported order via GetOrder fails with
    // kUnsupported rather than silently skipping.
    auto direct_cabinet = venue.GetOrder("804");
    ASSERT_FALSE(direct_cabinet);
    EXPECT_EQ(direct_cabinet.error().category, ErrorCategory::kUnsupported);

    // Malformed JSON in GetOpenOrders still fails with kParse.
    mHttp->reply = [](const net::HttpRequest& r) {
        if (r.url.ends_with("/accounts/accountNumbers")) {
            return Ok(kSingleAccount);
        }
        return Ok(R"([{"status":"WORKING"}])");
    };
    auto malformed = venue.GetOpenOrders();
    ASSERT_FALSE(malformed);
    EXPECT_EQ(malformed.error().category, ErrorCategory::kParse);
}

TEST_F(SchwabTradingVenueTest,
       AcceptsCollectiveInvestmentAssetTypeAcrossPositionsBalancesAndFills) {
    mHttp->reply = [](const net::HttpRequest& r) {
        if (r.url.ends_with("/accounts/accountNumbers")) {
            return Ok(kSingleAccount);
        }
        if (r.url.ends_with("/orders")) {
            return Ok(R"([
              {
                "orderId": 910,
                "status": "FILLED",
                "orderType": "MARKET",
                "duration": "DAY",
                "quantity": 5,
                "filledQuantity": 5,
                "enteredTime": "2026-11-05T14:30:00+0000",
                "orderLegCollection": [{
                  "legId": 1,
                  "instruction": "BUY",
                  "quantity": 5,
                  "instrument": {
                    "symbol": "SPY",
                    "assetType": "COLLECTIVE_INVESTMENT"
                  }
                }],
                "orderActivityCollection": [{
                  "activityType": "EXECUTION",
                  "executionType": "FILL",
                  "executionLegs": [{
                    "legId": 1,
                    "quantity": 5,
                    "price": 580.25,
                    "time": "2026-11-05T14:30:01+0000"
                  }]
                }]
              }
            ])");
        }
        return Ok(R"({
          "securitiesAccount": {
            "type": "CASH",
            "accountNumber": "12345678",
            "positions": [{
              "longQuantity": 20,
              "shortQuantity": 0,
              "averageLongPrice": 575.50,
              "instrument": {
                "symbol": "SPY",
                "assetType": "COLLECTIVE_INVESTMENT"
              }
            }],
            "currentBalances": {
              "totalCash": 5000.00,
              "cashAvailableForTrading": 4200.00
            }
          }
        })");
    };

    auto venue = Venue();
    auto positions = venue.GetPositions();
    ASSERT_OK(positions);
    ASSERT_EQ(positions->size(), 1u);
    EXPECT_EQ((*positions)[0].symbol, "SPY");
    EXPECT_EQ((*positions)[0].quantity, D("20"));
    EXPECT_EQ((*positions)[0].average_price, D("575.5"));

    auto balances = venue.GetBalances();
    ASSERT_OK(balances);
    ASSERT_EQ(balances->size(), 2u);
    EXPECT_EQ((*balances)[0].asset, "USD");
    EXPECT_EQ((*balances)[0].total, D("5000"));
    EXPECT_EQ((*balances)[0].available, D("4200"));
    EXPECT_EQ((*balances)[0].locked, D("800"));
    EXPECT_EQ((*balances)[1].asset, "SPY");
    EXPECT_EQ((*balances)[1].total, D("20"));

    auto fills = venue.GetFills(At("2026-11-05T14:00:00Z"));
    ASSERT_OK(fills);
    ASSERT_EQ(fills->size(), 1u);
    EXPECT_EQ((*fills)[0].symbol, "SPY");
    EXPECT_EQ((*fills)[0].quantity, D("5"));
    EXPECT_EQ((*fills)[0].price, D("580.25"));
    // Without activityId, fallback fill_id embeds the execution epoch millis.
    EXPECT_EQ(
        (*fills)[0].fill_id,
        "910:" +
            std::to_string(types::ToEpochMillis(At("2026-11-05T14:30:01Z"))) +
            "-0:0");
}

TEST_F(SchwabTradingVenueTest,
       AdjustsTickSizeWhenOrderPriceCrossesOneDollarBoundary) {
    std::vector<nlohmann::json> posted;
    mHttp->reply = [&](const net::HttpRequest& r) {
        if (r.url.ends_with("/accounts/accountNumbers")) {
            return Ok(kSingleAccount);
        }
        posted.push_back(nlohmann::json::parse(r.body));
        return Created(
            "https://api.schwabapi.com/trader/v1/accounts/HASH123/orders/" +
            std::to_string(1000 + posted.size()));
    };

    auto venue = Venue();

    // TINY has a cached 0.0001 tick (from a sub-$1 quote), but an order at
    // $1.2345 is >= $1.00 so SEC Rule 612 requires a $0.01 tick ($1.23).
    types::OrderRequest above_dollar;
    above_dollar.symbol = "TINY";
    above_dollar.side = Side::kBuy;
    above_dollar.type = OrderType::kLimit;
    above_dollar.quantity = D("10");
    above_dollar.limit_price = D("1.2345");
    ASSERT_OK(venue.PlaceOrder(above_dollar));

    // TINY ordered below $1.00 retains its $0.0001 sub-penny tick.
    types::OrderRequest below_dollar = above_dollar;
    below_dollar.limit_price = D("0.5432");
    ASSERT_OK(venue.PlaceOrder(below_dollar));

    ASSERT_EQ(posted.size(), 2u);
    EXPECT_DOUBLE_EQ(posted[0].at("price").get<double>(), 1.23);
    EXPECT_DOUBLE_EQ(posted[1].at("price").get<double>(), 0.5432);
}

TEST_F(SchwabTradingVenueTest,
       RejectsNonNumericLocationHeaderAndReportsServerErrorWhenUnconfirmed) {
    mHttp->reply = [](const net::HttpRequest& r) {
        if (r.url.ends_with("/accounts/accountNumbers")) {
            return Ok(kSingleAccount);
        }
        if (r.method == net::HttpMethod::kPost) {
            return Created(
                "https://api.schwabapi.com/trader/v1/accounts/HASH123/orders/"
                "not-a-number");
        }
        return Ok("[]");
    };

    types::OrderRequest req;
    req.symbol = "SPY";
    req.side = Side::kBuy;
    req.type = OrderType::kLimit;
    req.quantity = D("1");
    req.limit_price = D("500");

    auto ack = Venue().PlaceOrder(req);
    ASSERT_FALSE(ack);
    EXPECT_EQ(ack.error().category, ErrorCategory::kServer);
    EXPECT_NE(ack.error().message.find("HTTP 201"), std::string::npos);
    EXPECT_NE(ack.error().message.find("check the open orders"),
              std::string::npos);
}

TEST_F(SchwabTradingVenueTest,
       RefusesToGuessWhenMultipleCandidateOrdersMatchTimedOutPlacement) {
    mHttp->reply = [](const net::HttpRequest& r) {
        if (r.url.ends_with("/accounts/accountNumbers")) {
            return Ok(kSingleAccount);
        }
        if (r.method == net::HttpMethod::kPost) {
            return types::Result<net::HttpResponse>(
                types::MakeError(ErrorCategory::kTimeout, "request timed out"));
        }
        return Ok(R"([
          {
            "orderId": 2001,
            "status": "WORKING",
            "orderType": "LIMIT",
            "duration": "GOOD_TILL_CANCEL",
            "quantity": 2,
            "filledQuantity": 0,
            "price": 575.25,
            "enteredTime": "2026-11-05T15:00:00+0000",
            "orderLegCollection": [{
              "instruction": "BUY",
              "quantity": 2,
              "instrument": {"symbol": "SPY", "assetType": "EQUITY"}
            }]
          },
          {
            "orderId": 2002,
            "status": "WORKING",
            "orderType": "LIMIT",
            "duration": "GOOD_TILL_CANCEL",
            "quantity": 2,
            "filledQuantity": 0,
            "price": 575.25,
            "enteredTime": "2026-11-05T15:00:01+0000",
            "orderLegCollection": [{
              "instruction": "BUY",
              "quantity": 2,
              "instrument": {"symbol": "SPY", "assetType": "EQUITY"}
            }]
          }
        ])");
    };

    types::OrderRequest req;
    req.symbol = "SPY";
    req.side = Side::kBuy;
    req.type = OrderType::kLimit;
    req.quantity = D("2");
    req.limit_price = D("575.25");
    req.time_in_force = TimeInForce::kGtc;

    auto ack = Venue().PlaceOrder(req);
    ASSERT_FALSE(ack);
    EXPECT_EQ(ack.error().category, ErrorCategory::kTimeout);
    EXPECT_NE(ack.error().message.find("it may have been placed"),
              std::string::npos);
}

TEST_F(SchwabTradingVenueTest,
       ValidatesOcoParametersAndHandlesImmediateFillOrRejectedSibling) {
    auto venue = Venue();

    types::OcoRequest bad;
    bad.symbol = "SPY";
    bad.side = Side::kSell;
    bad.quantity = D("10");
    bad.limit_price = D("480");
    bad.stop_price = D("500");
    auto sell_inverted = venue.PlaceOco(bad);
    ASSERT_FALSE(sell_inverted);
    EXPECT_EQ(sell_inverted.error().category, ErrorCategory::kInvalidArgument);

    bad.side = Side::kBuy;
    bad.limit_price = D("500");
    bad.stop_price = D("480");
    auto buy_inverted = venue.PlaceOco(bad);
    ASSERT_FALSE(buy_inverted);
    EXPECT_EQ(buy_inverted.error().category, ErrorCategory::kInvalidArgument);

    bad.side = Side::kSell;
    bad.limit_price = D("500.01");
    bad.stop_price = D("500.006");
    auto same_tick = venue.PlaceOco(bad);
    ASSERT_FALSE(same_tick);
    EXPECT_EQ(same_tick.error().category, ErrorCategory::kRejected);

    // 1. Immediate fill of the limit leg (which cancels the stop sibling) is
    //    recognized as a valid OCO placement.
    mHttp->reply = [](const net::HttpRequest& r) {
        if (r.url.ends_with("/accounts/accountNumbers")) {
            return Ok(kSingleAccount);
        }
        if (r.method == net::HttpMethod::kPost) {
            return Created(
                "https://api.schwabapi.com/trader/v1/accounts/HASH123/orders/"
                "6000");
        }
        return Ok(R"({
          "orderId": 6000,
          "orderStrategyType": "OCO",
          "status": "FILLED",
          "childOrderStrategies": [
            {
              "orderId": 6001,
              "orderStrategyType": "SINGLE",
              "orderType": "LIMIT",
              "duration": "GOOD_TILL_CANCEL",
              "status": "FILLED",
              "quantity": 10,
              "filledQuantity": 10,
              "price": 550,
              "enteredTime": "2026-11-05T15:00:00+0000",
              "orderLegCollection": [{
                "instruction": "SELL",
                "quantity": 10,
                "instrument": {"symbol": "SPY", "assetType": "EQUITY"}
              }]
            },
            {
              "orderId": 6002,
              "orderStrategyType": "SINGLE",
              "orderType": "STOP",
              "duration": "GOOD_TILL_CANCEL",
              "status": "CANCELED",
              "quantity": 10,
              "filledQuantity": 0,
              "stopPrice": 480,
              "enteredTime": "2026-11-05T15:00:00+0000",
              "orderLegCollection": [{
                "instruction": "SELL",
                "quantity": 10,
                "instrument": {"symbol": "SPY", "assetType": "EQUITY"}
              }]
            }
          ]
        })");
    };

    types::OcoRequest oco;
    oco.symbol = "SPY";
    oco.side = Side::kSell;
    oco.quantity = D("10");
    oco.limit_price = D("550");
    oco.stop_price = D("480");
    auto filled_ack = venue.PlaceOco(oco);
    ASSERT_OK(filled_ack);
    EXPECT_EQ(filled_ack->limit.order_id, "6001");
    EXPECT_EQ(filled_ack->limit.state, OrderState::kFilled);
    EXPECT_EQ(filled_ack->stop.order_id, "6002");
    EXPECT_EQ(filled_ack->stop.state, OrderState::kCancelled);

    // 2. If one leg is REJECTED while the other is WORKING, PlaceOco cancels
    //    the working leg and if that cancel fails, warns that the order may
    //    still be live.
    bool delete_called = false;
    mHttp->reply = [&](const net::HttpRequest& r) {
        if (r.url.ends_with("/accounts/accountNumbers")) {
            return Ok(kSingleAccount);
        }
        if (r.method == net::HttpMethod::kPost) {
            return Created(
                "https://api.schwabapi.com/trader/v1/accounts/HASH123/orders/"
                "6100");
        }
        if (r.method == net::HttpMethod::kDelete) {
            delete_called = true;
            return HttpStatus(500, R"({"message":"gateway down"})");
        }
        return Ok(R"({
          "orderId": 6100,
          "orderStrategyType": "OCO",
          "status": "WORKING",
          "childOrderStrategies": [
            {
              "orderId": 6101,
              "orderStrategyType": "SINGLE",
              "orderType": "LIMIT",
              "duration": "GOOD_TILL_CANCEL",
              "status": "WORKING",
              "quantity": 10,
              "filledQuantity": 0,
              "price": 550,
              "enteredTime": "2026-11-05T15:00:00+0000",
              "orderLegCollection": [{
                "instruction": "SELL",
                "quantity": 10,
                "instrument": {"symbol": "SPY", "assetType": "EQUITY"}
              }]
            },
            {
              "orderId": 6102,
              "orderStrategyType": "SINGLE",
              "orderType": "STOP",
              "duration": "GOOD_TILL_CANCEL",
              "status": "REJECTED",
              "quantity": 10,
              "filledQuantity": 0,
              "stopPrice": 480,
              "enteredTime": "2026-11-05T15:00:00+0000",
              "orderLegCollection": [{
                "instruction": "SELL",
                "quantity": 10,
                "instrument": {"symbol": "SPY", "assetType": "EQUITY"}
              }]
            }
          ]
        })");
    };

    auto rejected_ack = venue.PlaceOco(oco);
    ASSERT_FALSE(rejected_ack);
    EXPECT_TRUE(delete_called);
    EXPECT_EQ(rejected_ack.error().category, ErrorCategory::kRejected);
    EXPECT_NE(rejected_ack.error().message.find("it may still be working"),
              std::string::npos);
}

TEST_F(SchwabTradingVenueTest,
       RecoverPlacedOcoRequiresOcoParentAndCancelsLonePartialLeg) {
    // 1. After a 201 Created whose GET /orders/7000 fails, PlaceOco falls back
    //    to RecoverPlacedOco and matches the OCO parent (ignoring unrelated
    //    SINGLE orders).
    mHttp->reply = [](const net::HttpRequest& r) {
        if (r.url.ends_with("/accounts/accountNumbers")) {
            return Ok(kSingleAccount);
        }
        if (r.method == net::HttpMethod::kPost) {
            return Created(
                "https://api.schwabapi.com/trader/v1/accounts/HASH123/orders/"
                "7000");
        }
        if (r.url.ends_with("/orders/7000")) {
            return HttpStatus(503);
        }
        if (r.url.ends_with("/orders")) {
            return Ok(R"([
              {
                "orderId": 6999,
                "orderStrategyType": "SINGLE",
                "orderType": "LIMIT",
                "duration": "GOOD_TILL_CANCEL",
                "status": "WORKING",
                "quantity": 10,
                "filledQuantity": 0,
                "price": 550,
                "enteredTime": "2026-11-05T15:00:00+0000",
                "orderLegCollection": [{
                  "instruction": "SELL",
                  "quantity": 10,
                  "instrument": {"symbol": "SPY", "assetType": "EQUITY"}
                }]
              },
              {
                "orderId": 7000,
                "orderStrategyType": "OCO",
                "status": "WORKING",
                "childOrderStrategies": [
                  {
                    "orderId": 7001,
                    "orderStrategyType": "SINGLE",
                    "orderType": "LIMIT",
                    "duration": "GOOD_TILL_CANCEL",
                    "status": "WORKING",
                    "quantity": 10,
                    "filledQuantity": 0,
                    "price": 550,
                    "enteredTime": "2026-11-05T15:00:00+0000",
                    "orderLegCollection": [{
                      "instruction": "SELL",
                      "quantity": 10,
                      "instrument": {"symbol": "SPY", "assetType": "EQUITY"}
                    }]
                  },
                  {
                    "orderId": 7002,
                    "orderStrategyType": "SINGLE",
                    "orderType": "STOP",
                    "duration": "GOOD_TILL_CANCEL",
                    "status": "AWAITING_STOP_CONDITION",
                    "quantity": 10,
                    "filledQuantity": 0,
                    "stopPrice": 480,
                    "enteredTime": "2026-11-05T15:00:00+0000",
                    "orderLegCollection": [{
                      "instruction": "SELL",
                      "quantity": 10,
                      "instrument": {"symbol": "SPY", "assetType": "EQUITY"}
                    }]
                  }
                ]
              }
            ])");
        }
        return HttpStatus(404);
    };

    auto venue = Venue();
    types::OcoRequest oco;
    oco.symbol = "SPY";
    oco.side = Side::kSell;
    oco.quantity = D("10");
    oco.limit_price = D("550");
    oco.stop_price = D("480");

    auto recovered = venue.PlaceOco(oco);
    ASSERT_OK(recovered);
    EXPECT_EQ(recovered->limit.order_id, "7001");
    EXPECT_EQ(recovered->stop.order_id, "7002");

    // 2. If POST /orders times out and only one leg exists under a new OCO
    //    parent on the account, RecoverPlacedOco cancels that lone leg and
    //    returns kRejected.
    std::string cancelled_id;
    mHttp->reply = [&](const net::HttpRequest& r) {
        if (r.url.ends_with("/accounts/accountNumbers")) {
            return Ok(kSingleAccount);
        }
        if (r.method == net::HttpMethod::kPost) {
            return types::Result<net::HttpResponse>(
                types::MakeError(ErrorCategory::kTimeout, "request timed out"));
        }
        if (r.method == net::HttpMethod::kDelete) {
            cancelled_id = r.url.substr(r.url.rfind('/') + 1);
            return Ok("");
        }
        return Ok(R"([
          {
            "orderId": 7100,
            "orderStrategyType": "OCO",
            "status": "WORKING",
            "childOrderStrategies": [
              {
                "orderId": 7101,
                "orderStrategyType": "SINGLE",
                "orderType": "LIMIT",
                "duration": "GOOD_TILL_CANCEL",
                "status": "WORKING",
                "quantity": 10,
                "filledQuantity": 0,
                "price": 550,
                "enteredTime": "2026-11-05T15:00:00+0000",
                "orderLegCollection": [{
                  "instruction": "SELL",
                  "quantity": 10,
                  "instrument": {"symbol": "SPY", "assetType": "EQUITY"}
                }]
              }
            ]
          }
        ])");
    };

    auto lone_leg = venue.PlaceOco(oco);
    ASSERT_FALSE(lone_leg);
    EXPECT_EQ(lone_leg.error().category, ErrorCategory::kRejected);
    EXPECT_EQ(cancelled_id, "7101");
}

TEST_F(SchwabTradingVenueTest,
       CancelOrderHandlesTimeoutAndNotFoundAndCancelAllReportsPartialFailure) {
    // 1. DELETE times out, but GetOrder shows CANCELED -> kCancelled.
    mHttp->reply = [](const net::HttpRequest& r) {
        if (r.url.ends_with("/accounts/accountNumbers")) {
            return Ok(kSingleAccount);
        }
        if (r.method == net::HttpMethod::kDelete) {
            return types::Result<net::HttpResponse>(
                types::MakeError(ErrorCategory::kTimeout, "delete timed out"));
        }
        return Ok(R"({
          "orderId": 300,
          "status": "CANCELED",
          "orderType": "LIMIT",
          "duration": "DAY",
          "quantity": 5,
          "filledQuantity": 0,
          "price": 500,
          "enteredTime": "2026-11-05T14:30:00+0000",
          "orderLegCollection": [{
            "instruction": "BUY",
            "quantity": 5,
            "instrument": {"symbol": "SPY", "assetType": "EQUITY"}
          }]
        })");
    };

    auto venue = Venue();
    auto timed_out_cancel = venue.CancelOrder("300");
    ASSERT_OK(timed_out_cancel);
    EXPECT_EQ(*timed_out_cancel, CancelOutcome::kCancelled);

    // 2. 404 on both DELETE and GetOrder -> kNotFound.
    mHttp->reply = [](const net::HttpRequest& r) {
        if (r.url.ends_with("/accounts/accountNumbers")) {
            return Ok(kSingleAccount);
        }
        return HttpStatus(404, R"({"message":"Order not found"})");
    };
    auto not_found = venue.CancelOrder("404");
    ASSERT_FALSE(not_found);
    EXPECT_EQ(not_found.error().category, ErrorCategory::kNotFound);

    // 3. CancelAll attempts all open orders even when one fails, and reports
    //    the counts of failed and cancelled orders.
    std::vector<std::string> delete_attempts;
    mHttp->reply = [&](const net::HttpRequest& r) {
        if (r.url.ends_with("/accounts/accountNumbers")) {
            return Ok(kSingleAccount);
        }
        if (r.method == net::HttpMethod::kGet && r.url.ends_with("/orders")) {
            return Ok(R"([
              {
                "orderId": 301,
                "status": "WORKING",
                "orderType": "LIMIT",
                "duration": "DAY",
                "quantity": 1,
                "filledQuantity": 0,
                "price": 500,
                "orderLegCollection": [{
                  "instruction": "BUY",
                  "quantity": 1,
                  "instrument": {"symbol": "SPY", "assetType": "EQUITY"}
                }]
              },
              {
                "orderId": 302,
                "status": "WORKING",
                "orderType": "LIMIT",
                "duration": "DAY",
                "quantity": 1,
                "filledQuantity": 0,
                "price": 501,
                "orderLegCollection": [{
                  "instruction": "BUY",
                  "quantity": 1,
                  "instrument": {"symbol": "SPY", "assetType": "EQUITY"}
                }]
              },
              {
                "orderId": 303,
                "status": "WORKING",
                "orderType": "LIMIT",
                "duration": "DAY",
                "quantity": 1,
                "filledQuantity": 0,
                "price": 502,
                "orderLegCollection": [{
                  "instruction": "BUY",
                  "quantity": 1,
                  "instrument": {"symbol": "SPY", "assetType": "EQUITY"}
                }]
              }
            ])");
        }
        if (r.method == net::HttpMethod::kDelete) {
            const std::string id = r.url.substr(r.url.rfind('/') + 1);
            delete_attempts.push_back(id);
            if (id == "302") {
                return HttpStatus(500, R"({"message":"internal error"})");
            }
            return Ok("");
        }
        return HttpStatus(500);
    };

    auto cancel_all = venue.CancelAll();
    ASSERT_FALSE(cancel_all);
    EXPECT_EQ(delete_attempts, (std::vector<std::string>{"301", "302", "303"}));
    EXPECT_NE(
        cancel_all.error().message.find("1 of 3 cancels failed; 2 cancelled"),
        std::string::npos);
}

TEST_F(SchwabTradingVenueTest,
       GetOrderSynthesizesOcoParentStatusAndMatchesExactOrderId) {
    mHttp->reply = [](const net::HttpRequest& r) {
        if (r.url.ends_with("/accounts/accountNumbers")) {
            return Ok(kSingleAccount);
        }
        return Ok(R"({
          "orderId": 5000,
          "orderStrategyType": "OCO",
          "status": "WORKING",
          "childOrderStrategies": [
            {
              "orderId": 5001,
              "orderStrategyType": "SINGLE",
              "orderType": "LIMIT",
              "duration": "GOOD_TILL_CANCEL",
              "status": "WORKING",
              "quantity": 10,
              "filledQuantity": 0,
              "price": 550,
              "enteredTime": "2026-11-05T15:00:00+0000",
              "orderLegCollection": [{
                "instruction": "SELL",
                "quantity": 10,
                "instrument": {"symbol": "SPY", "assetType": "EQUITY"}
              }]
            },
            {
              "orderId": 5002,
              "orderStrategyType": "SINGLE",
              "orderType": "STOP",
              "duration": "GOOD_TILL_CANCEL",
              "status": "AWAITING_STOP_CONDITION",
              "quantity": 10,
              "filledQuantity": 0,
              "stopPrice": 480,
              "enteredTime": "2026-11-05T15:00:00+0000",
              "orderLegCollection": [{
                "instruction": "SELL",
                "quantity": 10,
                "instrument": {"symbol": "SPY", "assetType": "EQUITY"}
              }]
            }
          ]
        })");
    };

    auto venue = Venue();
    auto parent = venue.GetOrder("5000");
    ASSERT_OK(parent);
    EXPECT_EQ(parent->order_id, "5000");
    EXPECT_EQ(parent->state, OrderState::kNew);

    auto second_child = venue.GetOrder("5002");
    ASSERT_OK(second_child);
    EXPECT_EQ(second_child->order_id, "5002");
    EXPECT_EQ(second_child->type, OrderType::kStop);
    EXPECT_EQ(second_child->stop_price, D("480"));

    auto missing = venue.GetOrder("9999");
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().category, ErrorCategory::kNotFound);
}

TEST_F(SchwabTradingVenueTest,
       GetBalancesUsesIntradayMarginFieldsAndClampsMarginDebit) {
    // 1. Intraday MARGIN balances: currentBalances has 12000 (after buying
    //    3000 of SPY from 15000 morning initialBalances), and projectedBalances
    //    has 10000 (after an open 2000 buy order). GetBalances must use
    //    current/projected (12000 total, 10000 available, 2000 locked) rather
    //    than stale start-of-day initialBalances (15000).
    mHttp->reply = [](const net::HttpRequest& r) {
        if (r.url.ends_with("/accounts/accountNumbers")) {
            return Ok(kSingleAccount);
        }
        return Ok(R"({
          "securitiesAccount": {
            "type": "MARGIN",
            "accountNumber": "12345678",
            "initialBalances": {
              "cashBalance": 15000.00,
              "totalCash": 15000.00
            },
            "currentBalances": {
              "availableFundsNonMarginableTrade": 12000.00,
              "availableFunds": 24000.00,
              "buyingPower": 48000.00
            },
            "projectedBalances": {
              "availableFundsNonMarginableTrade": 10000.00,
              "availableFunds": 22000.00
            }
          }
        })");
    };

    auto venue = Venue();
    auto balances = venue.GetBalances();
    ASSERT_OK(balances);
    ASSERT_EQ(balances->size(), 1u);
    EXPECT_EQ((*balances)[0].asset, "USD");
    EXPECT_EQ((*balances)[0].total, D("12000"));
    EXPECT_EQ((*balances)[0].available, D("10000"));
    EXPECT_EQ((*balances)[0].locked, D("2000"));

    // 2. Margin debit: cashBalance is negative (-1500) while availableFunds
    //    reflects margin borrowing power (8000). USD total stays negative
    //    (-1500) so net equity subtracts the margin loan, while available and
    //    locked are 0.
    mHttp->reply = [](const net::HttpRequest& r) {
        if (r.url.ends_with("/accounts/accountNumbers")) {
            return Ok(kSingleAccount);
        }
        return Ok(R"({
          "securitiesAccount": {
            "type": "MARGIN",
            "accountNumber": "12345678",
            "currentBalances": {
              "cashBalance": -1500.00,
              "availableFundsNonMarginableTrade": 0.00,
              "availableFunds": 8000.00
            }
          }
        })");
    };

    auto debit_balances = venue.GetBalances();
    ASSERT_OK(debit_balances);
    ASSERT_EQ(debit_balances->size(), 1u);
    EXPECT_EQ((*debit_balances)[0].asset, "USD");
    EXPECT_EQ((*debit_balances)[0].total, D("-1500"));
    EXPECT_EQ((*debit_balances)[0].available, D("0"));
    EXPECT_EQ((*debit_balances)[0].locked, D("0"));

    // 3. Margin debit via negative `marginBalance` when `cashBalance` is absent
    //    (OpenAPI MarginBalance schema).
    mHttp->reply = [](const net::HttpRequest& r) {
        if (r.url.ends_with("/accounts/accountNumbers")) {
            return Ok(kSingleAccount);
        }
        return Ok(R"({
          "securitiesAccount": {
            "type": "MARGIN",
            "accountNumber": "12345678",
            "currentBalances": {
              "marginBalance": -2000.00,
              "availableFundsNonMarginableTrade": 0.00,
              "availableFunds": 6000.00
            }
          }
        })");
    };

    auto margin_bal_debit = venue.GetBalances();
    ASSERT_OK(margin_bal_debit);
    ASSERT_EQ(margin_bal_debit->size(), 1u);
    EXPECT_EQ((*margin_bal_debit)[0].asset, "USD");
    EXPECT_EQ((*margin_bal_debit)[0].total, D("-2000"));
    EXPECT_EQ((*margin_bal_debit)[0].available, D("0"));
    EXPECT_EQ((*margin_bal_debit)[0].locked, D("0"));
}

TEST_F(SchwabTradingVenueTest,
       RecoversPlacedOrderEvenWhenConcurrentGetOpenOrdersPollsFirst) {
    auto venue = Venue();
    bool post_seen = false;
    mHttp->reply = [&](const net::HttpRequest& r) {
        if (r.url.ends_with("/accounts/accountNumbers")) {
            return Ok(kSingleAccount);
        }
        if (r.method == net::HttpMethod::kPost) {
            post_seen = true;
            // Simulate a concurrent Runner/RiskGate poll calling GetOpenOrders
            // after the order landed at Schwab but before PlaceOrder's POST
            // returns its timeout.
            auto polled = venue.GetOpenOrders();
            EXPECT_TRUE(polled.has_value());
            if (polled) {
                EXPECT_EQ(polled->size(), 1u);
            }
            return types::Result<net::HttpResponse>(
                types::MakeError(ErrorCategory::kTimeout, "request timed out"));
        }
        if (!post_seen) {
            return Ok("[]");
        }
        return Ok(R"([
          {
            "orderId": 4242,
            "orderStrategyType": "SINGLE",
            "status": "WORKING",
            "orderType": "LIMIT",
            "duration": "GOOD_TILL_CANCEL",
            "quantity": 3,
            "filledQuantity": 0,
            "price": 510.25,
            "enteredTime": "2026-11-05T15:00:00+0000",
            "orderLegCollection": [{
              "instruction": "BUY",
              "quantity": 3,
              "instrument": {"symbol": "SPY", "assetType": "EQUITY"}
            }]
          }
        ])");
    };

    types::OrderRequest req;
    req.symbol = "SPY";
    req.side = Side::kBuy;
    req.type = OrderType::kLimit;
    req.quantity = D("3");
    req.limit_price = D("510.25");
    req.time_in_force = TimeInForce::kGtc;
    req.client_order_id = "cid-4242";

    auto ack = venue.PlaceOrder(req);
    ASSERT_OK(ack);
    EXPECT_EQ(ack->order_id, "4242");
    EXPECT_EQ(ack->client_order_id, "cid-4242");
}

TEST_F(
    SchwabTradingVenueTest,
    SingleOrderRecoveryIgnoresOcoChildrenAndOcoAcknowledgesFilledLegWhenSiblingRejected) {
    // 1. A timed-out SINGLE order must not match a child leg of a concurrent
    //    OCO order even if the child leg has identical symbol/side/type/price.
    mHttp->reply = [](const net::HttpRequest& r) {
        if (r.url.ends_with("/accounts/accountNumbers")) {
            return Ok(kSingleAccount);
        }
        if (r.method == net::HttpMethod::kPost) {
            return types::Result<net::HttpResponse>(
                types::MakeError(ErrorCategory::kTimeout, "request timed out"));
        }
        return Ok(R"([
          {
            "orderId": 8800,
            "orderStrategyType": "OCO",
            "status": "WORKING",
            "childOrderStrategies": [{
              "orderId": 8801,
              "orderStrategyType": "SINGLE",
              "status": "WORKING",
              "orderType": "LIMIT",
              "duration": "GOOD_TILL_CANCEL",
              "quantity": 5,
              "filledQuantity": 0,
              "price": 550,
              "enteredTime": "2026-11-05T15:00:00+0000",
              "orderLegCollection": [{
                "instruction": "SELL",
                "quantity": 5,
                "instrument": {"symbol": "SPY", "assetType": "EQUITY"}
              }]
            }]
          }
        ])");
    };

    auto venue = Venue();
    types::OrderRequest single_req;
    single_req.symbol = "SPY";
    single_req.side = Side::kSell;
    single_req.type = OrderType::kLimit;
    single_req.quantity = D("5");
    single_req.limit_price = D("550");
    single_req.time_in_force = TimeInForce::kGtc;

    auto single_ack = venue.PlaceOrder(single_req);
    ASSERT_FALSE(single_ack);
    EXPECT_EQ(single_ack.error().category, ErrorCategory::kTimeout);

    // 2. In PlaceOco, if one leg has filled while its sibling is REJECTED,
    //    PlaceOco returns Ok with the actual states rather than kRejected.
    mHttp->reply = [](const net::HttpRequest& r) {
        if (r.url.ends_with("/accounts/accountNumbers")) {
            return Ok(kSingleAccount);
        }
        if (r.method == net::HttpMethod::kPost) {
            return Created(
                "https://api.schwabapi.com/trader/v1/accounts/HASH123/orders/"
                "8900");
        }
        return Ok(R"({
          "orderId": 8900,
          "orderStrategyType": "OCO",
          "status": "FILLED",
          "childOrderStrategies": [
            {
              "orderId": 8901,
              "orderStrategyType": "SINGLE",
              "orderType": "LIMIT",
              "duration": "GOOD_TILL_CANCEL",
              "status": "FILLED",
              "quantity": 5,
              "filledQuantity": 5,
              "price": 550,
              "enteredTime": "2026-11-05T15:00:00+0000",
              "orderLegCollection": [{
                "instruction": "SELL",
                "quantity": 5,
                "instrument": {"symbol": "SPY", "assetType": "EQUITY"}
              }]
            },
            {
              "orderId": 8902,
              "orderStrategyType": "SINGLE",
              "orderType": "STOP",
              "duration": "GOOD_TILL_CANCEL",
              "status": "REJECTED",
              "quantity": 5,
              "filledQuantity": 0,
              "stopPrice": 480,
              "enteredTime": "2026-11-05T15:00:00+0000",
              "orderLegCollection": [{
                "instruction": "SELL",
                "quantity": 5,
                "instrument": {"symbol": "SPY", "assetType": "EQUITY"}
              }]
            }
          ]
        })");
    };

    types::OcoRequest oco;
    oco.symbol = "SPY";
    oco.side = Side::kSell;
    oco.quantity = D("5");
    oco.limit_price = D("550");
    oco.stop_price = D("480");
    auto oco_ack = venue.PlaceOco(oco);
    ASSERT_OK(oco_ack);
    EXPECT_EQ(oco_ack->limit.order_id, "8901");
    EXPECT_EQ(oco_ack->limit.state, OrderState::kFilled);
    EXPECT_EQ(oco_ack->stop.order_id, "8902");
    EXPECT_EQ(oco_ack->stop.state, OrderState::kRejected);

    // 3. GetOrder on a supported child ("5002") succeeds even when sibling
    //    "5001" has an unsupported orderType ("CABINET"), while
    //    GetOrder("5001") returns kUnsupported.
    mHttp->reply = [](const net::HttpRequest& r) {
        if (r.url.ends_with("/accounts/accountNumbers")) {
            return Ok(kSingleAccount);
        }
        return Ok(R"({
          "orderId": 5000,
          "orderStrategyType": "OCO",
          "status": "WORKING",
          "childOrderStrategies": [
            {
              "orderId": 5001,
              "orderStrategyType": "SINGLE",
              "orderType": "CABINET",
              "duration": "DAY",
              "status": "WORKING",
              "quantity": 10,
              "filledQuantity": 0,
              "orderLegCollection": [{
                "instruction": "SELL",
                "quantity": 10,
                "instrument": {"symbol": "SPY", "assetType": "EQUITY"}
              }]
            },
            {
              "orderId": 5002,
              "orderStrategyType": "SINGLE",
              "orderType": "STOP",
              "duration": "GOOD_TILL_CANCEL",
              "status": "AWAITING_STOP_CONDITION",
              "quantity": 10,
              "filledQuantity": 0,
              "stopPrice": 480,
              "orderLegCollection": [{
                "instruction": "SELL",
                "quantity": 10,
                "instrument": {"symbol": "SPY", "assetType": "EQUITY"}
              }]
            }
          ]
        })");
    };

    auto valid_child = venue.GetOrder("5002");
    ASSERT_OK(valid_child);
    EXPECT_EQ(valid_child->order_id, "5002");

    auto unsupported_child = venue.GetOrder("5001");
    ASSERT_FALSE(unsupported_child);
    EXPECT_EQ(unsupported_child.error().category, ErrorCategory::kUnsupported);
}

TEST_F(SchwabTradingVenueTest,
       FormatsOrdersQueryTimestampsAsUtcIso8601With180DayFillLookback) {
    std::string from_param;
    std::string to_param;
    mHttp->reply = [&](const net::HttpRequest& r) {
        if (r.url.ends_with("/accounts/accountNumbers")) {
            return Ok(kSingleAccount);
        }
        if (r.url.ends_with("/transactions")) {
            return HttpStatus(403);  // so the fills come from the long fetch
        }
        from_param = r.query.at("fromEnteredTime");
        to_param = r.query.at("toEnteredTime");
        return Ok("[]");
    };

    auto venue = Venue();
    // mNow is 2026-11-05T15:00:00Z; since is 2026-11-05T14:00:00Z.
    // 180 days before 2026-11-05T14:00:00Z is 2026-05-09T14:00:00.000Z,
    // and toEnteredTime is mNow + 5min = 2026-11-05T15:05:00.000Z.
    ASSERT_OK(venue.GetFills(At("2026-11-05T14:00:00Z")));
    EXPECT_EQ(from_param, "2026-05-09T14:00:00.000Z");
    EXPECT_EQ(to_param, "2026-11-05T15:05:00.000Z");
}

// A working order on `symbol`, entered at `entered`, with one fill if
// `fill_time` is given.
std::string WorkingOrder(int id, std::string_view symbol,
                         std::string_view entered,
                         std::string_view fill_time = {}) {
    json order = {
        {"orderId", id},
        {"status", fill_time.empty() ? "WORKING" : "FILLED"},
        {"orderType", "LIMIT"},
        {"duration", "GOOD_TILL_CANCEL"},
        {"quantity", 1},
        {"filledQuantity", fill_time.empty() ? 0 : 1},
        {"price", 100},
        {"enteredTime", entered},
        {"orderLegCollection",
         {{{"legId", 1},
           {"instruction", "BUY"},
           {"quantity", 1},
           {"instrument", {{"symbol", symbol}, {"assetType", "EQUITY"}}}}}}};
    if (!fill_time.empty()) {
        order["orderActivityCollection"] = {{{"activityId", id * 10},
                                             {"activityType", "EXECUTION"},
                                             {"executionType", "FILL"},
                                             {"executionLegs",
                                              {{{"legId", 1},
                                                {"quantity", 1},
                                                {"price", 100},
                                                {"time", fill_time}}}}}};
    }
    return order.dump();
}

TEST_F(SchwabTradingVenueTest, OnlyTheConfiguredSymbolsOrdersAndFillsAreOurs) {
    std::vector<std::string> deleted;
    mHttp->reply = [&](const net::HttpRequest& r) {
        if (r.url.ends_with("/accounts/accountNumbers")) {
            return Ok(kSingleAccount);
        }
        if (r.method == net::HttpMethod::kDelete) {
            deleted.push_back(r.url.substr(r.url.rfind('/') + 1));
            return Ok("");
        }
        return Ok("[" + WorkingOrder(1, "SPY", "2026-11-05T14:00:00+0000") +
                  "," + WorkingOrder(2, "AAPL", "2026-11-05T14:00:00+0000") +
                  "," +
                  WorkingOrder(3, "SPY", "2026-11-05T14:10:00+0000",
                               "2026-11-05T14:11:00+0000") +
                  "," +
                  WorkingOrder(4, "AAPL", "2026-11-05T14:10:00+0000",
                               "2026-11-05T14:12:00+0000") +
                  "]");
    };
    auto venue = Venue("", {"SPY"});

    auto open = venue.GetOpenOrders();
    ASSERT_OK(open);
    ASSERT_EQ(open->size(), 1u);
    EXPECT_EQ((*open)[0].order_id, "1");

    auto fills = venue.GetFills(At("2026-11-05T14:00:00Z"));
    ASSERT_OK(fills);
    ASSERT_EQ(fills->size(), 1u);
    EXPECT_EQ((*fills)[0].order_id, "3");

    // The AAPL order (placed by hand, say) is left working.
    auto cancelled = venue.CancelAll();
    ASSERT_OK(cancelled);
    EXPECT_EQ(*cancelled, std::vector<std::string>{"1"});
    EXPECT_EQ(deleted, std::vector<std::string>{"1"});
}

TEST_F(SchwabTradingVenueTest, OrderFetchesStartAtTheOldestWorkingOrder) {
    std::vector<std::string> froms;
    int transactions = 0;
    std::string orders =
        "[" + WorkingOrder(1, "SPY", "2026-11-04T10:00:00+0000") + "]";
    mHttp->reply = [&](const net::HttpRequest& r) {
        if (r.url.ends_with("/accounts/accountNumbers")) {
            return Ok(kSingleAccount);
        }
        if (r.url.ends_with("/transactions")) {
            ++transactions;
            return Ok("[]");
        }
        froms.push_back(r.query.at("fromEnteredTime"));
        return Ok(orders);
    };
    auto venue = Venue("", {"SPY"});
    const auto from = [&](std::size_t i) {
        return *types::ParseIso8601(froms.at(i));
    };

    // Nothing fetched yet: as far back as a GTC order lives.
    ASSERT_OK(venue.GetOpenOrders());
    EXPECT_EQ(from(0), mNow - std::chrono::days(180));

    // Then from the order working at the last fetch (less the clock margin).
    mNow += 1min;
    ASSERT_OK(venue.GetOpenOrders());
    EXPECT_EQ(from(1), At("2026-11-04T09:55:00Z"));
    ASSERT_OK(venue.GetFills(mNow));
    EXPECT_EQ(from(2), At("2026-11-04T09:55:00Z"));

    // With nothing working, from the fetch itself.
    orders = "[]";
    mNow += 1min;
    ASSERT_OK(venue.GetOpenOrders());
    const auto quiet = mNow;
    mNow += 1min;
    ASSERT_OK(venue.GetOpenOrders());
    EXPECT_EQ(from(4), quiet - 5min);

    // Fills since before that fetch still reach back to the order that was
    // working then.
    ASSERT_OK(venue.GetFills(quiet - 30s));
    EXPECT_EQ(from(5), At("2026-11-04T09:55:00Z"));

    EXPECT_EQ(transactions, 0);

    // Fills since before any fetch: the orders entered since then (and the
    // older ones the transactions show trading, none here).
    const auto early = At("2026-11-01T00:00:00Z");
    ASSERT_OK(venue.GetFills(early));
    EXPECT_EQ(transactions, 1);
    EXPECT_EQ(from(6), early - 5min);
}

// A TRADE transaction for order `order_id` on `symbol`.
std::string Trade(int order_id, std::string_view symbol) {
    return json{{"activityId", order_id * 10 + 1},
                {"type", "TRADE"},
                {"orderId", order_id},
                {"transferItems",
                 {{{"instrument",
                    {{"assetType", "CURRENCY"}, {"symbol", "CURRENCY_USD"}}},
                   {"feeType", "COMMISSION"}},
                  {{"instrument",
                    {{"assetType", "EQUITY"}, {"symbol", symbol}}},
                   {"amount", 1}}}}}
        .dump();
}

TEST_F(SchwabTradingVenueTest,
       FillsBeforeAnyFetchComeFromTheWindowAndTheOrdersTradedInIt) {
    std::map<std::string, std::string> transaction_query;
    std::string orders_from;
    std::vector<std::string> looked_up;
    mHttp->reply = [&](const net::HttpRequest& r) {
        if (r.url.ends_with("/accounts/accountNumbers")) {
            return Ok(kSingleAccount);
        }
        if (r.url.ends_with("/transactions")) {
            transaction_query = r.query;
            // 3 was entered in the window, 5 before it, 9 is not ours.
            return Ok("[" + Trade(3, "SPY") + "," + Trade(5, "SPY") + "," +
                      Trade(9, "AAPL") + "," + Trade(5, "SPY") +
                      R"(,{"type":"RECEIVE_AND_DELIVER","orderId":7}])");
        }
        if (r.url.ends_with("/orders")) {
            orders_from = r.query.at("fromEnteredTime");
            return Ok("[" +
                      WorkingOrder(3, "SPY", "2026-11-05T14:20:00+0000",
                                   "2026-11-05T14:21:00+0000") +
                      "]");
        }
        looked_up.push_back(r.url.substr(r.url.rfind('/') + 1));
        return Ok(WorkingOrder(5, "SPY", "2026-10-01T14:00:00+0000",
                               "2026-11-05T14:30:00+0000"));
    };
    auto venue = Venue("", {"SPY"});

    auto fills = venue.GetFills(At("2026-11-05T14:00:00Z"));
    ASSERT_OK(fills);
    EXPECT_EQ(transaction_query["startDate"], "2026-11-05T13:55:00.000Z");
    EXPECT_EQ(transaction_query["endDate"], "2026-11-05T15:05:00.000Z");
    EXPECT_EQ(transaction_query["types"], "TRADE");
    EXPECT_EQ(orders_from, "2026-11-05T13:55:00.000Z");
    EXPECT_EQ(looked_up, std::vector<std::string>{"5"});
    ASSERT_EQ(fills->size(), 2u);
    EXPECT_EQ((*fills)[0].fill_id, "3:30:0");
    EXPECT_EQ((*fills)[1].fill_id, "5:50:0");
}

TEST_F(SchwabTradingVenueTest,
       FillsFallBackToTheLongFetchWhenAnOlderOrderIsNotKnown) {
    // A trade without an order ID, and a lookup that fails.
    for (const std::string& trade :
         {std::string(R"({"type":"TRADE","activityId":1})"),
          Trade(5, "SPY")}) {
        std::vector<std::string> froms;
        mHttp->reply = [&](const net::HttpRequest& r) {
            if (r.url.ends_with("/accounts/accountNumbers")) {
                return Ok(kSingleAccount);
            }
            if (r.url.ends_with("/transactions")) {
                return Ok("[" + trade + "]");
            }
            if (r.url.ends_with("/orders")) {
                froms.push_back(r.query.at("fromEnteredTime"));
                return Ok("[]");
            }
            return HttpStatus(404);
        };
        auto venue = Venue("", {"SPY"});
        const auto since = At("2026-11-05T14:00:00Z");
        ASSERT_OK(venue.GetFills(since));
        ASSERT_FALSE(froms.empty()) << trade;
        EXPECT_EQ(*types::ParseIso8601(froms.back()),
                  since - std::chrono::days(180))
            << trade;
    }
}

TEST_F(SchwabTradingVenueTest,
       AnOcoLegTradedInTheWindowIsReadFromItsParentNotLookedUp) {
    json parent = {{"orderId", 11},
                   {"status", "FILLED"},
                   {"orderStrategyType", "OCO"},
                   {"enteredTime", "2026-11-05T14:20:00+0000"},
                   {"childOrderStrategies",
                    {json::parse(WorkingOrder(12, "SPY",
                                              "2026-11-05T14:20:00+0000",
                                              "2026-11-05T14:25:00+0000"))}}};
    int lookups = 0;
    mHttp->reply = [&](const net::HttpRequest& r) {
        if (r.url.ends_with("/accounts/accountNumbers")) {
            return Ok(kSingleAccount);
        }
        if (r.url.ends_with("/transactions")) {
            return Ok("[" + Trade(12, "SPY") + "]");
        }
        if (r.url.ends_with("/orders")) {
            return Ok("[" + parent.dump() + "]");
        }
        ++lookups;
        return HttpStatus(404);
    };
    auto venue = Venue("", {"SPY"});
    auto fills = venue.GetFills(At("2026-11-05T14:00:00Z"));
    ASSERT_OK(fills);
    EXPECT_EQ(lookups, 0);
    ASSERT_EQ(fills->size(), 1u);
    EXPECT_EQ((*fills)[0].fill_id, "12:120:0");
}

TEST_F(SchwabTradingVenueTest,
       FillsFallBackToTheLongFetchWithTooManyOlderOrdersTraded) {
    std::vector<std::string> froms;
    int lookups = 0;
    mHttp->reply = [&](const net::HttpRequest& r) {
        if (r.url.ends_with("/accounts/accountNumbers")) {
            return Ok(kSingleAccount);
        }
        if (r.url.ends_with("/transactions")) {
            std::string trades = "[";
            for (int id = 1; id <= 9; ++id) {
                trades += (id > 1 ? "," : "") + Trade(id, "SPY");
            }
            return Ok(trades + "]");
        }
        if (r.url.ends_with("/orders")) {
            froms.push_back(r.query.at("fromEnteredTime"));
            return Ok("[]");
        }
        ++lookups;
        return Ok("{}");
    };
    auto venue = Venue("", {"SPY"});

    const auto since = At("2026-11-05T14:00:00Z");
    ASSERT_OK(venue.GetFills(since));
    EXPECT_EQ(lookups, 0);
    ASSERT_EQ(froms.size(), 2u);
    EXPECT_EQ(*types::ParseIso8601(froms[0]), since - 5min);
    EXPECT_EQ(*types::ParseIso8601(froms[1]), since - std::chrono::days(180));
}

TEST_F(SchwabTradingVenueTest, AnOrderFilledDuringAFetchStillWidensTheNext) {
    std::vector<std::string> froms;
    // Entered a day ago, filled as the fetch ran.
    const std::string orders =
        "[" +
        WorkingOrder(1, "SPY", "2026-11-04T15:00:00+0000",
                     "2026-11-05T15:00:00+0000") +
        "]";
    mHttp->reply = [&](const net::HttpRequest& r) {
        if (r.url.ends_with("/accounts/accountNumbers")) {
            return Ok(kSingleAccount);
        }
        froms.push_back(r.query.at("fromEnteredTime"));
        return Ok(orders);
    };
    auto venue = Venue("", {"SPY"});
    ASSERT_OK(venue.GetOpenOrders());
    mNow += 1min;
    auto fills = venue.GetFills(mNow - 1min);
    ASSERT_OK(fills);
    EXPECT_EQ(fills->size(), 1u);
    EXPECT_EQ(*types::ParseIso8601(froms.at(1)), At("2026-11-04T14:55:00Z"));
}

TEST_F(SchwabTradingVenueTest, AFetchAtTheCapIsSplitInTwo) {
    std::vector<std::pair<std::string, std::string>> windows;
    mHttp->reply = [&](const net::HttpRequest& r) {
        if (r.url.ends_with("/accounts/accountNumbers")) {
            return Ok(kSingleAccount);
        }
        windows.emplace_back(r.query.at("fromEnteredTime"),
                             r.query.at("toEnteredTime"));
        if (windows.size() == 1) {  // the whole window: 3000, some left out
            json full = json::array();
            for (int i = 0; i < 3000; ++i) {
                full.push_back(json::parse(
                    WorkingOrder(i + 10, "AAPL", "2026-11-05T14:00:00+0000")));
            }
            return Ok(full.dump());
        }
        // Each half has the SPY order; it is kept once.
        return Ok("[" + WorkingOrder(1, "SPY", "2026-11-05T14:00:00+0000") +
                  "]");
    };
    auto open = Venue("", {"SPY"}).GetOpenOrders();
    ASSERT_OK(open);
    ASSERT_EQ(windows.size(), 3u);
    EXPECT_EQ(windows[1].second, windows[0].second);  // newer half first
    EXPECT_EQ(windows[1].first, windows[2].second);
    EXPECT_EQ(windows[2].first, windows[0].first);
    ASSERT_EQ(open->size(), 1u);
    EXPECT_EQ((*open)[0].order_id, "1");
}

TEST_F(SchwabTradingVenueTest, AnOcoCountsFromItsParentsEntryTime) {
    std::vector<std::string> froms;
    // The parent was entered a day before its working leg says.
    const std::string oco =
        R"([{
      "orderId": 50, "orderStrategyType": "OCO", "status": "WORKING",
      "enteredTime": "2026-11-03T15:00:00+0000",
      "childOrderStrategies": [)" +
        WorkingOrder(51, "SPY", "2026-11-04T15:00:00+0000") + "]}]";
    mHttp->reply = [&](const net::HttpRequest& r) {
        if (r.url.ends_with("/accounts/accountNumbers")) {
            return Ok(kSingleAccount);
        }
        froms.push_back(r.query.at("fromEnteredTime"));
        return Ok(oco);
    };
    auto venue = Venue("", {"SPY"});
    ASSERT_OK(venue.GetOpenOrders());
    mNow += 1min;
    ASSERT_OK(venue.GetOpenOrders());
    EXPECT_EQ(*types::ParseIso8601(froms.at(1)), At("2026-11-03T14:55:00Z"));
}

TEST_F(SchwabTradingVenueTest, ACancelAlreadyPendingCountsAsCancelled) {
    mHttp->reply = [](const net::HttpRequest& r) {
        if (r.url.ends_with("/accounts/accountNumbers")) {
            return Ok(kSingleAccount);
        }
        if (r.method == net::HttpMethod::kDelete) {
            return HttpStatus(400, R"({"message":"order not cancelable"})");
        }
        json order =
            json::parse(WorkingOrder(77, "SPY", "2026-11-05T14:00:00+0000"));
        order["status"] = "PENDING_CANCEL";
        return Ok(order.dump());
    };
    auto outcome = Venue().CancelOrder("77");
    ASSERT_OK(outcome);
    EXPECT_EQ(*outcome, CancelOutcome::kCancelled);
}

TEST_F(SchwabTradingVenueTest, RequestsAreLabelledWithoutTheAccountOrOrderId) {
    mHttp->reply = [](const net::HttpRequest& r) {
        if (r.url.ends_with("/accounts/accountNumbers")) {
            return Ok(kSingleAccount);
        }
        if (r.method == net::HttpMethod::kDelete) {
            return Ok("");
        }
        if (r.url.ends_with("/orders/77")) {
            return Ok(WorkingOrder(77, "SPY", "2026-11-05T14:00:00+0000"));
        }
        return Ok("[]");
    };
    auto venue = Venue();
    ASSERT_OK(venue.GetOrder("77"));
    ASSERT_OK(venue.CancelOrder("77"));
    ASSERT_OK(venue.GetOpenOrders());

    std::set<std::string> labels;
    for (const auto& r : mHttp->requests) {
        EXPECT_EQ(r.Label().find("HASH123"), std::string::npos) << r.Label();
        labels.insert(r.Label());
    }
    EXPECT_EQ(labels, (std::set<std::string>{
                          "https://api.test/trader/v1/accounts/accountNumbers",
                          "https://api.test/trader/v1/accounts/{account}/"
                          "orders",
                          "https://api.test/trader/v1/accounts/{account}/"
                          "orders/{id}"}));
}

}  // namespace
}  // namespace kestrel::trading
