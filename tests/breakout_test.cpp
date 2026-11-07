#include "kestrel/strategy/breakout.hpp"

#include <gtest/gtest.h>

#include <array>
#include <memory>
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

constexpr const char* kSymbol = "BTC/USD";

types::Timestamp Hour(int h) { return types::FromEpochSeconds(h * 3600); }

// Hourly bars from {open, high, low, close}, hour 0 first.
std::shared_ptr<ReplayDataSource> Bars(
    const std::vector<std::array<const char*, 4>>& bars) {
    auto data = std::make_shared<ReplayDataSource>();
    std::vector<types::Bar> series;
    for (std::size_t i = 0; i < bars.size(); ++i) {
        types::Bar bar;
        bar.open_time = Hour(static_cast<int>(i));
        bar.open = D(bars[i][0]);
        bar.high = D(bars[i][1]);
        bar.low = D(bars[i][2]);
        bar.close = D(bars[i][3]);
        series.push_back(bar);
    }
    data->AddBars(kSymbol, Timeframe::kH1, std::move(series));
    data->SetSymbolInfo({kSymbol, D("0.01"), D("0.0001"), D("0.0001"), {}});
    return data;
}

// Hours 0-3 have ranges 4, 3, 5 and 4; hour 4 (range 2, the first bar
// replayed) is an NR4 with high 13 and low 11: trigger 13.01, stop 10.98
// (11 less 10 bps, rounded down), height 2. Then `after`, from hour 5.
std::shared_ptr<ReplayDataSource> Nr4Then(
    std::vector<std::array<const char*, 4>> after) {
    std::vector<std::array<const char*, 4>> bars = {
        {"12", "14", "10", "12"}, {"12", "13", "10", "12"},
        {"12", "15", "10", "12"}, {"12", "14", "10", "12"},
        {"12", "13", "11", "12"},
    };
    bars.insert(bars.end(), after.begin(), after.end());
    return Bars(bars);
}

BreakoutParams Params() {
    BreakoutParams p;
    p.symbol = kSymbol;
    p.quantity = D("1");
    p.timeframe = Timeframe::kH1;
    return p;
}

backtest::BacktestOptions Options(int end_hour) {
    backtest::BacktestOptions o;
    o.start = Hour(4);
    o.end = Hour(end_hour);
    o.paper.slippage = D("0");
    o.paper.maker_fee_rate = D("0");
    o.paper.taker_fee_rate = D("0");
    return o;
}

struct Trade {
    Side side;
    types::Decimal price;
};
std::vector<Trade> Backtest(std::shared_ptr<ReplayDataSource> data,
                            BreakoutParams params, int end_hour = 8) {
    auto result = backtest::RunBacktest(
        data, std::make_unique<Breakout>(std::move(params)), Options(end_hour));
    EXPECT_TRUE(result.has_value()) << result.error().ToString();
    std::vector<Trade> trades;
    if (result) {
        for (const auto& fill : result->fills) {
            trades.push_back({fill.side, fill.price});
        }
    }
    return trades;
}

TEST(Breakout, EntersOnTheBreakAndTakesProfitAtTheTarget) {
    // Hour 5 breaks 13.01 inside the bar; hour 6 closes at 15.2, past the
    // target 13.01 + 2 = 15.01: sold at the next price, hour 7's open.
    const auto trades = Backtest(Nr4Then({{"12.5", "14", "12.4", "13.8"},
                                          {"13.8", "15.5", "13.5", "15.2"},
                                          {"15", "15", "15", "15"}}),
                                 Params());
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].side, Side::kBuy);
    EXPECT_EQ(trades[0].price, D("13.01"));
    EXPECT_EQ(trades[1].side, Side::kSell);
    EXPECT_EQ(trades[1].price, D("15"));
}

TEST(Breakout, TheProtectiveStopSellsBelowTheSetup) {
    // In at 13.01 in hour 5; hour 6 falls through the stop at 10.98.
    const auto trades = Backtest(Nr4Then({{"12.5", "14", "12.4", "13.8"},
                                          {"13", "13.2", "10.5", "11"},
                                          {"11", "11", "11", "11"}}),
                                 Params());
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[1].side, Side::kSell);
    EXPECT_EQ(trades[1].price, D("10.98"));
}

TEST(Breakout, AnUntriggeredEntryExpires) {
    // Hour 5 never reaches 13.01 (and, wide, is no new setup); hour 6 would
    // have, but the entry was cancelled at hour 5's close.
    const auto trades = Backtest(Nr4Then({{"12", "12.9", "9", "12"},
                                          {"12", "14", "11.5", "13.5"},
                                          {"13", "13", "13", "13"}}),
                                 Params());
    EXPECT_TRUE(trades.empty());
}

TEST(Breakout, ConfirmingClosesBuyAtMarketAfterThem) {
    auto params = Params();
    params.confirm = BreakoutParams::Confirm::kCloses;
    params.confirm_closes = 2;
    // Closes above 13.01 in hours 5 and 6: bought at hour 6's close, 13.6.
    const auto trades = Backtest(Nr4Then({{"12.5", "13.6", "12.4", "13.5"},
                                          {"13.5", "13.8", "13.2", "13.6"},
                                          {"13.6", "13.6", "13.6", "13.6"}}),
                                 params);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].side, Side::kBuy);
    EXPECT_EQ(trades[0].price, D("13.6"));
}

TEST(Breakout, APercentConfirmationRaisesTheTrigger) {
    auto params = Params();
    params.confirm = BreakoutParams::Confirm::kPercent;
    params.confirm_percent = D("5");  // 13 * 1.05 = 13.65
    const auto trades = Backtest(Nr4Then({{"12.5", "14", "12.4", "13.8"},
                                          {"13.8", "13.8", "13.8", "13.8"}}),
                                 params);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].price, D("13.65"));
}

TEST(Breakout, TheTrendFilterKeepsItOutBelowTheAverage) {
    auto params = Params();
    params.trend_sma = 3;  // the setup bar closes at 12, the average is 12
    const auto trades = Backtest(Nr4Then({{"12.5", "14", "12.4", "13.8"},
                                          {"13.8", "13.8", "13.8", "13.8"}}),
                                 params);
    EXPECT_TRUE(trades.empty());
}

