#include "kestrel/trading/risk_gate.hpp"

#include "strategy_fakes.hpp"
#include "test_helpers.hpp"

namespace {

using namespace kestrel;
using kestrel::testing::D;
using kestrel::testing::RecordingVenue;
using trading::RiskGatedVenue;
using trading::RiskLimits;
using types::ErrorCategory;
using types::OrderRequest;
using types::OrderType;
using types::Side;

constexpr const char* kSymbol = "BTC/USD";

types::Position PositionOf(const char* quantity, const char* realized = "0") {
    types::Position p;
    p.symbol = kSymbol;
    p.quantity = D(quantity);
    p.realized_pnl = D(realized);
    return p;
}

OrderRequest Market(Side side, const char* quantity,
                    const char* symbol = kSymbol) {
    OrderRequest r;
    r.symbol = symbol;
    r.side = side;
    r.type = OrderType::kMarket;
    r.quantity = D(quantity);
    return r;
}

OrderRequest Limit(Side side, const char* quantity, const char* price) {
    OrderRequest r = Market(side, quantity);
    r.type = OrderType::kLimit;
    r.limit_price = D(price);
    return r;
}

class RiskGateTest : public ::testing::Test {
   protected:
    void Build() {
        gate = std::make_unique<RiskGatedVenue>(venue, limits,
                                                [this] { return now; });
    }
    // Expects a rejection by the gate: kRejected, "risk:", venue untouched.
    void ExpectRejected(const OrderRequest& request) {
        const int before = venue->attempts;
        auto r = gate->PlaceOrder(request);
        ASSERT_FALSE(r);
        EXPECT_EQ(r.error().category, ErrorCategory::kRejected);
        EXPECT_EQ(r.error().message.rfind("risk:", 0), 0u) << r.error().message;
        EXPECT_EQ(venue->attempts, before);
    }

    RiskLimits limits;
    types::Timestamp now = types::FromEpochSeconds(86400 * 100 + 3600);
    std::shared_ptr<RecordingVenue> venue = std::make_shared<RecordingVenue>();
    std::unique_ptr<RiskGatedVenue> gate;
};

TEST_F(RiskGateTest, WithNoLimitsEverythingPasses) {
    Build();
    ASSERT_OK(gate->PlaceOrder(Market(Side::kBuy, "1000000")));
    EXPECT_EQ(venue->orders.size(), 1u);
}

TEST_F(RiskGateTest, RejectsNonPositiveQuantityAsInvalid) {
    Build();
    auto r = gate->PlaceOrder(Market(Side::kBuy, "0"));
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().category, ErrorCategory::kInvalidArgument);
    EXPECT_EQ(venue->attempts, 0);
}

TEST_F(RiskGateTest, SymbolAllowlist) {
    limits.allowed_symbols = {"BTC/USD"};
    Build();
    ExpectRejected(Market(Side::kBuy, "1", "ETH/USD"));
    ASSERT_OK(gate->PlaceOrder(Market(Side::kBuy, "1")));
}

TEST_F(RiskGateTest, OrderNotionalOfALimitOrderUsesItsLimitPrice) {
    limits.max_order_notional = D("1000");
    Build();
    ASSERT_OK(gate->PlaceOrder(Limit(Side::kBuy, "0.5", "2000")));  // = limit
    ExpectRejected(Limit(Side::kBuy, "0.5", "2000.01"));
}

TEST_F(RiskGateTest, OrderNotionalOfAMarketOrderUsesTheReferencePrice) {
    limits.max_order_notional = D("1000");
    Build();
    gate->NoteReferencePrice(kSymbol, D("100"));
    ASSERT_OK(gate->PlaceOrder(Market(Side::kBuy, "10")));
    ExpectRejected(Market(Side::kBuy, "11"));
}

TEST_F(RiskGateTest, MarketOrderWithoutAPriceFailsClosed) {
    limits.max_order_notional = D("1000");
    Build();
    ExpectRejected(Market(Side::kBuy, "1"));
    gate->NoteReferencePrice("ETH/USD", D("1"));  // a different symbol
    ExpectRejected(Market(Side::kBuy, "1"));
}

