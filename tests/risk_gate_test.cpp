#include "kestrel/trading/risk_gate.hpp"

#include <condition_variable>
#include <future>
#include <mutex>

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

TEST_F(RiskGateTest, AnOcoCountsAsTwoOrders) {
    types::OcoRequest pair;
    pair.symbol = kSymbol;
    pair.side = Side::kSell;
    pair.quantity = D("1");
    pair.limit_price = D("110");
    pair.stop_price = D("90");
    venue->oco = true;
    limits.max_open_orders = 2;
    limits.max_orders_per_minute = 3;
    venue->open_orders.resize(1);
    Build();
    auto refused = gate->PlaceOco(pair);  // 1 open + 2 is over 2
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().category, ErrorCategory::kRejected);
    EXPECT_TRUE(venue->ocos.empty());
    venue->open_orders.clear();
    ASSERT_OK(gate->PlaceOco(pair));
    EXPECT_EQ(venue->ocos.size(), 1u);
    // Two of the three this minute are used: one more order, not a pair.
    venue->open_orders.clear();
    EXPECT_FALSE(gate->PlaceOco(pair).has_value());
    ASSERT_OK(gate->PlaceOrder(Market(Side::kBuy, "1")));
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

TEST_F(RiskGateTest, OrdersPerDayBlocksEntriesButNotExitsUntilTomorrow) {
    limits.max_orders_per_day = 2;
    Build();
    ASSERT_OK(gate->PlaceOrder(Market(Side::kBuy, "1")));
    ASSERT_OK(gate->PlaceOrder(Market(Side::kBuy, "1")));
    ExpectRejected(Market(Side::kBuy, "1"));

    venue->positions = {PositionOf("2")};
    ASSERT_OK(gate->PlaceOrder(Market(Side::kSell, "2")));  // exit still works
    ExpectRejected(Market(Side::kBuy, "1"));

    now += std::chrono::hours(24);
    ASSERT_OK(gate->PlaceOrder(Market(Side::kBuy, "1")));
}

TEST_F(RiskGateTest, ConsecutiveLossesBlockEntriesUntilAWinOrANewDay) {
    limits.max_consecutive_losses = 2;
    Build();  // starts flat
    ASSERT_OK(gate->ResetDailyBaseline());

    auto trade = [&](const char* id, Side side, const char* price) {
        now += std::chrono::seconds(1);
        venue->fills.push_back(FillOf(id, side, "1", price, now));
    };
    trade("b1", Side::kBuy, "100");
    trade("s1", Side::kSell, "90");  // loss 1
    trade("b2", Side::kBuy, "100");
    trade("s2", Side::kSell, "110");  // a win resets the count
    trade("b3", Side::kBuy, "100");
    trade("s3", Side::kSell, "95");  // loss 1 again
    ASSERT_OK(gate->PlaceOrder(Market(Side::kBuy, "1")));

    trade("b4", Side::kBuy, "100");
    trade("s4", Side::kSell, "99");  // loss 2: at the limit
    ExpectRejected(Market(Side::kBuy, "1"));

    // The count lapses with the UTC day.
    now += std::chrono::hours(24);
    ASSERT_OK(gate->PlaceOrder(Market(Side::kBuy, "1")));
}

TEST_F(RiskGateTest, ConsecutiveLossesStillAllowAnExit) {
    limits.max_consecutive_losses = 1;
    venue->positions = {HoldingOf("1", "100")};
    Build();
    ASSERT_OK(gate->ResetDailyBaseline());
    now += std::chrono::seconds(1);
    venue->fills = {FillOf("s1", Side::kSell, "1", "90", now)};
    venue->positions = {HoldingOf("1", "100")};  // still holds some
    ExpectRejected(Market(Side::kBuy, "1"));
    ASSERT_OK(gate->PlaceOrder(Market(Side::kSell, "1")));
}