TEST(Breakout, ExitsAfterMaxHoldBars) {
    auto params = Params();
    params.target_multiple = D("0");
    params.max_hold_bars = 1;
    const auto trades = Backtest(Nr4Then({{"12.5", "14", "12.4", "13.8"},
                                          {"13.8", "14", "13.5", "13.9"},
                                          {"14", "14", "14", "14"}}),
                                 params);
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[1].side, Side::kSell);
    EXPECT_EQ(trades[1].price, D("14"));  // after hour 6: hour 7's open
}

TEST(Breakout, RangeSetupBreaksTheLastBarsHigh) {
    auto params = Params();
    params.setup = BreakoutParams::Setup::kRange;
    params.range_bars = 3;
    params.target_multiple = D("0");
    // Hours 2-4's high is 15: the buy-stop sits at 15.01.
    const auto trades = Backtest(Nr4Then({{"12.5", "15.5", "12.4", "15.2"},
                                          {"15.2", "15.2", "15.2", "15.2"}}),
                                 params);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].price, D("15.01"));
}

// A market open from 04:30 to 06:30 UTC on 1970-01-01 only.
class Session : public types::TradingCalendar {
   public:
    std::optional<types::TradingDay> On(
        std::chrono::year_month_day date) const override {
        if (date != std::chrono::year_month_day{std::chrono::sys_days{}}) {
            return std::nullopt;
        }
        const auto open = Hour(4) + std::chrono::minutes(30);
        const auto close = Hour(6) + std::chrono::minutes(30);
        return types::TradingDay{open, open, close, close};
    }
    std::chrono::year_month_day DateAt(types::Timestamp t) const override {
        return std::chrono::year_month_day{
            std::chrono::floor<std::chrono::days>(t)};
    }
};

TEST(Breakout, FlattensBeforeTheCloseWhenAsked) {
    auto params = Params();
    params.flatten_at_close = true;
    params.target_multiple = D("0");
    auto data = Nr4Then({{"12.5", "14", "12.4", "13.8"},
                         {"13.8", "14", "13.5", "13.9"},
                         {"14", "14", "14", "14"}});
    data->SetHours(std::make_shared<types::MarketHours>(
        std::make_shared<Session>(), false));
    auto result = backtest::RunBacktest(
        data, std::make_unique<Breakout>(params), Options(8));
    ASSERT_OK(result);
    ASSERT_EQ(result->fills.size(), 2u);
    EXPECT_EQ(result->fills[1].side, Side::kSell);
    // At the closing hook, 5 minutes before 06:30.
    EXPECT_EQ(result->fills[1].time, Hour(6) + std::chrono::minutes(25));
    // Hour 6 is walked only to the 06:30 close, a leg every 10 minutes:
    // 13.8 at 06:00, 14 at 06:10, 13.5 at 06:20, so the hook sells at 13.5.
    EXPECT_EQ(result->fills[1].price, D("13.5"));
}

// A market open from 04:30 to 06:30 UTC every day.
class DailySession : public types::TradingCalendar {
   public:
    std::optional<types::TradingDay> On(
        std::chrono::year_month_day date) const override {
        const types::Timestamp day{std::chrono::sys_days{date}};
        const auto open = day + std::chrono::minutes(4 * 60 + 30);
        const auto close = day + std::chrono::minutes(6 * 60 + 30);
        return types::TradingDay{open, open, close, close};
    }
    std::chrono::year_month_day DateAt(types::Timestamp t) const override {
        return std::chrono::year_month_day{
            std::chrono::floor<std::chrono::days>(t)};
    }
};

// Hour 6's bar reaches the target but closes at 07:00, after the market:
// the exit waits (the stop resting overnight), is called for at the close of
// the next session's first bar, hour 28's (04:00 to 05:00 the next day), and
// sells at hour 29's open.
TEST(Breakout, ATargetReachedAfterTheCloseExitsInTheNextSession) {
    std::vector<std::array<const char*, 4>> after = {
        {"12.5", "14", "12.4", "13.8"}, {"13.8", "15.5", "13.5", "15.2"}};
    for (int h = 7; h <= 29; ++h) {
        after.push_back({"15.3", "15.3", "15.3", "15.3"});
    }
    auto data = Nr4Then(after);
    data->SetHours(std::make_shared<types::MarketHours>(
        std::make_shared<DailySession>(), false));
    auto result = backtest::RunBacktest(
        data, std::make_unique<Breakout>(Params()), Options(30));
    ASSERT_OK(result);
    ASSERT_EQ(result->fills.size(), 2u);
    EXPECT_EQ(result->fills[0].price, D("13.01"));
    EXPECT_EQ(result->fills[1].side, Side::kSell);
    EXPECT_EQ(result->fills[1].time, Hour(29));
    EXPECT_EQ(result->fills[1].price, D("15.3"));
}

BreakoutParams Throwback() {
    auto params = Params();
    params.entry = BreakoutParams::Entry::kThrowback;
    params.throwback_bars = 2;
    return params;
}

TEST(Breakout, AThrowbackBuysTheBrokenLevelAsAMaker) {
    // Hour 5 closes at 13.8, above the trigger 13.01: a limit at the setup's
    // high, 13. Hour 6 comes back to 12.9: bought at 13, a maker. Hour 7
    // closes at 15.2, past the target 13 + 2 = 15: sold at hour 8's open.
    auto result = backtest::RunBacktest(
        Nr4Then({{"12.5", "14", "12.4", "13.8"},
                 {"13.8", "14.2", "12.9", "14"},
                 {"14", "15.5", "14", "15.2"},
                 {"15.1", "15.1", "15.1", "15.1"}}),
        std::make_unique<Breakout>(Throwback()), Options(9));
    ASSERT_TRUE(result.has_value()) << result.error().ToString();
    ASSERT_EQ(result->fills.size(), 2u);
    EXPECT_EQ(result->fills[0].side, Side::kBuy);
    EXPECT_EQ(result->fills[0].price, D("13"));
    EXPECT_EQ(result->fills[0].liquidity, types::Liquidity::kMaker);
    EXPECT_EQ(result->fills[1].side, Side::kSell);
    EXPECT_EQ(result->fills[1].price, D("15.1"));
}

