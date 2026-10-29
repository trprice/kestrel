#include "kestrel/trading/binanceus_trading_venue.hpp"

#include <gtest/gtest.h>

#include <chrono>

#include "binanceus_trading_test_support.hpp"
#include "test_helpers.hpp"

namespace kestrel::trading {
namespace {

using kestrel::testing::D;
using kestrel::testing::QueryValue;
using kestrel::testing::RoutedHttp;
using namespace std::chrono_literals;

constexpr std::int64_t kNowMs = 1'700'000'000'000;

// Symbol rules only: ZEC/USDT and BTC/USDT with Binance.US's filters.
class RulesSource : public data::DataSource {
   public:
    std::string_view Name() const override { return "binanceus"; }
    types::Result<std::vector<types::Bar>> GetBars(const types::Symbol&,
                                                   types::Timeframe,
                                                   types::Timestamp,
                                                   types::Timestamp) override {
        return types::MakeError(types::ErrorCategory::kNotImplemented, "n/a");
    }
    types::Result<std::unique_ptr<data::Subscription>> StreamQuotes(
        const std::vector<std::string>&, data::QuoteCallback) override {
        return types::MakeError(types::ErrorCategory::kNotImplemented, "n/a");
    }
    types::Result<types::SymbolInfo> GetSymbolInfo(
        const types::Symbol& symbol) override {
        if (symbol != "ZEC/USDT" && symbol != "BTC/USDT") {
            return types::MakeError(types::ErrorCategory::kNotFound, symbol);
        }
        return types::SymbolInfo{symbol, D("0.01"), D("0.001"), D("0.001"),
                                 D("1")};
    }
};

struct Fixture {
    std::shared_ptr<RoutedHttp> http = std::make_shared<RoutedHttp>();
    std::vector<std::chrono::milliseconds> sleeps;
    types::Timestamp now = types::FromEpochMillis(kNowMs);

    std::unique_ptr<BinanceUSTradingVenue> Venue(
        std::vector<types::Symbol> symbols = {"ZEC/USDT"}) {
        http->Push("/api/v3/time",
                   "{\"serverTime\":" + std::to_string(kNowMs) + "}");
        BinanceUSApi::Options options;
        options.clock = [this] { return now; };
        options.sleeper = [this](std::chrono::milliseconds d) {
            sleeps.push_back(d);
        };
        options.weight_per_second = 100000;
        options.weight_burst = 100000;
        options.orders_per_second = 100000;
        options.order_burst = 100000;
        return std::make_unique<BinanceUSTradingVenue>(
            config::BinanceUSCredentials{"key", "secret"},
            std::make_shared<RulesSource>(), std::move(symbols), http, options);
    }
};

std::string Order(const std::string& symbol, std::int64_t id,
                  const std::string& status, const std::string& type,
                  const std::string& side, const std::string& price,
                  const std::string& qty, const std::string& executed,
                  const std::string& quote_executed,
                  const std::string& stop = "0.00000000") {
    return "{\"symbol\":\"" + symbol + "\",\"orderId\":" + std::to_string(id) +
           ",\"clientOrderId\":\"cid" + std::to_string(id) + "\",\"price\":\"" +
           price + "\",\"origQty\":\"" + qty + "\",\"executedQty\":\"" +
           executed + "\",\"cummulativeQuoteQty\":\"" + quote_executed +
           "\",\"status\":\"" + status +
           "\",\"timeInForce\":\"GTC\",\"type\":\"" + type + "\",\"side\":\"" +
           side + "\",\"stopPrice\":\"" + stop +
           "\",\"time\":1699999990000,\"updateTime\":1699999995000}";
}

std::string Trade(const std::string& id, const std::string& order_id,
                  std::int64_t time_ms, bool buyer, bool maker,
                  const std::string& price = "1300.00",
                  const std::string& qty = "0.01",
                  const std::string& fee = "0.00000000",
                  const std::string& fee_asset = "USDT") {
    return "{\"symbol\":\"ZECUSDT\",\"id\":" + id + ",\"orderId\":" + order_id +
           ",\"price\":\"" + price + "\",\"qty\":\"" + qty +
           "\",\"quoteQty\":\"13.00\",\"commission\":\"" + fee +
           "\",\"commissionAsset\":\"" + fee_asset +
           "\",\"time\":" + std::to_string(time_ms) +
           ",\"isBuyer\":" + (buyer ? "true" : "false") +
           ",\"isMaker\":" + (maker ? "true" : "false") +
           ",\"isBestMatch\":true}";
}

TEST(BinanceUSTradingVenue, ReportsBalancesAndDropsEmptyOnes) {
    Fixture f;
    auto venue = f.Venue();
    f.http->Push(
        "/api/v3/account",
        "{\"canTrade\":true,\"balances\":["
        "{\"asset\":\"USDT\",\"free\":\"50.10\",\"locked\":\"12.00\"},"
        "{\"asset\":\"ZEC\",\"free\":\"0.04100000\",\"locked\":\"0.00000000\"},"
        "{\"asset\":\"BTC\",\"free\":\"0.00000000\",\"locked\":\"0.00000000\"}]"
        "}");

    auto balances = venue->GetBalances();
    ASSERT_OK(balances);
    ASSERT_EQ(balances->size(), 2u);
    EXPECT_EQ((*balances)[0].asset, "USDT");
    EXPECT_EQ((*balances)[0].total, D("62.10"));
    EXPECT_EQ((*balances)[0].available, D("50.10"));
    EXPECT_EQ((*balances)[0].locked, D("12.00"));
    EXPECT_EQ((*balances)[1].asset, "ZEC");
    EXPECT_EQ((*balances)[1].total, D("0.041"));

    const auto sent = f.http->To("/api/v3/account");
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0].headers.at("X-MBX-APIKEY"), "key");
}

