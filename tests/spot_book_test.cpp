#include "kestrel/strategy/spot_book.hpp"

#include <gtest/gtest.h>

#include <map>
#include <span>
#include <string>
#include <vector>

#include "strategy_fakes.hpp"
#include "test_helpers.hpp"

namespace kestrel::strategy {
namespace {

using kestrel::testing::D;
using types::Side;
using types::Timeframe;

types::Timestamp Day(int d) {
    return types::FromEpochSeconds(std::int64_t{d} * 86400);
}

// Each period is complete once every symbol's bar is in, or late, once a
// bar of the next arrives; bars of a period already complete are ignored.
// A symbol that missed a period is not waited for again until it sends a
// bar, so a halted symbol holds up only the first period it misses.
TEST(PeriodGate, CompletesAPeriodOnceEverySymbolsBarIsIn) {
    PeriodGate gate(2);
    EXPECT_TRUE(gate.Add("A/USD", Day(1)).empty());
    EXPECT_EQ(gate.Add("B/USD", Day(1)), std::vector{Day(1)});
    EXPECT_TRUE(gate.Add("B/USD", Day(1)).empty());  // again: ignored
    EXPECT_TRUE(gate.Add("A/USD", Day(0)).empty());  // older: ignored
    // B's bar for day 2 never comes: day 2 completes on A's bar of day 3,
    // and day 3 with it, as B is no longer waited for.
    EXPECT_TRUE(gate.Add("A/USD", Day(2)).empty());
    EXPECT_EQ(gate.Add("A/USD", Day(3)), (std::vector{Day(2), Day(3)}));
    EXPECT_TRUE(gate.Add("B/USD", Day(2)).empty());  // too late
    // B is back on day 3, after it completed: waited for from day 4.
    EXPECT_TRUE(gate.Add("B/USD", Day(3)).empty());
    EXPECT_TRUE(gate.Add("A/USD", Day(4)).empty());
    EXPECT_EQ(gate.Add("B/USD", Day(4)), std::vector{Day(4)});
    // Halted for good: only its first missing day is late.
    EXPECT_TRUE(gate.Add("A/USD", Day(5)).empty());
    EXPECT_EQ(gate.Add("A/USD", Day(6)), (std::vector{Day(5), Day(6)}));
    EXPECT_EQ(gate.Add("A/USD", Day(7)), std::vector{Day(7)});
    PeriodGate one(1);
    EXPECT_EQ(one.Add("A/USD", Day(5)), std::vector{Day(5)});
    EXPECT_EQ(one.Add("A/USD", Day(6)), std::vector{Day(6)});
}

class BookContext : public Context {
   public:
    types::Timestamp Now() const override { return Day(1); }
    trading::TradingVenue& Venue() override { return venue; }
    types::Result<types::SymbolInfo> SymbolInfo(
        const types::Symbol& symbol) override {
        return types::SymbolInfo{symbol, D("0.01"), D("0.1"), D("0.1"), D("5")};
    }
    std::span<const types::Bar> Bars(const types::Symbol& symbol,
                                     Timeframe) const override {
        const auto it = bars.find(symbol);
        return it == bars.end() ? std::span<const types::Bar>()
                                : std::span<const types::Bar>(it->second);
    }
    void Price(const types::Symbol& symbol, const char* close) {
        types::Bar bar;
        bar.open_time = Day(1);
        bar.open = bar.high = bar.low = bar.close = D(close);
        bars[symbol] = {bar};
    }
    kestrel::testing::RecordingVenue venue;
    std::map<types::Symbol, std::vector<types::Bar>> bars;
};

types::Fill FillOf(const char* id, Side side, const char* quantity,
                   const char* fee = "0", const char* fee_asset = "USD") {
    types::Fill fill;
    fill.order_id = id;
    fill.symbol = "A/USD";
    fill.side = side;
    fill.quantity = D(quantity);
    fill.price = D("10");
    fill.fee = D(fee);
    fill.fee_asset = fee_asset;
    return fill;
}

// Holdings count what was bought less a fee in the coin; what can be sold is
// that to the lot, and nothing under the minimum order or notional (5 USD).
TEST(SpotBook, CountsFillsAndSellsWhatIsLeft) {
    BookContext context;
    context.Price("A/USD", "10");
    context.venue.balances = {{"USD", D("100"), D("90"), D("10")}};
    SpotBook book({"A/USD", "B/USD"}, Timeframe::kD1);
    EXPECT_EQ(book.Quote(), "USD");
    book.OnFill(FillOf("o1", Side::kBuy, "2", "0.01", "A"));
    EXPECT_EQ(book.Held("A/USD"), D("1.99"));
    EXPECT_EQ(book.Sellable(context, "A/USD"), D("1.9"));
    EXPECT_EQ(*book.Equity(context), D("119.9"));  // 100 + 1.99 x 10
    EXPECT_EQ(*book.FreeCash(context), D("90"));
    book.OnFill(FillOf("o2", Side::kSell, "1.6"));
    EXPECT_EQ(book.Held("A/USD"), D("0.39"));
    EXPECT_EQ(book.Sellable(context, "A/USD"), D("0"));  // 3.9 USD: dust
    EXPECT_THROW(SpotBook({"A/USD", "B/EUR"}, Timeframe::kD1),
                 std::invalid_argument);
    EXPECT_THROW(SpotBook({"A/USD", "A/USD"}, Timeframe::kD1),
                 std::invalid_argument);
    EXPECT_THROW(SpotBook({"A"}, Timeframe::kD1), std::invalid_argument);
}

// An order whose closing update never came is found closed by Reconcile,
// which counts its fill once: the fill arriving later is not counted again.
TEST(SpotBook, ReconcileCountsAnOrderFoundClosedOnce) {
    BookContext context;
    context.Price("A/USD", "10");
    SpotBook book({"A/USD"}, Timeframe::kD1);
    const auto id = book.Place(context, "A/USD", Side::kBuy, D("3"));
    ASSERT_TRUE(id);
    EXPECT_TRUE(book.OrdersOut());
    EXPECT_FALSE(book.SellOut());
    types::OrderStatus status;
    status.order_id = *id;
    status.symbol = "A/USD";
    status.state = types::OrderState::kFilled;
    status.filled_quantity = D("3");
    context.venue.order_states[*id] = status;
    const auto closed = book.Reconcile(context);
    ASSERT_EQ(closed.size(), 1u);
    EXPECT_FALSE(closed[0].sell);
    EXPECT_FALSE(book.OrdersOut());
    EXPECT_EQ(book.Held("A/USD"), D("3"));
    book.OnFill(FillOf(id->c_str(), Side::kBuy, "3"));
    EXPECT_EQ(book.Held("A/USD"), D("3"));
    // An update for an order no longer tracked is not ours.
    EXPECT_FALSE(book.OnOrderUpdate(status));
}

// A market order the venue reports filled at once (the paper venue and
// Binance.US do) is out until its fills total its quantity: its cash has
// moved, but the holdings count it only once the fills come.
TEST(SpotBook, AnOrderFilledAtOnceIsOutUntilItsFillsCome) {
    BookContext context;
    context.Price("A/USD", "10");
    context.venue.ack_state = types::OrderState::kFilled;
    SpotBook book({"A/USD"}, Timeframe::kD1);
    const auto buy = book.Place(context, "A/USD", Side::kBuy, D("3"));
    ASSERT_TRUE(buy);
    EXPECT_TRUE(book.OrdersOut());
    book.OnFill(FillOf(buy->c_str(), Side::kBuy, "1"));
    EXPECT_TRUE(book.OrdersOut());
    book.OnFill(FillOf(buy->c_str(), Side::kBuy, "2"));
    EXPECT_FALSE(book.OrdersOut());
    EXPECT_EQ(book.Held("A/USD"), D("3"));
    // Its closing update after the fills: already dropped.
    types::OrderStatus status;
    status.order_id = *buy;
    status.symbol = "A/USD";
    status.state = types::OrderState::kFilled;
    status.filled_quantity = D("3");
    EXPECT_FALSE(book.OnOrderUpdate(status));
    EXPECT_EQ(book.Held("A/USD"), D("3"));
    // Refused at once: not placed, and not out.
    context.venue.ack_state = types::OrderState::kRejected;
    EXPECT_FALSE(book.Place(context, "A/USD", Side::kBuy, D("1")));
    EXPECT_FALSE(book.OrdersOut());
}

// A closing update that comes before the order's fills counts what it says
// filled; the fills, when they come, are not counted again (a fee in the
// coin still is).
TEST(SpotBook, AnUpdateAheadOfTheFillsCountsThemOnce) {
    BookContext context;
    context.Price("A/USD", "10");
    SpotBook book({"A/USD"}, Timeframe::kD1);
    const auto id = book.Place(context, "A/USD", Side::kBuy, D("3"));
    ASSERT_TRUE(id);
    types::OrderStatus status;
    status.order_id = *id;
    status.symbol = "A/USD";
    status.state = types::OrderState::kFilled;
    status.filled_quantity = D("3");
    const auto done = book.OnOrderUpdate(status);
    ASSERT_TRUE(done);
    EXPECT_TRUE(done->filled);
    EXPECT_FALSE(done->sell);
    EXPECT_FALSE(book.OrdersOut());
    EXPECT_EQ(book.Held("A/USD"), D("3"));
    book.OnFill(FillOf(id->c_str(), Side::kBuy, "3", "0.01", "A"));
    EXPECT_EQ(book.Held("A/USD"), D("2.99"));
    // A sell cancelled part filled, its fill not seen: what filled is sold.
    const auto sell = book.Place(context, "A/USD", Side::kSell, D("2"));
    ASSERT_TRUE(sell);
    status.order_id = *sell;
    status.state = types::OrderState::kCancelled;
    status.filled_quantity = D("1.5");
    const auto cancelled = book.OnOrderUpdate(status);
    ASSERT_TRUE(cancelled);
    EXPECT_FALSE(cancelled->filled);
    EXPECT_TRUE(cancelled->sell);
    EXPECT_EQ(book.Held("A/USD"), D("1.49"));
    book.OnFill(FillOf(sell->c_str(), Side::kSell, "1.5"));
    EXPECT_EQ(book.Held("A/USD"), D("1.49"));
}

}  // namespace
}  // namespace kestrel::strategy