TEST(Breakout, ABreakoutThatNeverLooksBackIsMissed) {
    // The limit at 13 rests through hours 6 and 7, which stay above it, and
    // is cancelled; hour 8 comes back to 13 too late.
    const auto trades = Backtest(Nr4Then({{"12.5", "14", "12.4", "13.8"},
                                          {"13.8", "15", "13.5", "14.5"},
                                          {"14.5", "15", "14.2", "14.8"},
                                          {"14.8", "14.8", "13", "13.5"}}),
                                 Throwback(), 9);
    EXPECT_TRUE(trades.empty());
}

TEST(Breakout, AThrowbackNeedsACloseAboveTheTrigger) {
    // Hour 5 trades through 13.01 but closes back below it: no breakout,
    // so nothing rests for hour 6's dip to 13.
    const auto trades = Backtest(Nr4Then({{"12.5", "14", "12.4", "12.8"},
                                          {"12.8", "13.5", "12.5", "13.2"},
                                          {"13.2", "13.5", "13", "13.4"}}),
                                 Throwback());
    EXPECT_TRUE(trades.empty());
}

// Drives the strategy directly against a RecordingVenue, for the venue
// answers a paper venue never gives: partial fills, failed cancels, fills
// crossing cancels.
class FakeContext : public Context {
   public:
    FakeContext() {
        // The NR4 setup of Nr4Then: trigger 13.01, stop 10.98, height 2.
        for (const auto& b : std::vector<std::array<const char*, 4>>{
                 {"12", "14", "10", "12"},
                 {"12", "13", "10", "12"},
                 {"12", "15", "10", "12"},
                 {"12", "14", "10", "12"},
                 {"12", "13", "11", "12"}}) {
            Add(b);
        }
    }
    types::Timestamp Now() const override { return bars.back().open_time; }
    trading::TradingVenue& Venue() override { return venue; }
    types::Result<types::SymbolInfo> SymbolInfo(const types::Symbol&) override {
        return types::SymbolInfo{
            kSymbol, D("0.01"), D("0.0001"), D("0.0001"), {}};
    }
    std::span<const types::Bar> Bars(const types::Symbol&,
                                     Timeframe) const override {
        return bars;
    }
    const types::Bar& Add(std::array<const char*, 4> b) {
        types::Bar bar;
        bar.open_time = Hour(static_cast<int>(bars.size()));
        bar.open = D(b[0]);
        bar.high = D(b[1]);
        bar.low = D(b[2]);
        bar.close = D(b[3]);
        bars.push_back(bar);
        return bars.back();
    }

    kestrel::testing::RecordingVenue venue;
    std::vector<types::Bar> bars;
};

// Feeds the newest bar, or `next` added first.
void Feed(Breakout& strategy, FakeContext& context) {
    strategy.OnBar(context, kSymbol, Timeframe::kH1, context.bars.back());
}
void Feed(Breakout& strategy, FakeContext& context,
          std::array<const char*, 4> next) {
    strategy.OnBar(context, kSymbol, Timeframe::kH1, context.Add(next));
}

types::Fill FillOf(const char* order_id, Side side, const char* quantity,
                   const char* price) {
    types::Fill fill;
    fill.order_id = order_id;
    fill.symbol = kSymbol;
    fill.side = side;
    fill.quantity = D(quantity);
    fill.price = D(price);
    fill.time = Hour(5);
    return fill;
}

constexpr std::array<const char*, 4> kQuietBar = {"12", "12.5", "11.5", "12"};

// The stop raised to breakeven (13.01) on a close of 15.1 is refused (as a
// venue refuses a sell-stop above the price): sold at market instead.
TEST(Breakout, ARefusedRaisedStopSellsAtMarket) {
    auto params = Params();
    params.target_multiple = D("0");
    params.breakeven_r = D("1");
    Breakout strategy(params);
    FakeContext context;
    Feed(strategy, context);  // armed: order-1
    strategy.OnFill(context, FillOf("order-1", Side::kBuy, "1", "13.01"));
    ASSERT_EQ(context.venue.orders.size(), 2u);  // the stop at 10.98
    context.venue.reject_type = types::OrderType::kStop;
    context.venue.cancel_result = trading::CancelOutcome::kCancelled;
    Feed(strategy, context, {"13.8", "15.5", "13.5", "15.1"});
    EXPECT_EQ(context.venue.cancels, std::vector<std::string>{"order-2"});
    ASSERT_EQ(context.venue.orders.size(), 3u);
    EXPECT_EQ(context.venue.orders[2].side, Side::kSell);
    EXPECT_EQ(context.venue.orders[2].type, types::OrderType::kMarket);
}

// A stop that could not be placed again, after a close through its level,
// is a market sell, not a stop the venue would refuse.
TEST(Breakout, AMissingStopAfterACloseThroughItSellsAtMarket) {
    auto params = Params();
    params.target_multiple = D("0");
    Breakout strategy(params);
    FakeContext context;
    Feed(strategy, context);
    context.venue.reject_type = types::OrderType::kStop;
    strategy.OnFill(context, FillOf("order-1", Side::kBuy, "1", "13.01"));
    ASSERT_EQ(context.venue.orders.size(), 1u);  // the stop was refused
    context.venue.reject_type.reset();
    Feed(strategy, context, {"12", "12", "10.5", "10.9"});  // below 10.98
    ASSERT_EQ(context.venue.orders.size(), 2u);
    EXPECT_EQ(context.venue.orders[1].type, types::OrderType::kMarket);
}

TEST(Breakout, BarsWithoutVolumeNeverConfirm) {
    auto params = Params();
    params.confirm = BreakoutParams::Confirm::kCloses;
    params.confirm_closes = 1;
    params.volume_ratio = D("2");
    params.volume_bars = 3;
    Breakout strategy(params);
    FakeContext context;      // no volume anywhere
    Feed(strategy, context);  // the NR4: watching
    Feed(strategy, context, {"12.5", "13.5", "12.4", "13.2"});
    EXPECT_TRUE(context.venue.orders.empty());
}