TEST_F(RiskGateTest, DrawdownIsMeasuredFromThePeakIncludingUnrealized) {
    limits.max_drawdown = D("50");
    venue->positions = {HoldingOf("1", "100")};
    Build();
    ASSERT_OK(gate->ResetDailyBaseline());

    gate->NoteReferencePrice(kSymbol, D("130"));  // +30 unrealized: new peak
    ASSERT_OK(gate->PlaceOrder(Market(Side::kBuy, "1")));
    gate->NoteReferencePrice(kSymbol, D("85"));  // -15; 45 below the peak
    ASSERT_OK(gate->PlaceOrder(Market(Side::kBuy, "1")));
    gate->NoteReferencePrice(kSymbol, D("79"));  // 51 below the peak
    ExpectRejected(Market(Side::kBuy, "1"));
    ASSERT_OK(gate->PlaceOrder(Market(Side::kSell, "1")));  // exit is allowed
}

TEST_F(RiskGateTest, DrawdownCountsRealizedLossesAndDoesNotResetAtMidnight) {
    limits.max_drawdown = D("20");
    Build();
    ASSERT_OK(gate->ResetDailyBaseline());
    now += std::chrono::seconds(1);
    venue->fills = {FillOf("b1", Side::kBuy, "1", "100", now),
                    FillOf("s1", Side::kSell, "1", "85", now)};  // lost 15
    ASSERT_OK(gate->PlaceOrder(Market(Side::kBuy, "1")));

    now += std::chrono::hours(24);
    venue->fills.push_back(FillOf("b2", Side::kBuy, "1", "100", now));
    venue->fills.push_back(FillOf("s2", Side::kSell, "1", "95", now));  // -5
    ExpectRejected(Market(Side::kBuy, "1"));  // 20 down in all, over two days
}

TEST_F(RiskGateTest, DrawdownFailsClosedWithoutAPriceForWhatIsHeld) {
    limits.max_drawdown = D("50");
    venue->positions = {HoldingOf("1", "100")};
    Build();
    ASSERT_OK(gate->ResetDailyBaseline());
    ExpectRejected(Market(Side::kBuy, "1"));
    ASSERT_OK(gate->PlaceOrder(Market(Side::kSell, "1")));
    gate->NoteReferencePrice(kSymbol, D("100"));
    ASSERT_OK(gate->PlaceOrder(Market(Side::kBuy, "1")));
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
    EXPECT_EQ(venue->cancel_all_calls, 2);  // see the in-flight test below

    ExpectRejected(Market(Side::kBuy, "1"));
    ASSERT_OK(gate->PlaceOrder(Market(Side::kSell, "3")));  // exit is fine

    gate->Resume();
    ASSERT_OK(gate->PlaceOrder(Market(Side::kBuy, "1")));
}

// Binance.US spot reports no positions: the gate's own count of what filled
// since it started tells what can be sold.
TEST_F(RiskGateTest, AHaltLetsAnExitThroughOnAVenueWithoutPositions) {
    Build();
    ASSERT_OK(gate->ResetDailyBaseline());
    now += std::chrono::seconds(1);
    venue->fills = {FillOf("f1", Side::kBuy, "2", "100", now)};
    ASSERT_OK(gate->Halt("test"));

    ExpectRejected(Market(Side::kBuy, "1"));
    ExpectRejected(Market(Side::kSell, "2.5"));  // more than was bought
    ASSERT_OK(gate->PlaceOrder(Market(Side::kSell, "2")));
}

TEST_F(RiskGateTest, ALossLimitLetsAnExitThroughOnAVenueWithoutPositions) {
    limits.max_daily_loss = D("50");
    Build();
    ASSERT_OK(gate->ResetDailyBaseline());
    now += std::chrono::seconds(1);
    venue->fills = {FillOf("f1", Side::kBuy, "2", "100", now),
                    FillOf("f2", Side::kSell, "1", "40", now)};  // -60
    ExpectRejected(Market(Side::kBuy, "1"));
    ASSERT_OK(gate->PlaceOrder(Market(Side::kSell, "1")));
}

