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

struct Fixture {
    std::shared_ptr<RoutedHttp> http = std::make_shared<RoutedHttp>();
    types::Timestamp now = types::FromEpochMillis(kNowMs);

    std::unique_ptr<BinanceUSTradingVenue> Venue(
        std::vector<types::Symbol> symbols = {"ZEC/USDT"}) {
        http->Push("/api/v3/time",
                   "{\"serverTime\":" + std::to_string(kNowMs) + "}");
        BinanceUSApi::Options options;
        options.clock = [this] { return now; };
        options.sleeper = [](std::chrono::milliseconds) {};
        options.weight_per_second = 100000;
        options.weight_burst = 100000;
        return std::make_unique<BinanceUSTradingVenue>(
            config::BinanceUSCredentials{"key", "secret"}, nullptr,
            std::move(symbols), http, options);
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

TEST(BinanceUSTradingVenue, CannotTradeYet) {
    Fixture f;
    auto venue = f.Venue();
    types::OrderRequest request;
    request.symbol = "ZEC/USDT";
    request.quantity = D("0.01");
    auto placed = venue->PlaceOrder(request);
    ASSERT_FALSE(placed.has_value());
    EXPECT_EQ(placed.error().category, types::ErrorCategory::kNotImplemented);
    EXPECT_EQ(venue->CancelOrder("1").error().category,
              types::ErrorCategory::kNotImplemented);
    EXPECT_EQ(venue->CancelAll().error().category,
              types::ErrorCategory::kNotImplemented);
    EXPECT_TRUE(f.http->To("/api/v3/order").empty());  // nothing was sent
}

}  // namespace
}  // namespace kestrel::trading