TEST(Breakout, APartlyFilledEntryThatExpiresIsHeldWithAStop) {
    Breakout strategy(Params());
    FakeContext context;
    Feed(strategy, context);  // armed: order-1
    ASSERT_EQ(context.venue.orders.size(), 1u);
    strategy.OnFill(context, FillOf("order-1", Side::kBuy, "0.4", "13.01"));
    context.venue.cancel_result = trading::CancelOutcome::kAlreadyClosed;
    Feed(strategy, context, kQuietBar);  // expires
    EXPECT_EQ(context.venue.cancels, std::vector<std::string>{"order-1"});
    ASSERT_EQ(context.venue.orders.size(), 2u);
    const auto& stop = context.venue.orders[1];
    EXPECT_EQ(stop.side, Side::kSell);
    EXPECT_EQ(stop.type, types::OrderType::kStop);
    EXPECT_EQ(stop.quantity, D("0.4"));
    EXPECT_EQ(stop.stop_price, D("10.98"));
}

TEST(Breakout, AnEntryClosingPartlyFilledIsHeldWithAStop) {
    Breakout strategy(Params());
    FakeContext context;
    Feed(strategy, context);
    strategy.OnFill(context, FillOf("order-1", Side::kBuy, "0.4", "13.01"));
    types::OrderStatus status;
    status.order_id = "order-1";
    status.state = types::OrderState::kExpired;
    status.filled_quantity = D("0.4");
    strategy.OnOrderUpdate(context, status);
    ASSERT_EQ(context.venue.orders.size(), 2u);
    EXPECT_EQ(context.venue.orders[1].quantity, D("0.4"));
}

TEST(Breakout, AFailedCancelKeepsTheEntryArmedAndTriesAgain) {
    Breakout strategy(Params());
    FakeContext context;
    Feed(strategy, context);
    // RecordingVenue's default: the cancel fails.
    Feed(strategy, context, kQuietBar);
    Feed(strategy, context, kQuietBar);
    EXPECT_EQ(context.venue.cancels,
              (std::vector<std::string>{"order-1", "order-1"}));
    ASSERT_EQ(context.venue.orders.size(), 1u);  // no new setup meanwhile
    // It was live after all: its fill is protected.
    strategy.OnFill(context, FillOf("order-1", Side::kBuy, "1", "13.01"));
    ASSERT_EQ(context.venue.orders.size(), 2u);
    EXPECT_EQ(context.venue.orders[1].type, types::OrderType::kStop);
    EXPECT_EQ(context.venue.orders[1].quantity, D("1"));
}

TEST(Breakout, ALateEntryFillResizesTheStop) {
    Breakout strategy(Params());
    FakeContext context;
    Feed(strategy, context);
    strategy.OnFill(context, FillOf("order-1", Side::kBuy, "0.4", "13.01"));
    context.venue.cancel_result = trading::CancelOutcome::kCancelled;
    Feed(strategy, context, kQuietBar);  // long 0.4, stop order-2
    strategy.OnFill(context, FillOf("order-1", Side::kBuy, "0.6", "13.01"));
    EXPECT_EQ(context.venue.cancels,
              (std::vector<std::string>{"order-1", "order-2"}));
    ASSERT_EQ(context.venue.orders.size(), 3u);
    EXPECT_EQ(context.venue.orders[2].type, types::OrderType::kStop);
    EXPECT_EQ(context.venue.orders[2].quantity, D("1"));
}

TEST(Breakout, TheStopSellingOutCancelsAnExitStillOut) {
    Breakout strategy(Params());
    FakeContext context;
    Feed(strategy, context);
    strategy.OnFill(context, FillOf("order-1", Side::kBuy, "1", "13.01"));
    context.venue.cancel_result = trading::CancelOutcome::kCancelled;
    // Closes past the target 15.01: the stop (order-2) cancelled, the exit
    // (order-3) sent.
    Feed(strategy, context, {"13", "15.5", "13", "15.2"});
    ASSERT_EQ(context.venue.orders.size(), 3u);
    EXPECT_EQ(context.venue.orders[2].type, types::OrderType::kMarket);
    // The stop's cancel had not landed: it sells out first.
    strategy.OnFill(context, FillOf("order-2", Side::kSell, "1", "10.98"));
    EXPECT_EQ(context.venue.cancels,
              (std::vector<std::string>{"order-2", "order-3"}));
    // Flat again: the next setup arms.
    Feed(strategy, context, {"12", "14", "10", "12"});
    Feed(strategy, context, {"12", "13", "10", "12"});
    Feed(strategy, context, {"12", "15", "10", "12"});
    Feed(strategy, context, {"12", "13", "11", "12"});
    EXPECT_EQ(context.venue.orders.size(), 4u);
}

TEST(Breakout, TheThrowbackLimitIsPostOnlyAtTheSetupsHigh) {
    Breakout strategy(Throwback());
    FakeContext context;
    Feed(strategy, context);  // the setup: watching, nothing placed
    EXPECT_TRUE(context.venue.orders.empty());
    Feed(strategy, context, {"12.5", "14", "12.4", "13.8"});
    ASSERT_EQ(context.venue.orders.size(), 1u);
    const auto& limit = context.venue.orders[0];
    EXPECT_EQ(limit.side, Side::kBuy);
    EXPECT_EQ(limit.type, types::OrderType::kLimit);
    EXPECT_EQ(limit.limit_price, D("13"));
    EXPECT_TRUE(limit.post_only);
    EXPECT_EQ(limit.quantity, D("1"));
}

TEST(Breakout, AThrowbackWithConfirmingClosesWaitsForAllOfThem) {
    auto params = Throwback();
    params.confirm = BreakoutParams::Confirm::kCloses;
    params.confirm_closes = 2;
    Breakout strategy(params);
    FakeContext context;
    Feed(strategy, context);
    Feed(strategy, context, {"12.5", "13.6", "12.4", "13.5"});  // one close
    EXPECT_TRUE(context.venue.orders.empty());
    Feed(strategy, context, {"13.5", "13.8", "13.2", "13.6"});  // two
    ASSERT_EQ(context.venue.orders.size(), 1u);
    EXPECT_EQ(context.venue.orders[0].type, types::OrderType::kLimit);
    EXPECT_EQ(context.venue.orders[0].limit_price, D("13"));
    EXPECT_TRUE(context.venue.orders[0].post_only);
}

// A rectangle over 8 bars: high 20, low 10 (zones at 19 and 11, the middle
// 15), touched at the top three times and at the bottom twice.
void RectangleBars(FakeContext& context) {
    context.bars.clear();
    for (const auto& b :
         std::vector<std::array<const char*, 4>>{{"16", "20", "15", "16"},
                                                 {"16", "19.5", "14", "15"},
                                                 {"15", "16", "10", "12"},
                                                 {"12", "18", "12", "17"},
                                                 {"17", "19", "13", "14"},
                                                 {"14", "15", "11", "13"},
                                                 {"13", "17", "12", "16"},
                                                 {"16", "19.2", "16", "18"}}) {
        context.Add(b);
    }
}

