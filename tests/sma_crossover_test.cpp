#include "kestrel/strategy/sma_crossover.hpp"

#include "strategy_fakes.hpp"
#include "test_helpers.hpp"

namespace {

using namespace kestrel;
using kestrel::testing::D;
using kestrel::testing::RecordingVenue;
using strategy::SmaCrossover;
using strategy::SmaCrossoverParams;
using types::Side;
using types::Timeframe;

class FakeContext : public strategy::Context {
   public:
    types::Timestamp Now() const override { return types::FromEpochSeconds(0); }
    trading::TradingVenue& Venue() override { return venue; }
    types::Result<types::SymbolInfo> SymbolInfo(const types::Symbol&) override {
        return types::MakeError(types::ErrorCategory::kNotFound, "none");
    }
    std::span<const types::Bar> Bars(const types::Symbol&,
                                     Timeframe) const override {
        return bars;
    }

    // Appends a bar closing at `close` and returns it.
    const types::Bar& Add(const char* close) {
        types::Bar bar;
        bar.open_time = types::FromEpochSeconds(3600 * bars.size());
        bar.open = bar.high = bar.low = bar.close = D(close);
        bars.push_back(bar);
        return bars.back();
    }

    RecordingVenue venue;
    std::vector<types::Bar> bars;
};

SmaCrossoverParams Params() {
    SmaCrossoverParams p;
    p.symbol = "BTC/USD";
    p.timeframe = Timeframe::kH1;
    p.fast_period = 2;
    p.slow_period = 3;
    p.quantity = D("0.5");
    return p;
}

// Adds a bar and runs the strategy's hook on it.
void Feed(SmaCrossover& strategy, FakeContext& context, const char* close,
          const types::Symbol& symbol = "BTC/USD") {
    const types::Bar& bar = context.Add(close);
    strategy.OnBar(context, symbol, Timeframe::kH1, bar);
}

types::Fill FillOf(Side side) {
    types::Fill fill;
    fill.symbol = "BTC/USD";
    fill.side = side;
    return fill;
}

TEST(SmaCrossoverTest, RejectsBadParameters) {
    auto p = Params();
    p.fast_period = 0;
    EXPECT_THROW(SmaCrossover{p}, std::invalid_argument);
    p = Params();
    p.fast_period = 3;  // not below slow_period
    EXPECT_THROW(SmaCrossover{p}, std::invalid_argument);
    p = Params();
    p.quantity = D("0");
    EXPECT_THROW(SmaCrossover{p}, std::invalid_argument);
}

TEST(SmaCrossoverTest, AsksForOneSeriesWithEnoughWarmup) {
    SmaCrossover strategy(Params());
    auto needs = strategy.Needs();
    EXPECT_EQ(needs.symbols, std::vector<types::Symbol>{"BTC/USD"});
    EXPECT_EQ(needs.bar_timeframes, std::vector<Timeframe>{Timeframe::kH1});
    EXPECT_FALSE(needs.quotes);
    EXPECT_EQ(needs.warmup_bars, 4u);  // slow period + 1
}

TEST(SmaCrossoverTest, BuysOnUpCrossAndSellsOnDownCross) {
    SmaCrossover strategy(Params());
    FakeContext context;
    for (const char* close : {"10", "10", "10", "10"}) {
        Feed(strategy, context, close);  // flat: averages are equal
    }
    EXPECT_EQ(context.venue.attempts, 0);

    Feed(strategy, context, "20");  // fast 15 crosses above slow 13.33
    ASSERT_EQ(context.venue.orders.size(), 1u);
    EXPECT_EQ(context.venue.orders[0].side, Side::kBuy);
    EXPECT_EQ(context.venue.orders[0].symbol, "BTC/USD");
    EXPECT_EQ(context.venue.orders[0].type, types::OrderType::kMarket);
    EXPECT_EQ(context.venue.orders[0].quantity, D("0.5"));

    strategy.OnFill(context, FillOf(Side::kBuy));

    Feed(strategy, context, "20");  // still above: no second buy
    EXPECT_EQ(context.venue.orders.size(), 1u);

    Feed(strategy, context, "5");  // fast 12.5 crosses below slow 15
    ASSERT_EQ(context.venue.orders.size(), 2u);
    EXPECT_EQ(context.venue.orders[1].side, Side::kSell);
}

TEST(SmaCrossoverTest, PlacesNothingWhileAnOrderAwaitsItsFill) {
    SmaCrossover strategy(Params());
    FakeContext context;
    for (const char* close : {"10", "10", "10", "10", "20"}) {
        Feed(strategy, context, close);
    }
    ASSERT_EQ(context.venue.orders.size(), 1u);

    // No fill yet: the down cross must not trigger a sell.
    Feed(strategy, context, "20");
    Feed(strategy, context, "5");
    EXPECT_EQ(context.venue.orders.size(), 1u);
}

TEST(SmaCrossoverTest, DoesNothingBeforeTheWindowIsFull) {
    SmaCrossover strategy(Params());
    FakeContext context;
    Feed(strategy, context, "10");
    Feed(strategy, context, "10");
    Feed(strategy, context, "50");  // 3 bars, one short of slow period + 1
    EXPECT_EQ(context.venue.attempts, 0);
}

TEST(SmaCrossoverTest, IgnoresOtherSymbols) {
    SmaCrossover strategy(Params());
    FakeContext context;
    for (const char* close : {"10", "10", "10", "10", "20"}) {
        Feed(strategy, context, close, "ETH/USD");
    }
    EXPECT_EQ(context.venue.attempts, 0);
}

TEST(SmaCrossoverTest, RejectedBuyLeavesItFlat) {
    SmaCrossover strategy(Params());
    FakeContext context;
    context.venue.reject_orders = true;
    for (const char* close : {"10", "10", "10", "10", "20"}) {
        Feed(strategy, context, close);
    }
    EXPECT_EQ(context.venue.attempts, 1);

    Feed(strategy, context, "20");
    Feed(strategy, context, "5");  // would be a sell if it thought it was long
    EXPECT_EQ(context.venue.attempts, 1);
}

TEST(SmaCrossoverTest, OrderClosedWithoutAFillDoesNotLeaveItWaiting) {
    SmaCrossover strategy(Params());
    FakeContext context;
    context.venue.ack_state = types::OrderState::kExpired;
    for (const char* close : {"10", "10", "10", "10", "20"}) {
        Feed(strategy, context, close);
    }
    ASSERT_EQ(context.venue.orders.size(), 1u);

    // Not pending and still flat: the next up cross tries the buy again.
    for (const char* close : {"10", "10", "10", "30"}) {
        Feed(strategy, context, close);
    }
    ASSERT_EQ(context.venue.orders.size(), 2u);
    EXPECT_EQ(context.venue.orders[1].side, Side::kBuy);
}

}  // namespace
