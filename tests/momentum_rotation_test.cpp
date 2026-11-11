#include "kestrel/strategy/momentum_rotation.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "kestrel/backtest/backtest.hpp"
#include "kestrel/backtest/replay_data_source.hpp"
#include "strategy_fakes.hpp"
#include "test_helpers.hpp"

namespace kestrel::strategy {
namespace {

using backtest::ReplayDataSource;
using kestrel::testing::D;
using types::Side;
using types::Timeframe;

types::Timestamp Day(int d) {
    return types::FromEpochSeconds(std::int64_t{d} * 86400);
}

// Daily bars from closes, day 0 first, each opening at the close before it
// (as on a market that never closes); a null close leaves the day out.
std::vector<types::Bar> Daily(const std::vector<const char*>& closes) {
    std::vector<types::Bar> bars;
    for (std::size_t i = 0; i < closes.size(); ++i) {
        if (!closes[i]) {
            continue;
        }
        types::Bar bar;
        bar.open_time = Day(static_cast<int>(i));
        bar.close = D(closes[i]);
        bar.open = bars.empty() ? bar.close : bars.back().close;
        bar.high = std::max(bar.open, bar.close);
        bar.low = std::min(bar.open, bar.close);
        bars.push_back(bar);
    }
    return bars;
}

struct Series {
    const char* symbol;
    std::vector<const char*> closes;
};

std::shared_ptr<ReplayDataSource> Data(const std::vector<Series>& series) {
    auto data = std::make_shared<ReplayDataSource>();
    for (const auto& s : series) {
        data->AddBars(s.symbol, Timeframe::kD1, Daily(s.closes));
        data->SetSymbolInfo({s.symbol, D("0.01"), D("0.001"), D("0.001"), {}});
    }
    return data;
}

MomentumRotationParams Params() {
    MomentumRotationParams p;
    p.symbols = {"A/USD", "B/USD", "C/USD"};
    p.lookback_bars = 2;
    p.rebalance_bars = 1;
    p.top_n = 1;
    return p;
}

struct Trade {
    std::string symbol;
    Side side;
    types::Decimal price;
};

std::vector<Trade> Rotate(const std::shared_ptr<ReplayDataSource>& data,
                          MomentumRotationParams params, int end_day = 7) {
    backtest::BacktestOptions o;
    o.start = Day(3);
    o.end = Day(end_day);
    o.paper.initial_balances = {{"USD", D("1000")}};
    o.paper.slippage = D("0");
    o.paper.maker_fee_rate = D("0");
    o.paper.taker_fee_rate = D("0");
    auto result = backtest::RunBacktest(
        data, std::make_unique<MomentumRotation>(std::move(params)), o);
    EXPECT_TRUE(result.has_value()) << result.error().ToString();
    std::vector<Trade> trades;
    if (result) {
        for (const auto& fill : result->fills) {
            trades.push_back({fill.symbol, fill.side, fill.price});
        }
    }
    return trades;
}

// A rises steadily; B is flat, then jumps on day 4; C falls.
std::vector<Series> Rotating() {
    return {{"A/USD", {"10", "11", "12", "13", "14", "13", "12"}},
            {"B/USD", {"10", "10", "10", "10", "15", "18", "20"}},
            {"C/USD", {"10", "9", "8", "7", "6", "5", "4"}}};
}

// Day 3 ranks A first (13 / 11): bought at day 4's open, 13. Day 4 ranks B
// (15 / 10) over A: A is sold at day 5's open (14), and B bought once that
// sell is done, at its price then (15).
TEST(MomentumRotation, HoldsTheStrongestAndRotatesWhenItChanges) {
    const auto trades = Rotate(Data(Rotating()), Params());
    ASSERT_EQ(trades.size(), 3u);
    EXPECT_EQ(trades[0].symbol, "A/USD");
    EXPECT_EQ(trades[0].side, Side::kBuy);
    EXPECT_EQ(trades[0].price, D("13"));
    EXPECT_EQ(trades[1].symbol, "A/USD");
    EXPECT_EQ(trades[1].side, Side::kSell);
    EXPECT_EQ(trades[1].price, D("14"));
    EXPECT_EQ(trades[2].symbol, "B/USD");
    EXPECT_EQ(trades[2].side, Side::kBuy);
    EXPECT_EQ(trades[2].price, D("15"));
}

TEST(MomentumRotation, SitsInCashWhenNothingRose) {
    const auto trades =
        Rotate(Data({{"A/USD", {"10", "9", "8", "7", "6", "5"}},
                     {"B/USD", {"10", "10", "10", "10", "10", "10"}},
                     {"C/USD", {"10", "9", "8", "7", "6", "5"}}}),
               Params(), 6);
    EXPECT_TRUE(trades.empty());
}

TEST(MomentumRotation, WithoutPositiveOnlyHoldsTheLeastBad) {
    auto params = Params();
    params.positive_only = false;
    const auto trades =
        Rotate(Data({{"A/USD", {"10", "9", "8", "7", "6", "5"}},
                     {"B/USD", {"10", "10", "10", "10", "10", "10"}},
                     {"C/USD", {"10", "9", "8", "7", "6", "5"}}}),
               params, 6);
    ASSERT_FALSE(trades.empty());
    EXPECT_EQ(trades[0].symbol, "B/USD");
}

// Rebalancing every 3 bars: day 3 buys A; days 4 and 5 are not rebalance
// days, so B's jump on day 4 is acted on only on day 6.
TEST(MomentumRotation, RebalancesOnlyEveryRebalanceBars) {
    auto params = Params();
    params.rebalance_bars = 3;
    const auto trades = Rotate(Data(Rotating()), params);
    ASSERT_EQ(trades.size(), 1u);  // day 6's rebalance trades after the end
    EXPECT_EQ(trades[0].symbol, "A/USD");
}

// C has no bar for day 3: the period is ranked when day 4's bars arrive,
// from the day 3 bars of A and B, and A is bought.
TEST(MomentumRotation, RanksAPeriodWithABarMissingOnTheNext) {
    auto series = Rotating();
    series[2].closes[3] = nullptr;
    const auto trades = Rotate(Data(series), Params(), 6);
    ASSERT_FALSE(trades.empty());
    EXPECT_EQ(trades[0].symbol, "A/USD");
    EXPECT_EQ(trades[0].side, Side::kBuy);
}

// Two slots of equity * 0.95 / 2 each: 475 USD at day 4's open.
TEST(MomentumRotation, SplitsEquityOverTheSlots) {
    auto params = Params();
    params.top_n = 2;
    params.positive_only = false;
    backtest::BacktestOptions o;
    o.start = Day(3);
    o.end = Day(5);
    o.paper.initial_balances = {{"USD", D("1000")}};
    o.paper.slippage = D("0");
    o.paper.maker_fee_rate = D("0");
    o.paper.taker_fee_rate = D("0");
    auto result = backtest::RunBacktest(
        Data(Rotating()), std::make_unique<MomentumRotation>(params), o);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->fills.size(), 2u);
    for (const auto& fill : result->fills) {
        // Sized at day 3's close, filled at day 4's open.
        EXPECT_LE(fill.quantity * fill.price, D("550"));
        EXPECT_GE(fill.quantity * fill.price, D("400"));
    }
}

// A context over fixed daily bars and a RecordingVenue, for what a backtest
// cannot show: fees in the base asset, lost updates, fills after updates.
class FakeContext : public Context {
   public:
    types::Timestamp Now() const override { return Day(day); }
    trading::TradingVenue& Venue() override { return venue; }
    types::Result<types::SymbolInfo> SymbolInfo(
        const types::Symbol& symbol) override {
        return types::SymbolInfo{symbol, D("0.01"), D("0.001"), D("0.01"), {}};
    }
    std::span<const types::Bar> Bars(const types::Symbol& symbol,
                                     Timeframe) const override {
        // Up to the current day, as the runner's window would hold.
        const auto it = bars.find(symbol);
        if (it == bars.end()) {
            return {};
        }
        std::size_t n = 0;
        while (n < it->second.size() && it->second[n].open_time <= Day(day)) {
            ++n;
        }
        return std::span<const types::Bar>(it->second.data(), n);
    }
    // Bars for each symbol from closes; a null close leaves the day out.
    void Set(const std::vector<Series>& series) {
        for (const auto& s : series) {
            bars[s.symbol] = Daily(s.closes);
        }
    }
    // Delivers each symbol's bar for `d` (those it has).
    void Close(MomentumRotation& strategy, int d) {
        day = d;
        for (const auto& [symbol, series] : bars) {
            for (const auto& bar : series) {
                if (bar.open_time == Day(d)) {
                    strategy.OnBar(*this, symbol, Timeframe::kD1, bar);
                }
            }
        }
    }
    void Cash(const char* usd) {
        venue.balances = {{"USD", D(usd), D(usd), D("0")}};
    }