BreakoutParams RectangleParams() {
    auto params = Params();
    params.setup = BreakoutParams::Setup::kRectangle;
    params.range_bars = 8;
    return params;
}

TEST(Breakout, ARectangleArmsAboveItsResistance) {
    Breakout strategy(RectangleParams());
    FakeContext context;
    RectangleBars(context);
    Feed(strategy, context);
    ASSERT_EQ(context.venue.orders.size(), 1u);
    EXPECT_EQ(context.venue.orders[0].type, types::OrderType::kStop);
    EXPECT_EQ(context.venue.orders[0].stop_price, D("20.01"));
}

TEST(Breakout, ARectangleNeedsItsTouches) {
    auto params = RectangleParams();
    params.min_touches = 3;  // the bottom has only two
    Breakout strategy(params);
    FakeContext context;
    RectangleBars(context);
    Feed(strategy, context);
    EXPECT_TRUE(context.venue.orders.empty());
}

TEST(Breakout, ARectangleCanRequireAShortfall) {
    auto params = RectangleParams();
    params.range_bars = 9;
    params.shortfall = true;
    const auto orders_after = [&](std::array<const char*, 4> last) {
        Breakout strategy(params);
        FakeContext context;
        RectangleBars(context);
        Feed(strategy, context, last);
        return context.venue.orders;
    };
    // Down to 15.5 after the last top touch: not below the middle.
    EXPECT_TRUE(orders_after({"18", "18", "15.5", "16"}).empty());
    // Down to 14.5, short of the bottom: a shortfall.
    const auto orders = orders_after({"18", "18", "14.5", "16"});
    ASSERT_EQ(orders.size(), 1u);
    EXPECT_EQ(orders[0].stop_price, D("20.01"));
}

TEST(Breakout, APipeBottomBreaksAboveItsSecondBar) {
    auto params = Params();
    params.setup = BreakoutParams::Setup::kPipeBottom;
    params.decline_bars = 3;
    Breakout strategy(params);
    FakeContext context;
    context.bars.clear();
    for (int i = 0; i < 3; ++i) {
        context.Add({"11.5", "12", "11", "11.5"});
    }
    context.Add({"11.5", "11.5", "9", "9.2"});              // down to a new low
    Feed(strategy, context, {"9.2", "11", "9.1", "10.5"});  // and back up
    ASSERT_EQ(context.venue.orders.size(), 1u);
    EXPECT_EQ(context.venue.orders[0].type, types::OrderType::kStop);
    EXPECT_EQ(context.venue.orders[0].stop_price, D("11.01"));
    // Filled: the stop is under the lower low, 9 less 10 bps.
    strategy.OnFill(context, FillOf("order-1", Side::kBuy, "1", "11.01"));
    ASSERT_EQ(context.venue.orders.size(), 2u);
    EXPECT_EQ(context.venue.orders[1].stop_price, D("8.99"));
}

types::Timestamp Day(int d) { return types::FromEpochSeconds(d * 86400); }

// Daily bars, day 0 first, on DailySession (open 04:30 to 06:30 UTC): every
// bar closes at midnight, after the market. Days 0-4 are Nr4Then's setup.
backtest::BacktestResult Daily(std::vector<std::array<const char*, 4>> after,
                               int end_day, BreakoutParams params = Params()) {
    std::vector<std::array<const char*, 4>> bars = {{"12", "14", "10", "12"},
                                                    {"12", "13", "10", "12"},
                                                    {"12", "15", "10", "12"},
                                                    {"12", "14", "10", "12"},
                                                    {"12", "13", "11", "12"}};
    bars.insert(bars.end(), after.begin(), after.end());
    auto data = std::make_shared<ReplayDataSource>();
    std::vector<types::Bar> series;
    for (std::size_t i = 0; i < bars.size(); ++i) {
        types::Bar bar;
        bar.open_time = Day(static_cast<int>(i));
        bar.open = D(bars[i][0]);
        bar.high = D(bars[i][1]);
        bar.low = D(bars[i][2]);
        bar.close = D(bars[i][3]);
        series.push_back(bar);
    }
    data->AddBars(kSymbol, Timeframe::kD1, std::move(series));
    data->SetSymbolInfo({kSymbol, D("0.01"), D("0.0001"), D("0.0001"), {}});
    data->SetHours(std::make_shared<types::MarketHours>(
        std::make_shared<DailySession>(), false));
    params.timeframe = Timeframe::kD1;
    auto options = Options(0);
    options.start = Day(4);
    options.end = Day(end_day);
    auto result = backtest::RunBacktest(
        data, std::make_unique<Breakout>(params), options);
    EXPECT_TRUE(result.has_value()) << result.error().ToString();
    return result ? *result : backtest::BacktestResult{};
}

// Day 4's NR4 closes at midnight: the buy-stop at 13.01 goes in at day 5's
// open, which is above it, so it fills there (13.5; the target 15.5). Day 6
// closes at 15.6, past the target, after the market: sold at day 7's open.
TEST(Breakout, DailyBarsEnterAndExitAtTheNextOpen) {
    const auto result = Daily({{"13.5", "14", "13.4", "13.8"},
                               {"13.8", "15.7", "13.5", "15.6"},
                               {"15.8", "16", "15.7", "15.9"}},
                              8);
    ASSERT_EQ(result.fills.size(), 2u);
    EXPECT_EQ(result.fills[0].side, Side::kBuy);
    EXPECT_EQ(result.fills[0].price, D("13.5"));
    EXPECT_EQ(result.fills[0].time, Day(5) + std::chrono::minutes(270));
    EXPECT_EQ(result.fills[1].side, Side::kSell);
    EXPECT_EQ(result.fills[1].price, D("15.8"));
    EXPECT_EQ(result.fills[1].time, Day(7) + std::chrono::minutes(270));
}