TEST(BinanceUSTradingVenue, RejectsAMalformedAccountResponse) {
    Fixture f;
    auto venue = f.Venue();
    f.http->Push("/api/v3/account", "{\"nothing\":1}");
    auto balances = venue->GetBalances();
    ASSERT_FALSE(balances.has_value());
    EXPECT_EQ(balances.error().category, types::ErrorCategory::kParse);
}

TEST(BinanceUSTradingVenue, ListsOpenOrdersForEachConfiguredSymbol) {
    Fixture f;
    auto venue = f.Venue({"ZEC/USDT", "BTC/USDT"});
    f.http->Push("/api/v3/openOrders",
                 "[" +
                     Order("ZECUSDT", 11, "NEW", "LIMIT", "BUY", "1290.00",
                           "0.01", "0.00000000", "0.00000000") +
                     "," +
                     Order("ZECUSDT", 12, "PARTIALLY_FILLED", "LIMIT", "SELL",
                           "1310.00", "0.02", "0.01", "13.10") +
                     "]");
    f.http->Push("/api/v3/openOrders", "[]");

    auto orders = venue->GetOpenOrders();
    ASSERT_OK(orders);
    ASSERT_EQ(orders->size(), 2u);

    const auto& buy = (*orders)[0];
    EXPECT_EQ(buy.order_id, "11");
    EXPECT_EQ(buy.client_order_id, "cid11");
    EXPECT_EQ(buy.symbol, "ZEC/USDT");
    EXPECT_EQ(buy.side, types::Side::kBuy);
    EXPECT_EQ(buy.type, types::OrderType::kLimit);
    EXPECT_EQ(buy.state, types::OrderState::kNew);
    EXPECT_EQ(buy.quantity, D("0.01"));
    EXPECT_EQ(buy.limit_price, D("1290"));
    EXPECT_FALSE(buy.average_fill_price.has_value());
    EXPECT_FALSE(buy.stop_price.has_value());
    EXPECT_EQ(buy.created_time, types::FromEpochMillis(1699999990000));
    EXPECT_EQ(buy.updated_time, types::FromEpochMillis(1699999995000));

    const auto& sell = (*orders)[1];
    EXPECT_EQ(sell.state, types::OrderState::kPartiallyFilled);
    EXPECT_EQ(sell.filled_quantity, D("0.01"));
    EXPECT_EQ(sell.average_fill_price, D("1310"));

    const auto sent = f.http->To("/api/v3/openOrders");
    ASSERT_EQ(sent.size(), 2u);
    EXPECT_EQ(QueryValue(sent[0].raw_query, "symbol"), "ZECUSDT");
    EXPECT_EQ(QueryValue(sent[1].raw_query, "symbol"), "BTCUSDT");
}

TEST(BinanceUSTradingVenue, MapsOrderTypesAndStates) {
    Fixture f;
    auto venue = f.Venue();
    f.http->Push("/api/v3/openOrders",
                 "[" +
                     Order("ZECUSDT", 1, "NEW", "STOP_LOSS", "SELL",
                           "0.00000000", "0.01", "0.0", "0.0", "1200.00") +
                     "," +
                     Order("ZECUSDT", 2, "NEW", "STOP_LOSS_LIMIT", "SELL",
                           "1195.00", "0.01", "0.0", "0.0", "1200.00") +
                     "," +
                     Order("ZECUSDT", 3, "NEW", "LIMIT_MAKER", "BUY", "1200.00",
                           "0.01", "0.0", "0.0") +
                     "," +
                     Order("ZECUSDT", 4, "NEW", "MARKET", "BUY", "0.00000000",
                           "0.01", "0.0", "0.0") +
                     "," +
                     Order("ZECUSDT", 5, "PENDING_CANCEL", "LIMIT", "BUY",
                           "1200.00", "0.02", "0.01", "12.00") +
                     "]");

    auto orders = venue->GetOpenOrders();
    ASSERT_OK(orders);
    ASSERT_EQ(orders->size(), 5u);
    EXPECT_EQ((*orders)[0].type, types::OrderType::kStop);
    EXPECT_EQ((*orders)[0].stop_price, D("1200"));
    EXPECT_FALSE((*orders)[0].limit_price.has_value());
    EXPECT_EQ((*orders)[1].type, types::OrderType::kStop);
    EXPECT_EQ((*orders)[1].limit_price, D("1195"));
    EXPECT_EQ((*orders)[2].type, types::OrderType::kLimit);
    EXPECT_EQ((*orders)[3].type, types::OrderType::kMarket);
    EXPECT_FALSE((*orders)[3].limit_price.has_value());
    // Still working, and judged by what it has filled.
    EXPECT_EQ((*orders)[4].state, types::OrderState::kPartiallyFilled);
}