TEST_F(RiskGateTest, PositionNotionalCountsWhatFilledOnAVenueWithoutPositions) {
    limits.max_position_notional = D("150");
    Build();
    gate->NoteReferencePrice(kSymbol, D("100"));
    ASSERT_OK(gate->ResetDailyBaseline());
    now += std::chrono::seconds(1);
    venue->fills = {FillOf("f1", Side::kBuy, "1", "100", now)};
    ExpectRejected(Market(Side::kBuy, "1"));  // 2 held would be 200
    ASSERT_OK(gate->PlaceOrder(Market(Side::kBuy, "0.5")));
}

TEST_F(RiskGateTest, WithoutFillsAHaltStillLetsTheVenuesPositionOut) {
    venue->positions = {PositionOf("3")};
    Build();
    ASSERT_OK(gate->ResetDailyBaseline());
    ASSERT_OK(gate->Halt("test"));
    venue->fail_fills = true;
    ExpectRejected(Market(Side::kSell, "3.5"));
    ASSERT_OK(gate->PlaceOrder(Market(Side::kSell, "3")));
}

TEST_F(RiskGateTest, WithoutFillsPositionNotionalFailsClosed) {
    limits.max_position_notional = D("1000");
    Build();
    gate->NoteReferencePrice(kSymbol, D("1"));
    ASSERT_OK(gate->ResetDailyBaseline());
    venue->fail_fills = true;
    ExpectRejected(Market(Side::kBuy, "1"));
}

// A venue whose PlaceOrder waits for the test, and which notes how many
// orders had landed at each CancelAll.
class SlowVenue : public RecordingVenue {
   public:
    types::Result<types::OrderAcknowledgement> PlaceOrder(
        const OrderRequest& request) override {
        entered.set_value();
        release.get_future().wait();
        return RecordingVenue::PlaceOrder(request);
    }
    types::Result<std::vector<std::string>> CancelAll() override {
        {
            std::lock_guard lock(mutex);
            orders_at_cancel.push_back(orders.size());
        }
        cancelled.notify_all();
        return RecordingVenue::CancelAll();
    }

    std::promise<void> entered;
    std::promise<void> release;
    std::mutex mutex;
    std::condition_variable cancelled;
    std::vector<std::size_t> orders_at_cancel;
};