// In at day 5's open, 13.5 (stop 10.98, a risk of 2.52); day 6 closes at
// 16.1, a risk above, after the market: the stop moves to 13.5 at day 7's
// open, and day 7 falls through it.
TEST(Breakout, ADailyBreakevenStopMovesAtTheNextOpen) {
    auto params = Params();
    params.target_multiple = D("0");
    params.breakeven_r = D("1");
    const auto result = Daily({{"13.5", "14", "13.4", "13.8"},
                               {"13.8", "16.5", "13.5", "16.1"},
                               {"15", "15.2", "13", "13.2"}},
                              8, params);
    ASSERT_EQ(result.fills.size(), 2u);
    EXPECT_EQ(result.fills[1].side, Side::kSell);
    EXPECT_EQ(result.fills[1].price, D("13.5"));
    EXPECT_GE(result.fills[1].time, Day(7));
}

TEST(Breakout, ADailyEntryLastsOneSession) {
    // Day 5 stays under 13.01 (and, wide, is no new setup); the entry is
    // cancelled at its close, so day 6's rise through it buys nothing.
    const auto result =
        Daily({{"12.5", "12.9", "9", "10"}, {"10", "14", "9.8", "13.9"}}, 7);
    EXPECT_TRUE(result.fills.empty());
}

TEST(Breakout, ADailyEntryWithArmBarsLeftIsPlacedAgainAtTheNextOpen) {
    // Cancelled at day 5's close untriggered, placed again at day 6's open
    // (one of its two bars left), and filled on the way up through 13.01.
    auto params = Params();
    params.arm_bars = 2;
    const auto result = Daily(
        {{"12.5", "12.9", "9", "10"}, {"12.5", "14", "12", "13.5"}}, 7, params);
    ASSERT_EQ(result.fills.size(), 1u);
    EXPECT_EQ(result.fills[0].price, D("13.01"));
    EXPECT_GE(result.fills[0].time, Day(6));
}

TEST(Breakout, DailyConfirmingClosesBuyAtTheNextOpen) {
    // Days 5 and 6 close above 13.01; the watch spans the closes between,
    // and the market buy goes at day 7's open.
    auto params = Params();
    params.confirm = BreakoutParams::Confirm::kCloses;
    params.confirm_closes = 2;
    const auto result = Daily({{"12.5", "13.5", "12.4", "13.2"},
                               {"13.2", "13.8", "13.1", "13.6"},
                               {"14.2", "14.5", "14", "14.3"}},
                              8, params);
    ASSERT_EQ(result.fills.size(), 1u);
    EXPECT_EQ(result.fills[0].side, Side::kBuy);
    EXPECT_EQ(result.fills[0].price, D("14.2"));
    EXPECT_EQ(result.fills[0].time, Day(7) + std::chrono::minutes(270));
}

TEST(Breakout, ADailyThrowbackRestsFromTheNextOpen) {
    // Day 5 closes above 13.01: the limit at 13 goes in at day 6's open
    // (13.6, above it) and fills as a maker on day 6's dip to 12.9.
    auto params = Params();
    params.entry = BreakoutParams::Entry::kThrowback;
    const auto result = Daily(
        {{"12.5", "14", "12.4", "13.8"}, {"13.6", "13.9", "12.9", "13.5"}}, 7,
        params);
    ASSERT_EQ(result.fills.size(), 1u);
    EXPECT_EQ(result.fills[0].price, D("13"));
    EXPECT_EQ(result.fills[0].liquidity, types::Liquidity::kMaker);
}

TEST(Breakout, AFailedBreakdownBuysAboveTheBarBackInside) {
    auto params = Params();
    params.setup = BreakoutParams::Setup::kFailedBreakdown;
    params.range_bars = 4;
    params.reclaim_bars = 2;
    Breakout strategy(params);
    FakeContext context;
    context.bars.clear();
    for (int i = 0; i < 4; ++i) {
        context.Add({"11", "12", "10", "11"});  // the range: 10 to 12
    }
    context.Add({"10.5", "10.5", "9.5", "9.6"});  // closes below 10
    context.Add({"9.6", "10.2", "9.4", "9.8"});   // the false low, 9.4
    Feed(strategy, context, {"9.8", "10.6", "9.9", "10.4"});  // back inside
    ASSERT_EQ(context.venue.orders.size(), 1u);
    EXPECT_EQ(context.venue.orders[0].type, types::OrderType::kStop);
    EXPECT_EQ(context.venue.orders[0].stop_price, D("10.61"));
    // Filled: the stop is under the false low, 9.4 less 10 bps.
    strategy.OnFill(context, FillOf("order-1", Side::kBuy, "1", "10.61"));
    ASSERT_EQ(context.venue.orders.size(), 2u);
    EXPECT_EQ(context.venue.orders[1].stop_price, D("9.39"));
}

TEST(Breakout, ADoubleBottomBuysAboveItsNeckline) {
    auto params = Params();
    params.setup = BreakoutParams::Setup::kMultipleBottom;
    params.range_bars = 8;
    params.swing_bars = 1;
    Breakout strategy(params);
    FakeContext context;
    context.bars.clear();
    for (const auto& b : std::vector<std::array<const char*, 4>>{
             {"13", "14", "12", "13"},
             {"12", "12", "10", "11"},  // the first low, 10
             {"11", "13", "11", "12.5"},
             {"12.5", "15", "13", "14"},  // the neckline, 15
             {"14", "14", "12", "12.5"},
             {"12", "11.5", "10.2", "11"},  // the second low, 10.2
             {"11", "12.5", "11", "12"}}) {
        context.Add(b);
    }
    Feed(strategy, context, {"12", "13", "11.5", "12.8"});
    ASSERT_EQ(context.venue.orders.size(), 1u);
    EXPECT_EQ(context.venue.orders[0].type, types::OrderType::kStop);
    EXPECT_EQ(context.venue.orders[0].stop_price, D("15.01"));
    // Filled: the stop is under the lowest low, 10 less 10 bps.
    strategy.OnFill(context, FillOf("order-1", Side::kBuy, "1", "15.01"));
    ASSERT_EQ(context.venue.orders.size(), 2u);
    EXPECT_EQ(context.venue.orders[1].stop_price, D("9.99"));
}

// A market open from 04:00 to 08:00 UTC every day, so hourly bars start at
// the open.
class MorningSession : public types::TradingCalendar {
   public:
    std::optional<types::TradingDay> On(
        std::chrono::year_month_day date) const override {
        const types::Timestamp day{std::chrono::sys_days{date}};
        const auto open = day + std::chrono::hours(4);
        const auto close = day + std::chrono::hours(8);
        return types::TradingDay{open, open, close, close};
    }
    std::chrono::year_month_day DateAt(types::Timestamp t) const override {
        return std::chrono::year_month_day{
            std::chrono::floor<std::chrono::days>(t)};
    }
};