TEST(BinanceUSTradingVenue, ReadsClosedStatesOfASingleOrder) {
    Fixture f;
    auto venue = f.Venue();
    const std::pair<const char*, types::OrderState> cases[] = {
        {"FILLED", types::OrderState::kFilled},
        {"CANCELED", types::OrderState::kCancelled},
        {"REJECTED", types::OrderState::kRejected},
        {"EXPIRED", types::OrderState::kExpired},
        {"EXPIRED_IN_MATCH", types::OrderState::kExpired}};
    for (const auto& [status, state] : cases) {
        f.http->Push("/api/v3/order",
                     Order("ZECUSDT", 21, status, "LIMIT", "BUY", "1290.00",
                           "0.01", "0.01", "12.90"));
        auto order = venue->GetOrder("21");
        ASSERT_OK(order);
        EXPECT_EQ(order->state, state) << status;
    }
}

TEST(BinanceUSTradingVenue, FindsAnUnknownOrderOnTheSymbolThatHasIt) {
    Fixture f;
    auto venue = f.Venue({"BTC/USDT", "ZEC/USDT"});
    f.http->PushStatus("/api/v3/order", 400,
                       "{\"code\":-2013,\"msg\":\"Order does not exist.\"}");
    f.http->Push("/api/v3/order", Order("ZECUSDT", 31, "NEW", "LIMIT", "BUY",
                                        "1290.00", "0.01", "0.0", "0.0"));

    auto order = venue->GetOrder("31");
    ASSERT_OK(order);
    EXPECT_EQ(order->symbol, "ZEC/USDT");
    auto sent = f.http->To("/api/v3/order");
    ASSERT_EQ(sent.size(), 2u);
    EXPECT_EQ(QueryValue(sent[0].raw_query, "symbol"), "BTCUSDT");
    EXPECT_EQ(QueryValue(sent[1].raw_query, "symbol"), "ZECUSDT");
    EXPECT_EQ(QueryValue(sent[1].raw_query, "orderId"), "31");

    // Remembered: the next lookup asks only the right symbol.
    f.http->Push("/api/v3/order", Order("ZECUSDT", 31, "FILLED", "LIMIT", "BUY",
                                        "1290.00", "0.01", "0.01", "12.90"));
    ASSERT_OK(venue->GetOrder("31"));
    sent = f.http->To("/api/v3/order");
    ASSERT_EQ(sent.size(), 3u);
    EXPECT_EQ(QueryValue(sent[2].raw_query, "symbol"), "ZECUSDT");
}

TEST(BinanceUSTradingVenue, ReportsAnOrderOnNoSymbolAsNotFound) {
    Fixture f;
    auto venue = f.Venue({"BTC/USDT", "ZEC/USDT"});
    for (int i = 0; i < 2; ++i) {
        f.http->PushStatus(
            "/api/v3/order", 400,
            "{\"code\":-2013,\"msg\":\"Order does not exist.\"}");
    }
    auto order = venue->GetOrder("99");
    ASSERT_FALSE(order.has_value());
    EXPECT_EQ(order.error().category, types::ErrorCategory::kNotFound);
}

TEST(BinanceUSTradingVenue, ASymbolErrorOtherThanNotFoundStopsTheLookup) {
    Fixture f;
    auto venue = f.Venue({"BTC/USDT", "ZEC/USDT"});
    f.http->PushStatus("/api/v3/order", 401,
                       "{\"code\":-2015,\"msg\":\"Invalid API-key\"}");
    auto order = venue->GetOrder("99");
    ASSERT_FALSE(order.has_value());
    EXPECT_EQ(order.error().category, types::ErrorCategory::kAuth);
    EXPECT_EQ(f.http->To("/api/v3/order").size(), 1u);
}

TEST(BinanceUSTradingVenue, ReadsFills) {
    Fixture f;
    auto venue = f.Venue();
    const auto since = f.now - 1h;
    f.http->Push("/api/v3/myTrades",
                 "[" + Trade("500", "11", kNowMs - 1800000, true, true) + "," +
                     Trade("501", "12", kNowMs - 600000, false, false,
                           "1310.55", "0.02", "0.00052422", "USDT") +
                     "]");

    auto fills = venue->GetFills(since);
    ASSERT_OK(fills);
    ASSERT_EQ(fills->size(), 2u);

    const auto& a = (*fills)[0];
    EXPECT_EQ(a.fill_id, "ZECUSDT:500");
    EXPECT_EQ(a.order_id, "11");
    EXPECT_EQ(a.symbol, "ZEC/USDT");
    EXPECT_EQ(a.side, types::Side::kBuy);
    EXPECT_EQ(a.liquidity, types::Liquidity::kMaker);
    EXPECT_EQ(a.quantity, D("0.01"));
    EXPECT_EQ(a.price, D("1300"));
    EXPECT_TRUE(a.fee.IsZero());

    const auto& b = (*fills)[1];
    EXPECT_EQ(b.side, types::Side::kSell);
    EXPECT_EQ(b.liquidity, types::Liquidity::kTaker);
    EXPECT_EQ(b.price, D("1310.55"));
    EXPECT_EQ(b.fee, D("0.00052422"));
    EXPECT_EQ(b.fee_asset, "USDT");
    EXPECT_EQ(b.time, types::FromEpochMillis(kNowMs - 600000));

    const auto sent = f.http->To("/api/v3/myTrades");
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(QueryValue(sent[0].raw_query, "symbol"), "ZECUSDT");
    EXPECT_EQ(QueryValue(sent[0].raw_query, "startTime"),
              std::to_string(types::ToEpochMillis(since)));
    EXPECT_EQ(QueryValue(sent[0].raw_query, "endTime"), std::to_string(kNowMs));
    EXPECT_EQ(QueryValue(sent[0].raw_query, "limit"), "1000");
}