    kestrel::testing::RecordingVenue venue;
    std::map<types::Symbol, std::vector<types::Bar>> bars;
    int day = 0;
};

types::Fill FillOf(const std::string& order_id, const char* symbol, Side side,
                   const char* quantity, const char* price,
                   const char* fee = "0", const char* fee_asset = "USD") {
    types::Fill fill;
    fill.order_id = order_id;
    fill.symbol = symbol;
    fill.side = side;
    fill.quantity = D(quantity);
    fill.price = D(price);
    fill.fee = D(fee);
    fill.fee_asset = fee_asset;
    return fill;
}

types::OrderStatus Filled(const std::string& order_id, const char* symbol) {
    types::OrderStatus status;
    status.order_id = order_id;
    status.symbol = symbol;
    status.state = types::OrderState::kFilled;
    return status;
}

// Days 0-6 of Rotating(), A first on days 2 and 3, B first from day 4.
struct Live {
    MomentumRotation strategy{Params()};
    FakeContext context;
    Live() {
        context.Set(Rotating());
        context.Cash("1000");
    }
    // Day 2 ranks A first: one market buy.
    const types::OrderRequest& BuyA() {
        context.Close(strategy, 2);
        EXPECT_EQ(context.venue.orders.size(), 1u);
        return context.venue.orders.back();
    }
};

// Binance.US takes a buy's fee in the coin: what is sold is what is left.
TEST(MomentumRotation, SellsWhatIsLeftAfterAFeeInTheCoin) {
    Live live;
    live.BuyA();
    live.strategy.OnFill(live.context, FillOf("order-1", "A/USD", Side::kBuy,
                                              "10", "12", "0.01", "A"));
    live.strategy.OnOrderUpdate(live.context, Filled("order-1", "A/USD"));
    live.context.Close(live.strategy, 4);  // B overtakes A
    ASSERT_EQ(live.context.venue.orders.size(), 2u);
    const auto& sell = live.context.venue.orders[1];
    EXPECT_EQ(sell.symbol, "A/USD");
    EXPECT_EQ(sell.side, Side::kSell);
    EXPECT_EQ(sell.quantity, D("9.99"));
}

// B is bought only once A's sell has filled and its fill has been seen,
// even when the sell's closing update comes before the fill.
TEST(MomentumRotation, BuysOnlyOnceTheSellsFillHasBeenSeen) {
    Live live;
    live.BuyA();
    live.strategy.OnFill(live.context,
                         FillOf("order-1", "A/USD", Side::kBuy, "10", "12"));
    live.strategy.OnOrderUpdate(live.context, Filled("order-1", "A/USD"));
    live.context.Close(live.strategy, 4);
    ASSERT_EQ(live.context.venue.orders.size(), 2u);  // the sell of A
    live.strategy.OnOrderUpdate(live.context, Filled("order-2", "A/USD"));
    EXPECT_EQ(live.context.venue.orders.size(), 2u);  // no buy yet
    live.context.Cash("1140");
    live.strategy.OnFill(live.context,
                         FillOf("order-2", "A/USD", Side::kSell, "10", "14"));
    ASSERT_EQ(live.context.venue.orders.size(), 3u);
    const auto& buy = live.context.venue.orders[2];
    EXPECT_EQ(buy.symbol, "B/USD");
    // Equity 1140 (cash only now) * 0.95 at B's close of 15.
    EXPECT_EQ(buy.quantity, D("72.2"));
}

// A held symbol without a bar for the ranked period is kept, not sold.
TEST(MomentumRotation, KeepsAHoldingThatCouldNotBeRanked) {
    auto series = Rotating();
    series[0].closes[4] = nullptr;  // A has no day 4 or 5: days 4 and 5
    series[0].closes[5] = nullptr;  // are ranked late without it, and day 6
    Live live;                      // has no bar 2 days before it
    live.context.Set(series);
    live.BuyA();
    live.strategy.OnFill(live.context,
                         FillOf("order-1", "A/USD", Side::kBuy, "10", "12"));
    live.strategy.OnOrderUpdate(live.context, Filled("order-1", "A/USD"));
    for (int d = 4; d <= 6; ++d) {
        live.context.Close(live.strategy, d);
    }
    // B ranks first each time, but A, unranked, keeps the one slot.
    EXPECT_EQ(live.context.venue.orders.size(), 1u);
}

// Dust left after a sell does not count as held: the symbol can be bought
// again.
TEST(MomentumRotation, DustDoesNotBlockBuyingAgain) {
    Live live;
    live.BuyA();
    live.strategy.OnFill(live.context,
                         FillOf("order-1", "A/USD", Side::kBuy, "10", "12"));
    live.strategy.OnOrderUpdate(live.context, Filled("order-1", "A/USD"));
    live.context.Close(live.strategy, 4);  // sell A for B
    live.strategy.OnFill(
        live.context, FillOf("order-2", "A/USD", Side::kSell, "9.995", "14"));
    live.strategy.OnOrderUpdate(live.context, Filled("order-2", "A/USD"));
    // 0.005 A is left, under the 0.01 minimum: B is bought, as if A were
    // gone.
    ASSERT_EQ(live.context.venue.orders.size(), 3u);
    EXPECT_EQ(live.context.venue.orders[2].symbol, "B/USD");
}

// An order whose closing update never came is checked with the venue, so
// the next rebalance is not skipped forever.
TEST(MomentumRotation, AsksTheVenueAboutAnOrderThatNeverClosed) {
    Live live;
    live.BuyA();  // order-1: no fill or update reaches the strategy
    live.context.venue.order_states["order-1"] = Filled("order-1", "A/USD");
    live.context.Close(live.strategy, 4);  // B first; A was never seen held
    ASSERT_EQ(live.context.venue.orders.size(), 2u);
    EXPECT_EQ(live.context.venue.orders[1].symbol, "B/USD");
    EXPECT_EQ(live.context.venue.orders[1].side, Side::kBuy);
}

// The venue's status of an order closed unnoticed counts what it filled;
// that order's fills, arriving later, are not counted again.
TEST(MomentumRotation, AFillOfAnOrderFoundClosedIsNotCountedTwice) {
    Live live;
    live.BuyA();
    auto status = Filled("order-1", "A/USD");
    status.filled_quantity = D("10");
    live.context.venue.order_states["order-1"] = status;
    live.context.Close(live.strategy, 4);  // B first: A, counted, is sold
    ASSERT_EQ(live.context.venue.orders.size(), 2u);
    EXPECT_EQ(live.context.venue.orders[1].symbol, "A/USD");
    EXPECT_EQ(live.context.venue.orders[1].side, Side::kSell);
    EXPECT_EQ(live.context.venue.orders[1].quantity, D("10"));

    live.strategy.OnFill(live.context,
                         FillOf("order-1", "A/USD", Side::kBuy, "10", "12"));
    live.strategy.OnOrderUpdate(live.context, Filled("order-2", "A/USD"));
    live.strategy.OnFill(live.context,
                         FillOf("order-2", "A/USD", Side::kSell, "10", "14"));
    // Flat in A, so B is bought.
    ASSERT_EQ(live.context.venue.orders.size(), 3u);
    EXPECT_EQ(live.context.venue.orders[2].symbol, "B/USD");
}

TEST(MomentumRotation, RefusesBadParameters) {
    auto bad = Params();
    bad.symbols = {"A/USD"};
    EXPECT_THROW(MomentumRotation{bad}, std::invalid_argument);
    bad = Params();
    bad.symbols = {"A/USD", "B/USDT"};
    EXPECT_THROW(MomentumRotation{bad}, std::invalid_argument);
    bad = Params();
    bad.top_n = 4;
    EXPECT_THROW(MomentumRotation{bad}, std::invalid_argument);
    bad = Params();
    bad.lookback_bars = 0;
    EXPECT_THROW(MomentumRotation{bad}, std::invalid_argument);
    bad = Params();
    bad.invest_fraction = D("1.5");
    EXPECT_THROW(MomentumRotation{bad}, std::invalid_argument);
    bad = Params();
    bad.symbols = {"A/USD", "B/USD", "A/USD"};
    EXPECT_THROW(MomentumRotation{bad}, std::invalid_argument);
    bad = Params();
    bad.timeframe = Timeframe::kMo1;
    EXPECT_THROW(MomentumRotation{bad}, std::invalid_argument);
}

}  // namespace
}  // namespace kestrel::strategy