BreakoutParams OpeningRange() {
    auto params = Params();
    params.setup = BreakoutParams::Setup::kOpeningRange;
    params.opening_bars = 2;
    return params;
}

// Hours 0-3 are before the open and wider than anything after; hours 4 and 5
// are the opening range, high 12.5 and low 10: trigger 12.51, stop 9.99.
// Then `after`, from hour 6.
std::shared_ptr<ReplayDataSource> OpeningThen(
    std::vector<std::array<const char*, 4>> after) {
    std::vector<std::array<const char*, 4>> bars = {
        {"12", "20", "5", "12"},    {"12", "20", "5", "12"},
        {"12", "20", "5", "12"},    {"12", "20", "5", "12"},
        {"11", "12", "10", "11.5"}, {"11.5", "12.5", "10.5", "12"},
    };
    bars.insert(bars.end(), after.begin(), after.end());
    auto data = Bars(bars);
    data->SetHours(std::make_shared<types::MarketHours>(
        std::make_shared<MorningSession>(), false));
    return data;
}

TEST(Breakout, AnOpeningRangeBreakoutBuysAboveTheFirstBarsAndFlattens) {
    auto params = OpeningRange();
    params.arm_bars = 2;
    params.target_multiple = D("0");
    params.flatten_at_close = true;
    // Hour 6 stays inside; hour 7 breaks 12.51, the entry still armed.
    auto result =
        backtest::RunBacktest(OpeningThen({{"12", "12.4", "11", "12.2"},
                                           {"12.2", "13", "12.1", "12.8"},
                                           {"12.8", "12.8", "12.8", "12.8"}}),
                              std::make_unique<Breakout>(params), Options(9));
    ASSERT_OK(result);
    ASSERT_EQ(result->fills.size(), 2u);
    EXPECT_EQ(result->fills[0].side, Side::kBuy);
    EXPECT_EQ(result->fills[0].price, D("12.51"));
    // Sold in the closing hook, 5 minutes before 08:00.
    EXPECT_EQ(result->fills[1].side, Side::kSell);
    EXPECT_EQ(result->fills[1].time, Hour(7) + std::chrono::minutes(55));
}

TEST(Breakout, AnOpeningRangeIsASetupOnceASession) {
    // The entry armed at 06:00 lasts one bar and hour 6 stays inside; hour
    // 7's break is not bought, since no later bar is a setup.
    auto result = backtest::RunBacktest(
        OpeningThen({{"12", "12.4", "11", "12.2"},
                     {"12.2", "13", "12.1", "12.8"},
                     {"12.8", "12.8", "12.8", "12.8"}}),
        std::make_unique<Breakout>(OpeningRange()), Options(9));
    ASSERT_OK(result);
    EXPECT_TRUE(result->fills.empty());
}

TEST(Breakout, EachSessionHasItsOwnOpeningRange) {
    // Day one's range (hours 4 and 5) is never broken. Day two's (hours 28
    // and 29) ranges 20 to 21, and hour 30 breaks 21.01.
    std::vector<std::array<const char*, 4>> after(
        22, {"11", "11", "11", "11"});  // hours 6-27
    after.push_back({"20", "21", "20", "20.5"});
    after.push_back({"20.5", "20.8", "20.2", "20.5"});
    after.push_back({"20.5", "22", "20.4", "21.8"});
    auto result = backtest::RunBacktest(
        OpeningThen(after), std::make_unique<Breakout>(OpeningRange()),
        Options(31));
    ASSERT_OK(result);
    ASSERT_EQ(result->fills.size(), 1u);
    EXPECT_EQ(result->fills[0].price, D("21.01"));
    EXPECT_GE(result->fills[0].time, Hour(30));
}

TEST(Breakout, AnOpeningRangeOnAMarketThatNeverClosesIsTheUtcDays) {
    // Hours 0-23 are flat at 12 (the backtest starts at hour 4); the day's
    // first two bars (hours 24 and 25) range 11.5 to 12.5, and hour 26
    // breaks 12.51.
    std::vector<std::array<const char*, 4>> bars(24, {"12", "12", "12", "12"});
    bars.push_back({"12", "12.5", "11.5", "12"});
    bars.push_back({"12", "12.2", "11.8", "12"});
    bars.push_back({"12", "13", "12", "12.9"});
    auto result = backtest::RunBacktest(
        Bars(bars), std::make_unique<Breakout>(OpeningRange()), Options(27));
    ASSERT_OK(result);
    ASSERT_EQ(result->fills.size(), 1u);
    EXPECT_EQ(result->fills[0].price, D("12.51"));
    EXPECT_GE(result->fills[0].time, Hour(26));
}

// In at 13.01 in hour 5 (stop 10.98: a risk of 2.03); hour 6 closes at
// 15.1, more than one risk above the entry; hour 7 falls back to 12.
std::shared_ptr<ReplayDataSource> UpThenBack() {
    return Nr4Then({{"12.5", "14", "12.4", "13.8"},
                    {"13.8", "15.5", "13.5", "15.1"},
                    {"15", "15", "12", "12.5"}});
}

BreakoutParams NoTarget() {
    auto p = Params();
    p.target_multiple = D("0");
    return p;
}

TEST(Breakout, WithoutAMovingStopTheFirstStopStays) {
    const auto trades = Backtest(UpThenBack(), NoTarget());
    ASSERT_EQ(trades.size(), 1u);  // 12 never reaches 10.98
}

TEST(Breakout, BreakevenRaisesTheStopToTheEntry) {
    auto params = NoTarget();
    params.breakeven_r = D("1");  // 15.1 >= 13.01 + 2.03
    const auto trades = Backtest(UpThenBack(), params);
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[1].side, Side::kSell);
    EXPECT_EQ(trades[1].price, D("13.01"));
}

TEST(Breakout, BreakevenWaitsForTheMultipleOfTheRisk) {
    auto params = NoTarget();
    params.breakeven_r = D("1.5");  // 13.01 + 3.045 = 16.055: not reached
    EXPECT_EQ(Backtest(UpThenBack(), params).size(), 1u);
}