TEST(BinanceUSTradingVenue, AsksInWindowsOfAtMostADay) {
    Fixture f;
    auto venue = f.Venue();
    for (int i = 0; i < 3; ++i) {
        f.http->Push("/api/v3/myTrades", "[]");
    }
    ASSERT_OK(venue->GetFills(f.now - 50h));

    const auto sent = f.http->To("/api/v3/myTrades");
    ASSERT_EQ(sent.size(), 3u);
    const auto start = [&](std::size_t i) {
        return std::stoll(QueryValue(sent[i].raw_query, "startTime"));
    };
    const auto end = [&](std::size_t i) {
        return std::stoll(QueryValue(sent[i].raw_query, "endTime"));
    };
    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_LE(end(i) - start(i), 24 * 3'600'000 - 1);
    }
    EXPECT_EQ(start(0), kNowMs - 50 * 3'600'000);
    EXPECT_EQ(start(1), end(0) + 1);  // no gap, no overlap
    EXPECT_EQ(start(2), end(1) + 1);
    EXPECT_EQ(end(2), kNowMs);
}

TEST(BinanceUSTradingVenue, PagesThroughAFullWindowWithoutDuplicates) {
    Fixture f;
    auto venue = f.Venue();
    std::string first = "[";
    for (int i = 1; i <= 1000; ++i) {
        first +=
            (i > 1 ? "," : "") + Trade(std::to_string(i), "7",
                                       kNowMs - 3'000'000 + i * 10, true, true);
    }
    first += "]";
    f.http->Push("/api/v3/myTrades", first);
    // The next page starts at the last trade's time, so that trade comes
    // again, followed by new ones.
    f.http->Push(
        "/api/v3/myTrades",
        "[" + Trade("1000", "7", kNowMs - 3'000'000 + 10'000, true, true) +
            "," + Trade("1001", "8", kNowMs - 2'000'000, false, true) + "]");

    auto fills = venue->GetFills(f.now - 1h);
    ASSERT_OK(fills);
    EXPECT_EQ(fills->size(), 1001u);
    const auto sent = f.http->To("/api/v3/myTrades");
    ASSERT_EQ(sent.size(), 2u);
    EXPECT_EQ(QueryValue(sent[1].raw_query, "startTime"),
              std::to_string(kNowMs - 3'000'000 + 10'000));
    EXPECT_EQ(fills->back().fill_id, "ZECUSDT:1001");
}

TEST(BinanceUSTradingVenue, SortsFillsFromSeveralSymbolsByTime) {
    Fixture f;
    auto venue = f.Venue({"ZEC/USDT", "BTC/USDT"});
    f.http->Push("/api/v3/myTrades",
                 "[" + Trade("1", "1", kNowMs - 100, true, true) + "]");
    f.http->Push(
        "/api/v3/myTrades",
        "[" +
            std::string("{\"symbol\":\"BTCUSDT\",\"id\":9,\"orderId\":2,"
                        "\"price\":\"60000.00\",\"qty\":\"0.001\","
                        "\"quoteQty\":\"60\",\"commission\":\"0\","
                        "\"commissionAsset\":\"USDT\",\"time\":") +
            std::to_string(kNowMs - 500) +
            ",\"isBuyer\":true,\"isMaker\":false,\"isBestMatch\":true}]");

    auto fills = venue->GetFills(f.now - 1h);
    ASSERT_OK(fills);
    ASSERT_EQ(fills->size(), 2u);
    EXPECT_EQ((*fills)[0].fill_id, "BTCUSDT:9");  // older, though asked second
    EXPECT_EQ((*fills)[1].fill_id, "ZECUSDT:1");
}

TEST(BinanceUSTradingVenue, FailsTheCallOnAMalformedTrade) {
    Fixture f;
    auto venue = f.Venue();
    f.http->Push("/api/v3/myTrades", "[{\"id\":1}]");
    auto fills = venue->GetFills(f.now - 1h);
    ASSERT_FALSE(fills.has_value());
    EXPECT_EQ(fills.error().category, types::ErrorCategory::kParse);
}

TEST(BinanceUSTradingVenue, ReportsNoPositions) {
    Fixture f;
    auto venue = f.Venue();
    auto positions = venue->GetPositions();
    ASSERT_OK(positions);
    EXPECT_TRUE(positions->empty());
}

types::OrderRequest Limit(types::Side side, const char* qty,
                          const char* price) {
    types::OrderRequest r;
    r.symbol = "ZEC/USDT";
    r.side = side;
    r.type = types::OrderType::kLimit;
    r.quantity = D(qty);
    r.limit_price = D(price);
    return r;
}

// A newOrderRespType=RESULT answer.
std::string Placed(std::int64_t id, const std::string& client_id,
                   const std::string& status, const std::string& executed = "0",
                   const std::string& quote = "0") {
    return "{\"symbol\":\"ZECUSDT\",\"orderId\":" + std::to_string(id) +
           ",\"clientOrderId\":\"" + client_id +
           "\",\"transactTime\":1700000000123,\"price\":\"700.00\","
           "\"origQty\":\"0.010\",\"executedQty\":\"" +
           executed + "\",\"cummulativeQuoteQty\":\"" + quote +
           "\",\"status\":\"" + status +
           "\",\"timeInForce\":\"GTC\",\"type\":\"LIMIT\",\"side\":\"BUY\"}";
}

TEST(BinanceUSOrders, PlacesALimitOrderWithTheSymbolRulesApplied) {
    Fixture f;
    auto venue = f.Venue();
    f.http->Push("/api/v3/order", Placed(42, "x", "NEW"));
    auto ack = venue->PlaceOrder(Limit(types::Side::kBuy, "0.0105", "700.009"));
    ASSERT_OK(ack);
    EXPECT_EQ(ack->order_id, "42");
    EXPECT_EQ(ack->state, types::OrderState::kNew);
    EXPECT_EQ(ack->time, types::FromEpochMillis(1700000000123));

    const auto sent = f.http->To("/api/v3/order");
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0].method, net::HttpMethod::kPost);
    const auto& q = sent[0].raw_query;
    EXPECT_EQ(QueryValue(q, "symbol"), "ZECUSDT");
    EXPECT_EQ(QueryValue(q, "side"), "BUY");
    EXPECT_EQ(QueryValue(q, "type"), "LIMIT");
    EXPECT_EQ(QueryValue(q, "timeInForce"), "GTC");
    EXPECT_EQ(QueryValue(q, "quantity"), "0.010");  // down to the lot
    EXPECT_EQ(QueryValue(q, "price"), "700.00");    // a buy rounds down
    EXPECT_EQ(QueryValue(q, "newOrderRespType"), "RESULT");
    EXPECT_EQ(QueryValue(q, "newClientOrderId").rfind("kst-", 0), 0u);
    EXPECT_FALSE(QueryValue(q, "signature").empty());

    // The order's symbol is remembered, so a lookup asks only for it.
    f.http->Push("/api/v3/order", Order("ZECUSDT", 42, "NEW", "LIMIT", "BUY",
                                        "700.00", "0.010", "0", "0"));
    ASSERT_OK(venue->GetOrder("42"));
}

TEST(BinanceUSOrders, GeneratedClientIdsDifferAndACallersIdIsKept) {
    Fixture f;
    auto venue = f.Venue();
    for (int i = 0; i < 3; ++i) {
        f.http->Push("/api/v3/order", Placed(i, "x", "NEW"));
    }
    ASSERT_OK(venue->PlaceOrder(Limit(types::Side::kBuy, "0.01", "700")));
    ASSERT_OK(venue->PlaceOrder(Limit(types::Side::kBuy, "0.01", "700")));
    auto mine = Limit(types::Side::kBuy, "0.01", "700");
    mine.client_order_id = "my-own_1";
    ASSERT_OK(venue->PlaceOrder(mine));
    const auto sent = f.http->To("/api/v3/order");
    ASSERT_EQ(sent.size(), 3u);
    const auto first = QueryValue(sent[0].raw_query, "newClientOrderId");
    const auto second = QueryValue(sent[1].raw_query, "newClientOrderId");
    EXPECT_NE(first, second);
    EXPECT_LE(first.size(), 36u);
    EXPECT_EQ(QueryValue(sent[2].raw_query, "newClientOrderId"), "my-own_1");

    mine.client_order_id = "not allowed!";
    auto bad = venue->PlaceOrder(mine);
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error().category, types::ErrorCategory::kInvalidArgument);
    EXPECT_EQ(f.http->To("/api/v3/order").size(), 3u);  // not sent
}