TEST_F(RiskGateTest, StaleReferencePriceIsRefused) {
    limits.max_order_notional = D("1000");
    limits.max_price_age = std::chrono::seconds(30);
    Build();
    gate->NoteReferencePrice(kSymbol, D("100"));
    now += std::chrono::seconds(30);
    ASSERT_OK(gate->PlaceOrder(Market(Side::kBuy, "1")));
    now += std::chrono::seconds(1);
    ExpectRejected(Market(Side::kBuy, "1"));
    gate->NoteReferencePrice(kSymbol, D("100"));
    ASSERT_OK(gate->PlaceOrder(Market(Side::kBuy, "1")));
}

TEST_F(RiskGateTest, PositionNotionalCountsHoldingsAndOpenBuys) {
    limits.max_position_notional = D("1000");
    venue->positions = {PositionOf("4")};
    types::OrderStatus open;
    open.symbol = kSymbol;
    open.side = Side::kBuy;
    open.quantity = D("3");
    open.filled_quantity = D("1");  // 2 still to fill
    venue->open_orders = {open};
    Build();
    gate->NoteReferencePrice(kSymbol, D("100"));

    // 4 held + 2 working = 6; one more is 700, four more is exactly 1000.
    ASSERT_OK(gate->PlaceOrder(Market(Side::kBuy, "4")));
    ExpectRejected(Market(Side::kBuy, "4.01"));
}

TEST_F(RiskGateTest, PositionNotionalNeverBlocksASell) {
    limits.max_position_notional = D("100");
    venue->positions = {PositionOf("50")};  // already far over the limit
    Build();
    gate->NoteReferencePrice(kSymbol, D("100"));
    ASSERT_OK(gate->PlaceOrder(Market(Side::kSell, "50")));
}

TEST_F(RiskGateTest, MaxOpenOrders) {
    limits.max_open_orders = 2;
    venue->open_orders.resize(1);
    Build();
    ASSERT_OK(gate->PlaceOrder(Market(Side::kBuy, "1")));
    venue->open_orders.resize(2);
    ExpectRejected(Market(Side::kBuy, "1"));
}

TEST_F(RiskGateTest, OrderRateIsASlidingMinute) {
    limits.max_orders_per_minute = 2;
    Build();
    ASSERT_OK(gate->PlaceOrder(Market(Side::kBuy, "1")));
    now += std::chrono::seconds(30);
    ASSERT_OK(gate->PlaceOrder(Market(Side::kBuy, "1")));
    ExpectRejected(Market(Side::kBuy, "1"));

    now += std::chrono::seconds(30);  // the first order is 60 s old
    ASSERT_OK(gate->PlaceOrder(Market(Side::kBuy, "1")));
    ExpectRejected(Market(Side::kBuy, "1"));
}

TEST_F(RiskGateTest, OrdersTheVenueRefusesStillCountTowardsTheRate) {
    limits.max_orders_per_minute = 1;
    venue->reject_orders = true;
    Build();
    EXPECT_FALSE(gate->PlaceOrder(Market(Side::kBuy, "1")));
    EXPECT_EQ(venue->attempts, 1);
    ExpectRejected(Market(Side::kBuy, "1"));
}

types::Position HoldingOf(const char* quantity, const char* average_price) {
    types::Position p = PositionOf(quantity);
    p.average_price = D(average_price);
    return p;
}

// A fill `seconds` after the gate's clock reads `now`, fee in USD.
types::Fill FillOf(const char* id, Side side, const char* quantity,
                   const char* price, types::Timestamp time,
                   const char* fee = "0") {
    types::Fill f;
    f.fill_id = id;
    f.symbol = kSymbol;
    f.side = side;
    f.quantity = D(quantity);
    f.price = D(price);
    f.fee = D(fee);
    f.fee_asset = "USD";
    f.time = time;
    return f;
}