TEST_F(RiskGateTest, HaltCancelsAnOrderThatWasInFlight) {
    auto slow = std::make_shared<SlowVenue>();
    gate = std::make_unique<RiskGatedVenue>(slow, limits,
                                            [this] { return now; });

    auto placing = std::async(std::launch::async, [&] {
        return gate->PlaceOrder(Market(Side::kBuy, "1"));
    });
    slow->entered.get_future().wait();  // the order is inside the venue call

    auto halting = std::async(std::launch::async,
                              [&] { return gate->Halt("test"); });
    {
        // The first cancel goes out without waiting for the order.
        std::unique_lock lock(slow->mutex);
        ASSERT_TRUE(slow->cancelled.wait_for(
            lock, std::chrono::seconds(5),
            [&] { return !slow->orders_at_cancel.empty(); }));
    }
    EXPECT_EQ(halting.wait_for(std::chrono::milliseconds(50)),
              std::future_status::timeout);  // now waiting for the order

    slow->release.set_value();
    ASSERT_OK(placing.get());
    ASSERT_OK(halting.get());

    // The second cancel ran after the order landed.
    ASSERT_EQ(slow->orders_at_cancel.size(), 2u);
    EXPECT_EQ(slow->orders_at_cancel[0], 0u);
    EXPECT_EQ(slow->orders_at_cancel[1], 1u);
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

TEST_F(RiskGateTest, WithShortsAHaltLetsACoverThrough) {
    limits.shorts = true;
    venue->positions = {PositionOf("-2")};
    Build();
    ASSERT_OK(gate->Halt("test"));
    ExpectRejected(Market(Side::kSell, "1"));   // deeper
    ExpectRejected(Market(Side::kBuy, "2.5"));  // more than is short
    ASSERT_OK(gate->PlaceOrder(Market(Side::kBuy, "2")));
}

TEST_F(RiskGateTest, WithoutShortsABuyNeverReducesAPosition) {
    venue->positions = {PositionOf("-2")};
    Build();
    ASSERT_OK(gate->Halt("test"));
    ExpectRejected(Market(Side::kBuy, "1"));
}

TEST_F(RiskGateTest, WithShortsPositionNotionalCountsAShort) {
    limits.shorts = true;
    limits.max_position_notional = D("1000");
    venue->positions = {PositionOf("2")};
    Build();
    gate->NoteReferencePrice(kSymbol, D("100"));
    // Selling 12 leaves 10 short: 1,000, at the limit.
    ASSERT_OK(gate->PlaceOrder(Market(Side::kSell, "12")));
    ExpectRejected(Market(Side::kSell, "12.01"));
}

TEST_F(RiskGateTest, WithShortsALosingCoverCountsAsALoss) {
    limits.shorts = true;
    limits.max_consecutive_losses = 1;
    Build();  // starts flat
    ASSERT_OK(gate->ResetDailyBaseline());
    now += std::chrono::seconds(1);
    venue->fills = {FillOf("s1", Side::kSell, "1", "100", now)};
    // A short sale is no loss, though it pays a fee.
    ASSERT_OK(gate->PlaceOrder(Market(Side::kBuy, "1")));
    now += std::chrono::seconds(1);
    venue->fills.push_back(FillOf("b1", Side::kBuy, "1", "110", now));
    ExpectRejected(Market(Side::kSell, "1"));
}

TEST_F(RiskGateTest, WithShortsTheVenuesLargerShortCounts) {
    limits.shorts = true;
    limits.max_position_notional = D("1000");
    venue->positions = {PositionOf("-5")};
    Build();
    ASSERT_OK(gate->ResetDailyBaseline());
    gate->NoteReferencePrice(kSymbol, D("100"));
    ASSERT_OK(gate->PlaceOrder(Market(Side::kSell, "5")));  // 10 short
    ExpectRejected(Market(Side::kSell, "5.01"));
}

TEST_F(RiskGateTest, WithShortsPositionNotionalNeverBlocksAnExit) {
    limits.shorts = true;
    limits.max_position_notional = D("100");
    venue->positions = {PositionOf("50")};
    Build();
    gate->NoteReferencePrice(kSymbol, D("100"));
    ASSERT_OK(gate->PlaceOrder(Market(Side::kSell, "50")));
}

types::OrderStatus OpenOrder(const char* id, const char* symbol, Side side,
                             const char* quantity) {
    types::OrderStatus o;
    o.order_id = id;
    o.symbol = symbol;
    o.side = side;
    o.type = OrderType::kLimit;
    o.quantity = D(quantity);
    return o;
}

types::Balance BalanceOf(const char* asset, const char* total) {
    return types::Balance{asset, D(total), D(total), D("0")};
}

TEST_F(RiskGateTest, TotalNotionalCountsEverySymbolAndOpenBuys) {
    limits.max_total_notional = D("300");
    venue->positions = {PositionOf("1")};  // BTC at 100
    venue->open_orders = {OpenOrder("o1", "ETH/USD", Side::kBuy, "1")};
    Build();
    gate->NoteReferencePrice(kSymbol, D("100"));
    gate->NoteReferencePrice("ETH/USD", D("50"));
    // 100 of BTC and 50 of ETH on order: 150 more of ETH is 300.
    ASSERT_OK(gate->PlaceOrder(Market(Side::kBuy, "3", "ETH/USD")));
    ExpectRejected(Market(Side::kBuy, "3.01", "ETH/USD"));
    ExpectRejected(Market(Side::kBuy, "1.51"));  // 151 more of BTC
}

TEST_F(RiskGateTest, TotalNotionalFailsClosedWithoutAPriceForAHolding) {
    limits.max_total_notional = D("1000");
    venue->positions = {PositionOf("1")};  // BTC, never priced
    Build();
    gate->NoteReferencePrice("ETH/USD", D("50"));
    ExpectRejected(Market(Side::kBuy, "1", "ETH/USD"));
}

TEST_F(RiskGateTest, SymbolShareIsMeasuredAgainstEquity) {
    limits.max_symbol_share = D("0.5");
    venue->positions = {PositionOf("1")};
    // 1,000 of cash and 100 of BTC; DOGE has no price, so is left out.
    venue->balances = {BalanceOf("USD", "1000"), BalanceOf("BTC", "1"),
                       BalanceOf("DOGE", "500")};
    Build();
    gate->NoteReferencePrice(kSymbol, D("100"));
    ExpectRejected(Market(Side::kBuy, "4.51"));  // 551 of 1,100
    ASSERT_OK(gate->PlaceOrder(Market(Side::kBuy, "4.5")));
}

TEST_F(RiskGateTest, SymbolShareFailsClosed) {
    limits.max_symbol_share = D("0.5");
    venue->balances = {BalanceOf("USD", "-10")};
    Build();
    gate->NoteReferencePrice(kSymbol, D("100"));
    ExpectRejected(Market(Side::kBuy, "0.01"));  // equity below zero
    venue->fail_balances = true;
    ExpectRejected(Market(Side::kBuy, "0.01"));
}

TEST_F(RiskGateTest, DrawdownPeaksBetweenOrdersAreSeen) {
    limits.max_drawdown = D("50");
    venue->positions = {HoldingOf("1", "100")};
    Build();
    ASSERT_OK(gate->ResetDailyBaseline());
    gate->NoteReferencePrice(kSymbol, D("160"));  // a peak of +60, no order
    now += std::chrono::seconds(1);
    gate->NoteReferencePrice(kSymbol, D("105"));  // 55 below it
    ExpectRejected(Market(Side::kBuy, "1"));
    ASSERT_OK(gate->PlaceOrder(Market(Side::kSell, "1")));
}

TEST_F(RiskGateTest, PricesMeasureTheLossesAtMostOnceASecond) {
    limits.max_drawdown = D("50");
    Build();
    ASSERT_OK(gate->ResetDailyBaseline());
    const int before = venue->get_fills_calls;
    gate->NoteReferencePrice(kSymbol, D("100"));
    gate->NoteReferencePrice(kSymbol, D("101"));
    EXPECT_EQ(venue->get_fills_calls, before + 1);
    now += std::chrono::seconds(1);
    gate->NoteReferencePrice(kSymbol, D("102"));
    EXPECT_EQ(venue->get_fills_calls, before + 2);
}

TEST_F(RiskGateTest, DrawdownCloseHaltsAndClosesEverything) {
    limits.max_drawdown_close = D("50");
    venue->positions = {HoldingOf("2", "100")};
    Build();
    ASSERT_OK(gate->ResetDailyBaseline());
    gate->NoteReferencePrice(kSymbol, D("80"));  // -40
    EXPECT_FALSE(gate->IsHalted());
    EXPECT_TRUE(venue->orders.empty());

    now += std::chrono::seconds(1);
    gate->NoteReferencePrice(kSymbol, D("74"));  // -52
    EXPECT_TRUE(gate->IsHalted());
    EXPECT_EQ(venue->cancel_all_calls, 1);
    ASSERT_EQ(venue->orders.size(), 1u);
    EXPECT_EQ(venue->orders[0].side, Side::kSell);
    EXPECT_EQ(venue->orders[0].type, OrderType::kMarket);
    EXPECT_EQ(venue->orders[0].quantity, D("2"));

    ExpectRejected(Market(Side::kBuy, "1"));
    now += std::chrono::seconds(1);
    gate->NoteReferencePrice(kSymbol, D("70"));  // closing already
    EXPECT_EQ(venue->orders.size(), 1u);

    // Still held a minute on (the close did not fill): closed again, without
    // a second halt.
    now += std::chrono::minutes(1);
    gate->NoteReferencePrice(kSymbol, D("70"));
    EXPECT_EQ(venue->cancel_all_calls, 1);
    ASSERT_EQ(venue->orders.size(), 2u);
    EXPECT_EQ(venue->orders[1].side, Side::kSell);

    // Resume halts again while the fall stands.
    gate->Resume();
    now += std::chrono::seconds(1);
    gate->NoteReferencePrice(kSymbol, D("70"));
    EXPECT_TRUE(gate->IsHalted());
}

TEST_F(RiskGateTest, DrawdownCloseClosesAGateHaltedBefore) {
    limits.max_drawdown_close = D("50");
    venue->positions = {HoldingOf("2", "100")};
    Build();
    ASSERT_OK(gate->ResetDailyBaseline());
    ASSERT_OK(gate->Halt("by hand"));
    gate->NoteReferencePrice(kSymbol, D("74"));  // -52
    ASSERT_EQ(venue->orders.size(), 1u);
    EXPECT_EQ(venue->orders[0].side, Side::kSell);
}

TEST_F(RiskGateTest, WithShortsTheSizeLimitsDoNotBlockACover) {
    limits.shorts = true;
    limits.max_total_notional = D("100");
    limits.max_symbol_share = D("0.1");
    venue->positions = {PositionOf("-2")};
    venue->balances = {BalanceOf("USD", "-50")};  // equity below zero
    Build();
    gate->NoteReferencePrice(kSymbol, D("100"));
    ASSERT_OK(gate->PlaceOrder(Market(Side::kBuy, "2")));
    ExpectRejected(Market(Side::kBuy, "3"));  // past the short: a long
}

TEST_F(RiskGateTest, DrawdownCloseAlsoTripsAtAnOrder) {
    limits.max_drawdown_close = D("50");
    venue->positions = {HoldingOf("1", "100")};
    Build();
    ASSERT_OK(gate->ResetDailyBaseline());
    gate->NoteReferencePrice(kSymbol, D("100"));
    // Within the second: the price is noted but the losses not measured.
    gate->NoteReferencePrice(kSymbol, D("49"));
    EXPECT_FALSE(gate->IsHalted());
    auto r = gate->PlaceOrder(Market(Side::kBuy, "1"));
    ASSERT_FALSE(r);
    EXPECT_TRUE(gate->IsHalted());
    ASSERT_EQ(venue->orders.size(), 1u);  // the close, not the buy
    EXPECT_EQ(venue->orders[0].side, Side::kSell);
}

TEST_F(RiskGateTest, PositionLossClosesThatSymbolAndBlocksItForTheDay) {
    limits.max_loss_per_position = D("30");
    venue->positions = {HoldingOf("1", "100")};
    venue->open_orders = {OpenOrder("stop", kSymbol, Side::kSell, "1"),
                          OpenOrder("eth", "ETH/USD", Side::kBuy, "1")};
    Build();
    ASSERT_OK(gate->ResetDailyBaseline());
    gate->NoteReferencePrice(kSymbol, D("71"));  // -29
    EXPECT_TRUE(venue->orders.empty());

    now += std::chrono::seconds(1);
    gate->NoteReferencePrice(kSymbol, D("70"));  // -30
    EXPECT_EQ(venue->cancels, std::vector<std::string>{"stop"});
    ASSERT_EQ(venue->orders.size(), 1u);
    EXPECT_EQ(venue->orders[0].side, Side::kSell);
    EXPECT_EQ(venue->orders[0].quantity, D("1"));
    EXPECT_FALSE(gate->IsHalted());

    // Not again while it closes, though the venue still shows it held.
    now += std::chrono::seconds(1);
    gate->NoteReferencePrice(kSymbol, D("69"));
    EXPECT_EQ(venue->orders.size(), 1u);

    venue->fills = {FillOf("close", Side::kSell, "1", "69", now)};
    venue->positions.clear();
    venue->open_orders.clear();
    ExpectRejected(Market(Side::kBuy, "1"));
    ASSERT_OK(gate->PlaceOrder(Market(Side::kBuy, "1", "ETH/USD")));
    now += std::chrono::hours(24);
    ASSERT_OK(gate->PlaceOrder(Market(Side::kBuy, "1")));
}

TEST_F(RiskGateTest, AFailedCloseIsTriedAgainAfterAMinute) {
    limits.max_loss_per_position = D("30");
    venue->positions = {HoldingOf("1", "100")};
    venue->reject_orders = true;
    Build();
    ASSERT_OK(gate->ResetDailyBaseline());
    gate->NoteReferencePrice(kSymbol, D("50"));
    EXPECT_EQ(venue->attempts, 1);
    now += std::chrono::seconds(30);
    gate->NoteReferencePrice(kSymbol, D("50"));
    EXPECT_EQ(venue->attempts, 1);
    now += std::chrono::seconds(30);
    venue->reject_orders = false;
    gate->NoteReferencePrice(kSymbol, D("50"));
    EXPECT_EQ(venue->orders.size(), 1u);
}

TEST_F(RiskGateTest, ACloseIsRoundedDownToTheLot) {
    limits.max_loss_per_position = D("1");
    Build();
    types::SymbolInfo info;
    info.symbol = kSymbol;
    info.lot_size = D("0.01");
    gate->SetSymbolInfo([info](const types::Symbol&) {
        return types::Result<types::SymbolInfo>(info);
    });
    ASSERT_OK(gate->ResetDailyBaseline());
    now += std::chrono::seconds(1);
    // Bought 1, less a fee of 0.005 in the coin.
    types::Fill fill = FillOf("f1", Side::kBuy, "1", "100", now);
    fill.fee = D("0.005");
    fill.fee_asset = "BTC";
    venue->fills = {fill};
    venue->balances = {BalanceOf("BTC", "0.995")};
    gate->NoteReferencePrice(kSymbol, D("90"));
    ASSERT_EQ(venue->orders.size(), 1u);
    EXPECT_EQ(venue->orders[0].quantity, D("0.99"));
}

TEST_F(RiskGateTest, WithShortsAPositionLossBuysTheShortBack) {
    limits.shorts = true;
    limits.max_loss_per_position = D("30");
    venue->positions = {HoldingOf("-1", "100")};
    Build();
    ASSERT_OK(gate->ResetDailyBaseline());
    gate->NoteReferencePrice(kSymbol, D("131"));
    ASSERT_EQ(venue->orders.size(), 1u);
    EXPECT_EQ(venue->orders[0].side, Side::kBuy);
    EXPECT_EQ(venue->orders[0].quantity, D("1"));
}

TEST_F(RiskGateTest, StaleQuotesBlockEntriesButNotExits) {
    limits.max_quote_age = std::chrono::minutes(1);
    venue->positions = {PositionOf("1")};
    Build();
    gate->SetQuotesStale(kSymbol, true);
    ExpectRejected(Market(Side::kBuy, "1"));
    ASSERT_OK(gate->PlaceOrder(Market(Side::kBuy, "1", "ETH/USD")));
    ASSERT_OK(gate->PlaceOrder(Market(Side::kSell, "1")));
    gate->SetQuotesStale(kSymbol, false);
    ASSERT_OK(gate->PlaceOrder(Market(Side::kBuy, "1")));
}

}  // namespace