TEST(BinanceUSOrders, MapsMarketStopAndTimeInForce) {
    Fixture f;
    auto venue = f.Venue();
    for (int i = 0; i < 4; ++i) {
        f.http->Push("/api/v3/order", Placed(i, "x", "NEW"));
    }
    types::OrderRequest market;
    market.symbol = "ZEC/USDT";
    market.side = types::Side::kSell;
    market.type = types::OrderType::kMarket;
    market.quantity = D("0.01");
    ASSERT_OK(venue->PlaceOrder(market));

    types::OrderRequest stop = market;
    stop.type = types::OrderType::kStop;
    stop.stop_price = D("1200.004");
    ASSERT_OK(venue->PlaceOrder(stop));

    auto ioc = Limit(types::Side::kSell, "0.01", "1300.001");
    ioc.time_in_force = types::TimeInForce::kIoc;
    ASSERT_OK(venue->PlaceOrder(ioc));
    auto fok = ioc;
    fok.time_in_force = types::TimeInForce::kFok;
    ASSERT_OK(venue->PlaceOrder(fok));

    const auto sent = f.http->To("/api/v3/order");
    ASSERT_EQ(sent.size(), 4u);
    EXPECT_EQ(QueryValue(sent[0].raw_query, "type"), "MARKET");
    EXPECT_EQ(QueryValue(sent[0].raw_query, "side"), "SELL");
    EXPECT_EQ(QueryValue(sent[0].raw_query, "timeInForce"), "");
    EXPECT_EQ(QueryValue(sent[0].raw_query, "price"), "");
    EXPECT_EQ(QueryValue(sent[1].raw_query, "type"), "STOP_LOSS");
    EXPECT_EQ(QueryValue(sent[1].raw_query, "stopPrice"), "1200.00");
    EXPECT_EQ(QueryValue(sent[1].raw_query, "timeInForce"), "");
    EXPECT_EQ(QueryValue(sent[2].raw_query, "timeInForce"), "IOC");
    EXPECT_EQ(QueryValue(sent[2].raw_query, "price"), "1300.01");  // sell: up
    EXPECT_EQ(QueryValue(sent[3].raw_query, "timeInForce"), "FOK");
}