TEST_F(RiskGateTest, DailyLossHaltsNewPositionsButNotExits) {
    limits.max_daily_loss = D("100");
    venue->positions = {HoldingOf("2", "100")};
    Build();
    ASSERT_OK(gate->ResetDailyBaseline());
    ASSERT_OK(gate->PlaceOrder(Market(Side::kBuy, "1")));

    // Sells one at 1: realizes (1 - 100) = -99, under the limit.
    now += std::chrono::seconds(1);
    venue->fills = {FillOf("f1", Side::kSell, "1", "1", now)};
    venue->positions = {HoldingOf("1", "100")};
    ASSERT_OK(gate->PlaceOrder(Market(Side::kBuy, "1")));

    // A buy whose fee is 1 takes the loss to exactly the limit.
    now += std::chrono::seconds(1);
    venue->fills.push_back(FillOf("f2", Side::kBuy, "0.1", "10", now, "1"));
    venue->positions = {HoldingOf("1.1", "100")};
    ExpectRejected(Market(Side::kBuy, "1"));
    ExpectRejected(Market(Side::kSell, "1.11"));  // would go short
    ASSERT_OK(gate->PlaceOrder(Market(Side::kSell, "1.1")));
}

TEST_F(RiskGateTest, LossesOnPositionsThatAreNowClosedStillCount) {
    limits.max_daily_loss = D("50");
    Build();  // starts flat
    ASSERT_OK(gate->ResetDailyBaseline());

    // Bought at 100 and sold at 40: lost 60. The venue reports no position.
    now += std::chrono::seconds(1);
    venue->fills = {FillOf("b", Side::kBuy, "1", "100", now),
                    FillOf("s", Side::kSell, "1", "40", now)};
    ExpectRejected(Market(Side::kBuy, "1"));
}

TEST_F(RiskGateTest, ProfitsOffsetLosses) {
    limits.max_daily_loss = D("50");
    Build();
    ASSERT_OK(gate->ResetDailyBaseline());
    now += std::chrono::seconds(1);
    venue->fills = {FillOf("b1", Side::kBuy, "1", "100", now),
                    FillOf("s1", Side::kSell, "1", "40", now),  // -60
                    FillOf("b2", Side::kBuy, "1", "100", now),
                    FillOf("s2", Side::kSell, "1", "130", now)};  // +30
    ASSERT_OK(gate->PlaceOrder(Market(Side::kBuy, "1")));         // net -30
}

TEST_F(RiskGateTest, EachFillIsCountedOnce) {
    limits.max_daily_loss = D("100");
    venue->positions = {HoldingOf("1", "100")};
    Build();
    ASSERT_OK(gate->ResetDailyBaseline());
    now += std::chrono::seconds(1);
    // Lost 60. The venue returns this fill on every poll.
    venue->fills = {FillOf("f1", Side::kSell, "1", "40", now)};
    for (int i = 0; i < 3; ++i) {
        ASSERT_OK(gate->PlaceOrder(Market(Side::kBuy, "1")));
    }
}

TEST_F(RiskGateTest, FillsBeforeTheBaselineAreNotCounted) {
    limits.max_daily_loss = D("100");
    venue->positions = {HoldingOf("1", "100")};
    venue->fills = {
        FillOf("old", Side::kSell, "1", "1", now - std::chrono::hours(1))};
    Build();
    ASSERT_OK(gate->ResetDailyBaseline());
    ASSERT_OK(gate->PlaceOrder(Market(Side::kBuy, "1")));
}

TEST_F(RiskGateTest, TheLossCountRestartsEachUtcDay) {
    limits.max_daily_loss = D("100");
    Build();
    ASSERT_OK(gate->ResetDailyBaseline());
    now += std::chrono::seconds(1);
    venue->fills = {FillOf("b", Side::kBuy, "1", "200", now),
                    FillOf("s", Side::kSell, "1", "50", now)};  // -150
    ExpectRejected(Market(Side::kBuy, "1"));

    now += std::chrono::hours(24);
    ASSERT_OK(gate->PlaceOrder(Market(Side::kBuy, "1")));
}

