#include "kestrel/strategy/single_order_position.hpp"

#include "kestrel/strategy/orders.hpp"
#include "strategy_fakes.hpp"
#include "test_helpers.hpp"

namespace {

using namespace kestrel;
using kestrel::testing::D;
using kestrel::testing::RecordingVenue;
using strategy::SingleOrderPosition;
using Change = SingleOrderPosition::Change;
using types::OrderState;
using types::Side;

class FakeContext : public strategy::Context {
   public:
    types::Timestamp Now() const override { return types::FromEpochSeconds(0); }
    trading::TradingVenue& Venue() override { return venue; }
    types::Result<types::SymbolInfo> SymbolInfo(const types::Symbol&) override {
        if (lot.Sign() > 0) {
            return types::SymbolInfo{"BTC/USD", D("0.01"), lot, lot, {}};
        }
        return types::MakeError(types::ErrorCategory::kNotFound, "none");
    }
    std::span<const types::Bar> Bars(const types::Symbol&,
                                     types::Timeframe) const override {
        return {};
    }

    RecordingVenue venue;
    types::Decimal lot;
};

types::Fill Part(Side side, const char* quantity, const char* price = "100",
                 const std::string& order = "order-1") {
    types::Fill fill;
    fill.order_id = order;
    fill.symbol = "BTC/USD";
    fill.side = side;
    fill.quantity = D(quantity);
    fill.price = D(price);
    return fill;
}

types::OrderStatus Closed(const std::string& order, OrderState state,
                          const char* filled) {
    types::OrderStatus status;
    status.order_id = order;
    status.symbol = "BTC/USD";
    status.state = state;
    status.filled_quantity = D(filled);
    return status;
}

SingleOrderPosition Position() {
    return SingleOrderPosition("BTC/USD", D("1"), "test");
}

TEST(SingleOrderPositionTest, AnOrderFilledInOnePieceEntersAndExits) {
    FakeContext context;
    auto position = Position();
    ASSERT_TRUE(position.Buy(context));
    EXPECT_TRUE(position.Busy());
    EXPECT_FALSE(position.Buy(context));  // one order at a time
    EXPECT_EQ(context.venue.orders.size(), 1u);

    EXPECT_EQ(position.OnFill(Part(Side::kBuy, "1")), Change::kEntered);
    EXPECT_FALSE(position.Busy());
    EXPECT_TRUE(position.Long());
    EXPECT_EQ(position.Held(), D("1"));
    EXPECT_EQ(position.EntryPrice(), D("100"));

    ASSERT_TRUE(position.Sell(context));
    EXPECT_EQ(context.venue.orders[1].side, Side::kSell);
    EXPECT_EQ(context.venue.orders[1].quantity, D("1"));
    EXPECT_EQ(position.OnFill(Part(Side::kSell, "1", "110", "order-2")),
              Change::kExited);
    EXPECT_FALSE(position.Long());
    EXPECT_EQ(position.EntryPrice(), D("0"));
}

TEST(SingleOrderPositionTest, AnEntryFilledInPartsIsOneEntryAtTheAveragePrice) {
    FakeContext context;
    auto position = Position();
    ASSERT_TRUE(position.Buy(context));
    EXPECT_EQ(position.OnFill(Part(Side::kBuy, "0.4", "100")), Change::kNone);
    EXPECT_TRUE(position.Busy());
    EXPECT_FALSE(position.Long());
    EXPECT_FALSE(position.Sell(context));  // still buying
    EXPECT_EQ(position.OnFill(Part(Side::kBuy, "0.6", "105")),
              Change::kEntered);
    EXPECT_FALSE(position.Busy());
    EXPECT_EQ(position.Held(), D("1"));
    EXPECT_EQ(position.EntryPrice(), D("103"));
}

TEST(SingleOrderPositionTest, AnExitFilledInPartsStaysBusyUntilTheLastPart) {
    FakeContext context;
    auto position = Position();
    position.Buy(context);
    position.OnFill(Part(Side::kBuy, "1"));
    position.Sell(context);
    EXPECT_EQ(position.OnFill(Part(Side::kSell, "0.3", "100", "order-2")),
              Change::kNone);
    // Not flat yet, and nothing new can be sent: no buy against the rest of
    // the sell.
    EXPECT_TRUE(position.Long());
    EXPECT_TRUE(position.Busy());
    EXPECT_FALSE(position.Buy(context));
    EXPECT_EQ(position.OnFill(Part(Side::kSell, "0.7", "100", "order-2")),
              Change::kExited);
    EXPECT_FALSE(position.Long());
    EXPECT_EQ(position.Held(), D("0"));
}

TEST(SingleOrderPositionTest, AnEntryClosedPartlyFilledHoldsWhatItBought) {
    FakeContext context;
    auto position = Position();
    position.Buy(context);
    // The close can come before its fill, or after.
    EXPECT_EQ(position.OnOrderUpdate(
                  Closed("order-1", OrderState::kCancelled, "0.25")),
              Change::kNone);
    EXPECT_TRUE(position.Busy());
    EXPECT_EQ(position.OnFill(Part(Side::kBuy, "0.25")), Change::kEntered);
    EXPECT_EQ(position.Held(), D("0.25"));

    position.Sell(context);
    EXPECT_EQ(context.venue.orders[1].quantity, D("0.25"));

    auto after = Position();
    after.Buy(context);
    EXPECT_EQ(after.OnFill(Part(Side::kBuy, "0.25", "100", "order-3")),
              Change::kNone);
    EXPECT_EQ(
        after.OnOrderUpdate(Closed("order-3", OrderState::kExpired, "0.25")),
        Change::kEntered);
    EXPECT_FALSE(after.Busy());
}

TEST(SingleOrderPositionTest, ACloseReportingLessThanHasFilledGoesByTheFills) {
    FakeContext context;
    auto position = Position();
    position.Buy(context);
    position.OnFill(Part(Side::kBuy, "0.4"));
    // The venue's cancel says nothing filled, though 0.4 has.
    EXPECT_EQ(
        position.OnOrderUpdate(Closed("order-1", OrderState::kCancelled, "0")),
        Change::kEntered);
    EXPECT_FALSE(position.Busy());
    EXPECT_EQ(position.Held(), D("0.4"));
}

TEST(SingleOrderPositionTest, AnExitClosedPartlyFilledStaysLongForTheRest) {
    FakeContext context;
    auto position = Position();
    position.Buy(context);
    position.OnFill(Part(Side::kBuy, "1"));
    position.Sell(context);
    position.OnFill(Part(Side::kSell, "0.4", "100", "order-2"));
    EXPECT_EQ(position.OnOrderUpdate(
                  Closed("order-2", OrderState::kCancelled, "0.4")),
              Change::kNone);
    EXPECT_FALSE(position.Busy());
    EXPECT_TRUE(position.Long());
    EXPECT_EQ(position.Held(), D("0.6"));

    ASSERT_TRUE(position.Sell(context));
    EXPECT_EQ(context.venue.orders[2].quantity, D("0.6"));
    EXPECT_EQ(position.OnFill(Part(Side::kSell, "0.6", "100", "order-3")),
              Change::kExited);
}

TEST(SingleOrderPositionTest, AnOrderClosedWithoutAFillLeavesItWhereItWas) {
    FakeContext context;
    auto position = Position();
    position.Buy(context);
    EXPECT_EQ(
        position.OnOrderUpdate(Closed("order-1", OrderState::kRejected, "0")),
        Change::kNone);
    EXPECT_FALSE(position.Busy());
    EXPECT_FALSE(position.Long());

    // Only a close counts, and only of its own order.
    position.Buy(context);
    position.OnOrderUpdate(Closed("order-2", OrderState::kNew, "0"));
    position.OnOrderUpdate(Closed("order-9", OrderState::kCancelled, "0"));
    EXPECT_TRUE(position.Busy());
}

TEST(SingleOrderPositionTest, OrdersTheVenueRefusesAreNotWaitedOn) {
    FakeContext context;
    auto position = Position();
    context.venue.reject_orders = true;
    EXPECT_FALSE(position.Buy(context));
    EXPECT_FALSE(position.Busy());

    context.venue.reject_orders = false;
    context.venue.ack_state = OrderState::kExpired;  // closed at once
    EXPECT_FALSE(position.Buy(context));
    EXPECT_FALSE(position.Busy());
}

TEST(SingleOrderPositionTest, ACoinFeeIsOffWhatIsHeldAndTheSellIsInLots) {
    FakeContext context;
    context.lot = D("0.01");
    auto position = Position();
    position.Buy(context);
    auto fill = Part(Side::kBuy, "1");
    fill.fee = D("0.001");
    fill.fee_asset = "BTC";
    position.OnFill(fill);
    EXPECT_EQ(position.Held(), D("0.999"));
    position.Sell(context);
    EXPECT_EQ(context.venue.orders[1].quantity, D("0.99"));
    position.OnFill(Part(Side::kSell, "0.99", "100", "order-2"));
    EXPECT_FALSE(position.Long());
    EXPECT_EQ(position.Held(), D("0.009"));  // goes with the next exit
}

TEST(SingleOrderPositionTest, LessThanALotIsDustAndCountsAsFlat) {
    FakeContext context;
    context.lot = D("0.01");
    SingleOrderPosition position("BTC/USD", D("0.01"), "test");
    position.Buy(context);
    auto fill = Part(Side::kBuy, "0.01");
    fill.fee = D("0.00001");
    fill.fee_asset = "BTC";
    position.OnFill(fill);
    EXPECT_FALSE(position.Sell(context));
    EXPECT_EQ(context.venue.orders.size(), 1u);
    EXPECT_FALSE(position.Long());
    EXPECT_FALSE(position.Busy());
}

TEST(SingleOrderPositionTest, AnOrderTheVenueFilledShortIsDoneAtWhatItFilled) {
    FakeContext context;
    auto position = Position();
    position.Buy(context);
    position.OnFill(Part(Side::kBuy, "0.99"));  // the venue sized it to 0.99
    EXPECT_TRUE(position.Busy());
    // The update can come before the fill, or after.
    EXPECT_EQ(
        position.OnOrderUpdate(Closed("order-1", OrderState::kFilled, "0.99")),
        Change::kEntered);
    EXPECT_FALSE(position.Busy());

    position.Sell(context);
    EXPECT_EQ(
        position.OnOrderUpdate(Closed("order-2", OrderState::kFilled, "0.99")),
        Change::kNone);
    EXPECT_TRUE(position.Busy());
    EXPECT_EQ(position.OnFill(Part(Side::kSell, "0.99", "100", "order-2")),
              Change::kExited);
}

TEST(SingleOrderPositionTest, TheBuyIsInLots) {
    FakeContext context;
    context.lot = D("0.01");
    SingleOrderPosition position("BTC/USD", D("0.129"), "test");
    ASSERT_TRUE(position.Buy(context));
    EXPECT_EQ(context.venue.orders[0].quantity, D("0.12"));

    SingleOrderPosition tiny("BTC/USD", D("0.009"), "test");
    EXPECT_FALSE(tiny.Buy(context));
    EXPECT_EQ(context.venue.orders.size(), 1u);
}

TEST(SingleOrderPositionTest, AnotherOrdersFillsMoveThePositionToo) {
    FakeContext context;
    auto position = Position();
    position.Buy(context);
    position.OnFill(Part(Side::kBuy, "1"));
    // A risk gate closes it at market.
    EXPECT_EQ(position.OnFill(Part(Side::kSell, "1", "90", "gate-1")),
              Change::kExited);
    EXPECT_FALSE(position.Long());
    EXPECT_EQ(position.OnFill(Part(Side::kBuy, "1", "90", "other-1")),
              Change::kEntered);
    EXPECT_TRUE(position.Long());
    EXPECT_FALSE(position.Busy());
}

TEST(SingleOrderPositionTest, AFillWithoutAnOrderIdIsTheWorkingOrders) {
    FakeContext context;
    auto position = Position();
    position.Buy(context);
    EXPECT_EQ(position.OnFill(Part(Side::kBuy, "1", "100", "")),
              Change::kEntered);
    EXPECT_FALSE(position.Busy());

    // Nor a quantity: the rest of the order.
    position.Sell(context);
    EXPECT_EQ(position.OnFill(Part(Side::kSell, "0", "100", "")),
              Change::kExited);
    EXPECT_FALSE(position.Busy());
    EXPECT_EQ(position.Held(), D("0"));
}

TEST(OrderHelpersTest, SplitFeesAndRound) {
    EXPECT_EQ(strategy::BaseAsset("ETH/USDT"), "ETH");
    EXPECT_TRUE(strategy::ClosedUnfilled(OrderState::kCancelled));
    EXPECT_FALSE(strategy::ClosedUnfilled(OrderState::kFilled));
    EXPECT_FALSE(strategy::ClosedUnfilled(OrderState::kPartiallyFilled));

    auto fill = Part(Side::kBuy, "1");
    fill.fee = D("0.002");
    fill.fee_asset = "USD";
    EXPECT_FALSE(strategy::FeeInBase(fill, "BTC"));
    EXPECT_EQ(strategy::ReceivedOnBuy(fill, "BTC"), D("1"));
    fill.fee_asset = "BTC";
    EXPECT_EQ(strategy::ReceivedOnBuy(fill, "BTC"), D("0.998"));
    EXPECT_FALSE(strategy::FeeInBase(fill, ""));

    FakeContext context;
    EXPECT_EQ(strategy::RoundDownToLot(context, "BTC/USD", D("0.129")),
              D("0.129"));  // no lot size
    context.lot = D("0.01");
    EXPECT_EQ(strategy::RoundDownToLot(context, "BTC/USD", D("0.129")),
              D("0.12"));
}

}  // namespace