TEST(BinanceUSOrders, RefusesWhatBinanceCannotTakeWithoutSendingIt) {
    Fixture f;
    auto venue = f.Venue();
    auto day = Limit(types::Side::kBuy, "0.01", "700");
    day.time_in_force = types::TimeInForce::kDay;
    EXPECT_EQ(venue->PlaceOrder(day).error().category,
              types::ErrorCategory::kUnsupported);

    types::OrderRequest stop;
    stop.symbol = "ZEC/USDT";
    stop.type = types::OrderType::kStop;
    stop.quantity = D("0.01");
    stop.stop_price = D("1200");
    stop.time_in_force = types::TimeInForce::kIoc;
    EXPECT_EQ(venue->PlaceOrder(stop).error().category,
              types::ErrorCategory::kUnsupported);

    // 0.001 at 700 is 0.70 USDT, under the 1 USDT minimum.
    auto small = venue->PlaceOrder(Limit(types::Side::kBuy, "0.001", "700"));
    ASSERT_FALSE(small.has_value());
    EXPECT_EQ(small.error().category, types::ErrorCategory::kRejected);
    EXPECT_NE(small.error().message.find("minimum"), std::string::npos);

    auto unknown = Limit(types::Side::kBuy, "1", "700");
    unknown.symbol = "DOGE/USDT";
    EXPECT_EQ(venue->PlaceOrder(unknown).error().category,
              types::ErrorCategory::kNotFound);

    EXPECT_TRUE(f.http->To("/api/v3/order").empty());
}

TEST(BinanceUSOrders, ARefusedOrderIsReportedAndNotLookedUp) {
    Fixture f;
    auto venue = f.Venue();
    f.http->PushStatus(
        "/api/v3/order", 400,
        "{\"code\":-2010,\"msg\":\"Account has insufficient balance for "
        "requested action.\"}");
    auto placed = venue->PlaceOrder(Limit(types::Side::kBuy, "0.01", "700"));
    ASSERT_FALSE(placed.has_value());
    EXPECT_EQ(placed.error().category,
              types::ErrorCategory::kInsufficientFunds);
    EXPECT_EQ(f.http->To("/api/v3/order").size(), 1u);  // no lookup
}

TEST(BinanceUSOrders, AnUnansweredOrderIsLookedUpNotSentAgain) {
    Fixture f;
    auto venue = f.Venue();
    f.http->PushError(
        "/api/v3/order",
        types::Error{types::ErrorCategory::kTimeout, "timed out"});
    f.http->Push("/api/v3/order", Order("ZECUSDT", 77, "NEW", "LIMIT", "BUY",
                                        "700.00", "0.010", "0", "0"));
    auto ack = venue->PlaceOrder(Limit(types::Side::kBuy, "0.01", "700"));
    ASSERT_OK(ack);
    EXPECT_EQ(ack->order_id, "77");
    EXPECT_EQ(ack->state, types::OrderState::kNew);

    const auto sent = f.http->To("/api/v3/order");
    ASSERT_EQ(sent.size(), 2u);
    EXPECT_EQ(sent[0].method, net::HttpMethod::kPost);
    EXPECT_EQ(sent[1].method, net::HttpMethod::kGet);
    EXPECT_EQ(QueryValue(sent[1].raw_query, "origClientOrderId"),
              QueryValue(sent[0].raw_query, "newClientOrderId"));
    EXPECT_TRUE(f.sleeps.empty());  // found at the first look
}

TEST(BinanceUSOrders, AnUnansweredOrderThatCannotBeFoundIsAnError) {
    Fixture f;
    auto venue = f.Venue();
    f.http->PushError("/api/v3/order",
                      types::Error{types::ErrorCategory::kServer, "HTTP 503"});
    for (int i = 0; i < 2; ++i) {
        f.http->PushStatus(
            "/api/v3/order", 400,
            "{\"code\":-2013,\"msg\":\"Order does not exist.\"}");
    }
    auto placed = venue->PlaceOrder(Limit(types::Side::kBuy, "0.01", "700"));
    ASSERT_FALSE(placed.has_value());
    EXPECT_EQ(placed.error().category, types::ErrorCategory::kServer);
    const auto sent = f.http->To("/api/v3/order");
    ASSERT_EQ(sent.size(), 3u);  // one POST, two lookups
    EXPECT_EQ(sent[0].method, net::HttpMethod::kPost);
    const auto client_id = QueryValue(sent[0].raw_query, "newClientOrderId");
    EXPECT_NE(placed.error().message.find(client_id), std::string::npos);
    EXPECT_EQ(f.sleeps.size(), 1u);  // a pause before the second look
}

TEST(BinanceUSOrders, TestOrderGoesOnlyToTheTestEndpoint) {
    Fixture f;
    auto venue = f.Venue();
    f.http->Push("/api/v3/order/test", "{}");
    ASSERT_OK(venue->TestOrder(Limit(types::Side::kBuy, "0.002", "700")));
    const auto sent = f.http->To("/api/v3/order/test");
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0].method, net::HttpMethod::kPost);
    EXPECT_EQ(QueryValue(sent[0].raw_query, "price"), "700.00");
    EXPECT_EQ(f.http->To("/api/v3/order").size(), 0u);

    f.http->PushStatus("/api/v3/order/test", 400,
                       "{\"code\":-1013,\"msg\":\"Filter failure: "
                       "PERCENT_PRICE_BY_SIDE\"}");
    auto refused = venue->TestOrder(Limit(types::Side::kBuy, "0.002", "700"));
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().category, types::ErrorCategory::kRejected);
}