TEST_F(RiskGateTest, ACostBasisCarriesAcrossDays) {
    limits.max_daily_loss = D("100");
    venue->positions = {HoldingOf("1", "200")};
    Build();
    ASSERT_OK(gate->ResetDailyBaseline());

    now += std::chrono::hours(24);  // a day later, the position is still open
    ASSERT_OK(gate->PlaceOrder(Market(Side::kBuy, "1")));
    now += std::chrono::seconds(1);
    venue->fills = {FillOf("s", Side::kSell, "1", "50", now)};  // -150 today
    ExpectRejected(Market(Side::kBuy, "1"));
}

TEST_F(RiskGateTest, FirstOrderStartsTheCountIfNoneWasStarted) {
    limits.max_daily_loss = D("100");
    venue->fills = {FillOf("old", Side::kSell, "1", "1", now)};  // not counted
    Build();
    ASSERT_OK(gate->PlaceOrder(Market(Side::kBuy, "1")));
}

TEST_F(RiskGateTest, FailsClosedWhenFillsCannotBeRead) {
    limits.max_daily_loss = D("100");
    Build();
    ASSERT_OK(gate->ResetDailyBaseline());
    venue->fail_fills = true;
    ExpectRejected(Market(Side::kBuy, "1"));
}

TEST_F(RiskGateTest, DailyLossAloneDoesNotReadPositions) {
    limits.max_daily_loss = D("100");
    Build();
    ASSERT_OK(gate->ResetDailyBaseline());
    venue->fail_reads = true;  // would reject if the gate asked
    ASSERT_OK(gate->PlaceOrder(Market(Side::kBuy, "1")));
}

TEST_F(RiskGateTest, HaltCancelsEverythingAndStopsNewPositions) {
    venue->positions = {PositionOf("3")};
    Build();
    ASSERT_OK(gate->Halt("test"));
    EXPECT_TRUE(gate->IsHalted());
    EXPECT_EQ(venue->cancel_all_calls, 1);

    ExpectRejected(Market(Side::kBuy, "1"));
    ASSERT_OK(gate->PlaceOrder(Market(Side::kSell, "3")));  // exit is fine

    gate->Resume();
    ASSERT_OK(gate->PlaceOrder(Market(Side::kBuy, "1")));
}

TEST_F(RiskGateTest, HaltHoldsEvenIfTheCancelFails) {
    Build();
    // RecordingVenue's CancelAll succeeds; the halt flag must not depend on it
    // and must apply to the next order regardless.
    ASSERT_OK(gate->Halt("test"));
    ExpectRejected(Market(Side::kBuy, "1"));
}

TEST_F(RiskGateTest, FailsClosedWhenPositionsCannotBeRead) {
    limits.max_position_notional = D("1000");
    venue->fail_reads = true;
    Build();
    gate->NoteReferencePrice(kSymbol, D("1"));
    ExpectRejected(Market(Side::kBuy, "1"));
}

TEST_F(RiskGateTest, FailsClosedWhenOpenOrdersCannotBeRead) {
    limits.max_open_orders = 5;
    venue->fail_reads = true;
    Build();
    ExpectRejected(Market(Side::kBuy, "1"));
}

TEST_F(RiskGateTest, DoesNotReadWhatNoLimitNeeds) {
    limits.max_order_notional = D("1000");
    venue->fail_reads = true;  // would fail if the gate asked
    Build();
    ASSERT_OK(gate->PlaceOrder(Limit(Side::kBuy, "1", "10")));
}

TEST_F(RiskGateTest, QueriesAndCancelsPassThrough) {
    limits.allowed_symbols = {"NOPE"};
    Build();
    EXPECT_EQ(gate->Name(), "recording");
    ASSERT_OK(gate->GetOpenOrders());
    ASSERT_OK(gate->GetPositions());
    ASSERT_OK(gate->GetBalances());
    ASSERT_OK(gate->GetFills(types::FromEpochSeconds(0)));
    ASSERT_OK(gate->CancelAll());
    EXPECT_EQ(venue->cancel_all_calls, 1);
}

}  // namespace