TEST(Breakout, ATrailingStopFollowsTheLowestRecentLow) {
    auto params = NoTarget();
    // Hours 5 and 6: lowest low 12.4, less 10 bps: 12.38.
    params.trail_bars = 2;
    const auto trades = Backtest(UpThenBack(), params);
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[1].price, D("12.38"));
}

TEST(Breakout, TheHigherOfBreakevenAndTheTrailWins) {
    auto params = NoTarget();
    params.trail_bars = 2;
    params.breakeven_r = D("1");
    const auto trades = Backtest(UpThenBack(), params);
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[1].price, D("13.01"));
}

// Hourly bars with volume: {open, high, low, close, volume}.
std::shared_ptr<ReplayDataSource> VolumeBars(
    const std::vector<std::array<const char*, 5>>& bars) {
    auto data = std::make_shared<ReplayDataSource>();
    std::vector<types::Bar> series;
    for (std::size_t i = 0; i < bars.size(); ++i) {
        types::Bar bar;
        bar.open_time = Hour(static_cast<int>(i));
        bar.open = D(bars[i][0]);
        bar.high = D(bars[i][1]);
        bar.low = D(bars[i][2]);
        bar.close = D(bars[i][3]);
        bar.volume = D(bars[i][4]);
        series.push_back(bar);
    }
    data->AddBars(kSymbol, Timeframe::kH1, std::move(series));
    data->SetSymbolInfo({kSymbol, D("0.01"), D("0.0001"), D("0.0001"), {}});
    return data;
}

// The NR4 of Nr4Then on 10 a bar; hour 5 closes above 13.01 on 15, hour 6 on
// 30 (the three before average 11.67: twice that is 23.33).
std::shared_ptr<ReplayDataSource> BreakoutVolumes() {
    return VolumeBars({{"12", "14", "10", "12", "10"},
                       {"12", "13", "10", "12", "10"},
                       {"12", "15", "10", "12", "10"},
                       {"12", "14", "10", "12", "10"},
                       {"12", "13", "11", "12", "10"},
                       {"12.5", "13.5", "12.4", "13.2", "15"},
                       {"13.2", "13.6", "13.1", "13.5", "30"},
                       {"13.6", "13.8", "13.4", "13.7", "10"}});
}

BreakoutParams OnVolume() {
    auto p = Params();
    p.confirm = BreakoutParams::Confirm::kCloses;
    p.confirm_closes = 1;
    p.arm_bars = 3;
    p.volume_ratio = D("2");
    p.volume_bars = 3;
    return p;
}

TEST(Breakout, WithoutVolumeTheFirstCloseAboveConfirms) {
    auto params = OnVolume();
    params.volume_ratio = D("0");
    const auto trades = Backtest(BreakoutVolumes(), params);
    ASSERT_GE(trades.size(), 1u);
    EXPECT_EQ(trades[0].price, D("13.2"));  // hour 6's open
}

TEST(Breakout, AVolumeRatioWaitsForACloseOnEnoughVolume) {
    const auto trades = Backtest(BreakoutVolumes(), OnVolume());
    ASSERT_GE(trades.size(), 1u);
    EXPECT_EQ(trades[0].side, Side::kBuy);
    EXPECT_EQ(trades[0].price, D("13.6"));  // hour 7's open
}

TEST(Breakout, ACloseOnLowVolumeIsNotABreakout) {
    auto params = OnVolume();
    params.arm_bars = 1;  // only hour 5's close could confirm
    EXPECT_TRUE(Backtest(BreakoutVolumes(), params).empty());
}

TEST(Breakout, RefusesBadParameters) {
    auto bad = Params();
    bad.quantity = D("0");
    EXPECT_THROW(Breakout{bad}, std::invalid_argument);
    bad = Params();
    bad.nr_bars = 1;
    EXPECT_THROW(Breakout{bad}, std::invalid_argument);
    bad = Params();
    bad.target_multiple = D("-1");
    EXPECT_THROW(Breakout{bad}, std::invalid_argument);
    bad = Params();
    bad.arm_bars = 0;
    EXPECT_THROW(Breakout{bad}, std::invalid_argument);
    bad = Throwback();
    bad.throwback_bars = 0;
    EXPECT_THROW(Breakout{bad}, std::invalid_argument);
    bad = Params();
    bad.bottoms = 4;
    EXPECT_THROW(Breakout{bad}, std::invalid_argument);
    bad = Params();
    bad.swing_bars = 0;
    EXPECT_THROW(Breakout{bad}, std::invalid_argument);
    bad = Params();
    bad.setup = BreakoutParams::Setup::kMultipleBottom;
    bad.range_bars = 7;  // a double at swing_bars 2 needs 8
    EXPECT_THROW(Breakout{bad}, std::invalid_argument);
    bad.range_bars = 8;
    EXPECT_NO_THROW(Breakout{bad});
    bad = Params();
    bad.reclaim_bars = 0;
    EXPECT_THROW(Breakout{bad}, std::invalid_argument);
    bad = Params();
    bad.decline_bars = 0;
    EXPECT_THROW(Breakout{bad}, std::invalid_argument);
    bad = RectangleParams();
    bad.min_touches = 0;
    EXPECT_THROW(Breakout{bad}, std::invalid_argument);
    bad = RectangleParams();
    bad.touch_zone = D("0");
    EXPECT_THROW(Breakout{bad}, std::invalid_argument);
    bad = OpeningRange();
    bad.opening_bars = 0;
    EXPECT_THROW(Breakout{bad}, std::invalid_argument);
    bad = OpeningRange();
    bad.timeframe = Timeframe::kD1;
    EXPECT_THROW(Breakout{bad}, std::invalid_argument);
    bad = Params();
    bad.volume_ratio = D("2");  // a buy-stop entry
    EXPECT_THROW(Breakout{bad}, std::invalid_argument);
    bad.confirm = BreakoutParams::Confirm::kCloses;
    EXPECT_NO_THROW(Breakout{bad});
    bad.volume_bars = 0;
    EXPECT_THROW(Breakout{bad}, std::invalid_argument);
    bad = Params();
    bad.breakeven_r = D("-1");
    EXPECT_THROW(Breakout{bad}, std::invalid_argument);
}

}  // namespace
}  // namespace kestrel::strategy