TEST(BinanceUSOrders, CancelsARememberedOrderOnItsSymbol) {
    Fixture f;
    auto venue = f.Venue();
    f.http->Push("/api/v3/order", Placed(42, "x", "NEW"));
    ASSERT_OK(venue->PlaceOrder(Limit(types::Side::kBuy, "0.01", "700")));
    f.http->Push("/api/v3/order", Order("ZECUSDT", 42, "CANCELED", "LIMIT",
                                        "BUY", "700.00", "0.010", "0", "0"));
    auto outcome = venue->CancelOrder("42");
    ASSERT_OK(outcome);
    EXPECT_EQ(*outcome, CancelOutcome::kCancelled);
    const auto sent = f.http->To("/api/v3/order");
    ASSERT_EQ(sent.size(), 2u);
    EXPECT_EQ(sent[1].method, net::HttpMethod::kDelete);
    EXPECT_EQ(QueryValue(sent[1].raw_query, "symbol"), "ZECUSDT");
    EXPECT_EQ(QueryValue(sent[1].raw_query, "orderId"), "42");
}

TEST(BinanceUSOrders, ACancelThatComesTooLateSaysWhatHappened) {
    const std::pair<const char*, CancelOutcome> cases[] = {
        {"FILLED", CancelOutcome::kAlreadyFilled},
        {"CANCELED", CancelOutcome::kAlreadyClosed},
        {"EXPIRED", CancelOutcome::kAlreadyClosed}};
    for (const auto& [status, expected] : cases) {
        Fixture f;
        auto venue = f.Venue();
        f.http->Push("/api/v3/order", Placed(42, "x", "NEW"));
        ASSERT_OK(venue->PlaceOrder(Limit(types::Side::kBuy, "0.01", "700")));
        f.http->PushStatus("/api/v3/order", 400,
                           "{\"code\":-2011,\"msg\":\"Unknown order sent.\"}");
        f.http->Push("/api/v3/order",
                     Order("ZECUSDT", 42, status, "LIMIT", "BUY", "700.00",
                           "0.010", "0.010", "7.00"));
        auto outcome = venue->CancelOrder("42");
        ASSERT_OK(outcome);
        EXPECT_EQ(*outcome, expected) << status;
    }
}

TEST(BinanceUSOrders, CancellingAnOrderNobodyKnowsIsNotFound) {
    Fixture f;
    auto venue = f.Venue();
    // Not remembered: the symbol is looked for first, and it is on none.
    f.http->PushStatus("/api/v3/order", 400,
                       "{\"code\":-2013,\"msg\":\"Order does not exist.\"}");
    auto outcome = venue->CancelOrder("999");
    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().category, types::ErrorCategory::kNotFound);
    for (const auto& r : f.http->To("/api/v3/order")) {
        EXPECT_EQ(r.method, net::HttpMethod::kGet);  // no DELETE was sent
    }
}

TEST(BinanceUSOrders, CancelsAnOrderPlacedBeforeThisProcessStarted) {
    Fixture f;
    auto venue = f.Venue({"BTC/USDT", "ZEC/USDT"});
    f.http->PushStatus("/api/v3/order", 400,
                       "{\"code\":-2013,\"msg\":\"Order does not exist.\"}");
    f.http->Push("/api/v3/order", Order("ZECUSDT", 5, "NEW", "LIMIT", "BUY",
                                        "700.00", "0.010", "0", "0"));
    f.http->Push("/api/v3/order", Order("ZECUSDT", 5, "CANCELED", "LIMIT",
                                        "BUY", "700.00", "0.010", "0", "0"));
    auto outcome = venue->CancelOrder("5");
    ASSERT_OK(outcome);
    EXPECT_EQ(*outcome, CancelOutcome::kCancelled);
    const auto sent = f.http->To("/api/v3/order");
    ASSERT_EQ(sent.size(), 3u);
    EXPECT_EQ(sent[2].method, net::HttpMethod::kDelete);
    EXPECT_EQ(QueryValue(sent[2].raw_query, "symbol"), "ZECUSDT");
}

TEST(BinanceUSOrders, CancelAllCoversEverySymbol) {
    Fixture f;
    auto venue = f.Venue({"BTC/USDT", "ZEC/USDT"});
    // BTC/USDT has nothing open: Binance answers -2011.
    f.http->PushStatus("/api/v3/openOrders", 400,
                       "{\"code\":-2011,\"msg\":\"Unknown order sent.\"}");
    f.http->Push("/api/v3/openOrders",
                 "[" +
                     Order("ZECUSDT", 1, "CANCELED", "LIMIT", "BUY", "700.00",
                           "0.010", "0", "0") +
                     "," +
                     Order("ZECUSDT", 2, "CANCELED", "LIMIT", "SELL", "1500.00",
                           "0.010", "0", "0") +
                     "]");
    auto cancelled = venue->CancelAll();
    ASSERT_OK(cancelled);
    EXPECT_EQ(*cancelled, (std::vector<std::string>{"1", "2"}));
    const auto sent = f.http->To("/api/v3/openOrders");
    ASSERT_EQ(sent.size(), 2u);
    EXPECT_EQ(sent[0].method, net::HttpMethod::kDelete);
    EXPECT_EQ(QueryValue(sent[0].raw_query, "symbol"), "BTCUSDT");
}

TEST(BinanceUSOrders, AFailedLookupAfterAnUnansweredOrderSaysSo) {
    Fixture f;
    auto venue = f.Venue();
    f.http->PushError(
        "/api/v3/order",
        types::Error{types::ErrorCategory::kTimeout, "timed out"});
    // The lookup is a read, so it is retried; every attempt fails.
    for (int i = 0; i < 5; ++i) {
        f.http->PushError(
            "/api/v3/order",
            types::Error{types::ErrorCategory::kNetwork, "reset"});
    }
    auto placed = venue->PlaceOrder(Limit(types::Side::kBuy, "0.01", "700"));
    ASSERT_FALSE(placed.has_value());
    EXPECT_EQ(placed.error().category, types::ErrorCategory::kTimeout);
    EXPECT_NE(placed.error().message.find("could not be looked up"),
              std::string::npos);
    EXPECT_EQ(placed.error().message.find("was not found"), std::string::npos);
    std::size_t posts = 0;
    for (const auto& r : f.http->To("/api/v3/order")) {
        posts += r.method == net::HttpMethod::kPost;
    }
    EXPECT_EQ(posts, 1u);  // never sent again
}

TEST(BinanceUSOrders, AnUnansweredCancelOfAnOrderStillOpenIsAnError) {
    Fixture f;
    auto venue = f.Venue();
    f.http->Push("/api/v3/order", Placed(42, "x", "NEW"));
    ASSERT_OK(venue->PlaceOrder(Limit(types::Side::kBuy, "0.01", "700")));
    f.http->PushError(
        "/api/v3/order",
        types::Error{types::ErrorCategory::kTimeout, "timed out"});
    f.http->Push("/api/v3/order", Order("ZECUSDT", 42, "NEW", "LIMIT", "BUY",
                                        "700.00", "0.010", "0", "0"));
    auto outcome = venue->CancelOrder("42");
    ASSERT_FALSE(outcome.has_value());  // still working: the caller retries
    EXPECT_EQ(outcome.error().category, types::ErrorCategory::kTimeout);
}

TEST(BinanceUSOrders, CancelAllCarriesOnPastAFailingSymbol) {
    Fixture f;
    auto venue = f.Venue({"BTC/USDT", "ZEC/USDT"});
    f.http->PushStatus("/api/v3/openOrders", 503, "busy");
    f.http->Push("/api/v3/openOrders",
                 "[" +
                     Order("ZECUSDT", 1, "CANCELED", "LIMIT", "BUY", "700.00",
                           "0.010", "0", "0") +
                     "," +
                     Order("ZECUSDT", 2, "CANCELED", "LIMIT", "SELL", "1500.00",
                           "0.010", "0", "0") +
                     "]");
    auto cancelled = venue->CancelAll();
    ASSERT_FALSE(cancelled.has_value());
    EXPECT_EQ(cancelled.error().category, types::ErrorCategory::kServer);
    const auto& message = cancelled.error().message;
    EXPECT_NE(message.find("BTC/USDT"), std::string::npos);
    EXPECT_NE(message.find("cancelled 2 order(s)"), std::string::npos);
    EXPECT_NE(message.find(" 1 2"), std::string::npos);
    EXPECT_EQ(f.http->To("/api/v3/openOrders").size(), 2u);  // ZEC still tried
}

TEST(BinanceUSOrders, ABalanceRefusalInOtherWordsIsStillInsufficientFunds) {
    Fixture f;
    auto venue = f.Venue();
    f.http->PushStatus("/api/v3/order", 400,
                       "{\"code\":-2010,\"msg\":\"Insufficient Balance.\"}");
    f.http->PushStatus(
        "/api/v3/order", 400,
        "{\"code\":-2010,\"msg\":\"Order would immediately match and take.\"}");
    EXPECT_EQ(venue->PlaceOrder(Limit(types::Side::kBuy, "0.01", "700"))
                  .error()
                  .category,
              types::ErrorCategory::kInsufficientFunds);
    EXPECT_EQ(venue->PlaceOrder(Limit(types::Side::kBuy, "0.01", "700"))
                  .error()
                  .category,
              types::ErrorCategory::kRejected);
}

TEST(BinanceUSOrders, NewOrdersAreHeldToTheOrderRate) {
    Fixture f;
    auto venue = [&] {
        f.http->Push("/api/v3/time",
                     "{\"serverTime\":" + std::to_string(kNowMs) + "}");
        BinanceUSApi::Options options;
        options.clock = [&f] { return f.now; };
        options.sleeper = [](std::chrono::milliseconds) {};
        options.weight_per_second = 100000;
        options.weight_burst = 100000;
        options.orders_per_second = 20;  // one every 50 ms after the first
        options.order_burst = 1;
        return std::make_unique<BinanceUSTradingVenue>(
            config::BinanceUSCredentials{"key", "secret"},
            std::make_shared<RulesSource>(),
            std::vector<types::Symbol>{"ZEC/USDT"}, f.http, options);
    }();
    for (int i = 0; i < 3; ++i) {
        f.http->Push("/api/v3/order", Placed(i, "x", "NEW"));
    }
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 3; ++i) {
        ASSERT_OK(venue->PlaceOrder(Limit(types::Side::kBuy, "0.01", "700")));
    }
    // Two waits of 50 ms; a little slack for the clock's granularity.
    EXPECT_GE(std::chrono::steady_clock::now() - start, 90ms);
}

}  // namespace
}  // namespace kestrel::trading
