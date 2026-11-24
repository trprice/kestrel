#include "kestrel/strategy/breakout.hpp"

#include <gtest/gtest.h>

#include <algorithm>
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

// Bars mirrored about 25 (each price p is 25 - p, a bar's high its low): a
// setup's break down where the bars given break up.
std::shared_ptr<ReplayDataSource> Mirrored(
    const std::vector<std::array<const char*, 4>>& bars) {
    auto data = std::make_shared<ReplayDataSource>();
    std::vector<types::Bar> series;
    const types::Decimal about = D("25");
    for (std::size_t i = 0; i < bars.size(); ++i) {
        types::Bar bar;
        bar.open_time = Hour(static_cast<int>(i));
        bar.open = about - D(bars[i][0]);
        bar.high = about - D(bars[i][2]);
        bar.low = about - D(bars[i][1]);
        bar.close = about - D(bars[i][3]);
        series.push_back(bar);
    }
    data->AddBars(kSymbol, Timeframe::kH1, std::move(series));
    data->SetSymbolInfo({kSymbol, D("0.01"), D("0.0001"), D("0.0001"), {}});
    return data;
}

// Nr4Then upside down: the NR4's high is 14 and its low 12, so a short
// triggers at 11.99 with its stop at 14.02 (14 and 10 bps, rounded up) and
// its target 2 lower than its entry.
std::shared_ptr<ReplayDataSource> MirroredNr4Then(
    std::vector<std::array<const char*, 4>> after) {
    std::vector<std::array<const char*, 4>> bars = {
        {"12", "14", "10", "12"}, {"12", "13", "10", "12"},
        {"12", "15", "10", "12"}, {"12", "14", "10", "12"},
        {"12", "13", "11", "12"},
    };
    bars.insert(bars.end(), after.begin(), after.end());
    return Mirrored(bars);
}

bool gAllowShort = false;  // the backtests' paper venue shorts

backtest::BacktestOptions Options(int end_hour) {
    backtest::BacktestOptions o;
    o.paper.allow_short = gAllowShort;
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

// Fills' quantities, in order.
std::vector<types::Decimal> Quantities(std::shared_ptr<ReplayDataSource> data,
                                       BreakoutParams params) {
    auto result = backtest::RunBacktest(
        data, std::make_unique<Breakout>(std::move(params)), Options(8));
    EXPECT_TRUE(result.has_value()) << result.error().ToString();
    std::vector<types::Decimal> quantities;
    if (result) {
        for (const auto& fill : result->fills) {
            quantities.push_back(fill.quantity);
        }
    }
    return quantities;
}

TEST(Breakout, RiskPerTradeSizesTheEntryFromTheStop) {
    const auto bars = [] {
        return Nr4Then({{"12.5", "14", "12.4", "13.8"},
                        {"13", "13.2", "10.5", "11"},
                        {"11", "11", "11", "11"}});
    };
    // In at 13.01, stop 10.98: 2.03 a unit, so 1 to risk buys 0.4926 (to the
    // lot) and the stop sells the same.
    auto params = Params();
    params.risk_per_trade = D("1");
    EXPECT_EQ(Quantities(bars(), params),
              (std::vector<types::Decimal>{D("0.4926"), D("0.4926")}));
    // 5 would buy 2.46: quantity caps it.
    params.risk_per_trade = D("5");
    EXPECT_EQ(Quantities(bars(), params),
              (std::vector<types::Decimal>{D("1"), D("1")}));
    // Under the venue's minimum: no trade.
    params.risk_per_trade = D("0.0001");
    EXPECT_TRUE(Quantities(bars(), params).empty());
    // A size past what twelve places can hold (more than the account can
    // pay for, so refused) is worked out without overflowing.
    params.quantity = D("1000000000");
    params.risk_per_trade = D("100000000");
    EXPECT_NO_THROW(Quantities(bars(), params));
}

// The bars of RiskPerTradeSizesTheEntryFromTheStop: in at 13.01, stop 10.98,
// 2.03 a unit. The paper account holds 10,000 USD.
std::shared_ptr<ReplayDataSource> RiskBars() {
    return Nr4Then({{"12.5", "14", "12.4", "13.8"},
                    {"13", "13.2", "10.5", "11"},
                    {"11", "11", "11", "11"}});
}

BreakoutParams RiskFractionParams(const char* fraction) {
    auto params = Params();
    params.quantity = D("200");  // high enough not to cap
    params.risk_fraction = D(fraction);
    return params;
}

TEST(Breakout, RiskFractionRisksAShareOfTheBalance) {
    // 0.01 of 10,000 is 100 to risk: 49.261 at 2.03 a unit.
    EXPECT_EQ(Quantities(RiskBars(), RiskFractionParams("0.01")),
              (std::vector<types::Decimal>{D("49.261"), D("49.261")}));
}

TEST(Breakout, RiskFractionRisksAShareOfTheAllocation) {
    // 0.02 of an allocation of 50 is 1 to risk, as risk_per_trade = 1.
    auto params = RiskFractionParams("0.02");
    params.allocation = D("50");
    EXPECT_EQ(Quantities(RiskBars(), params),
              (std::vector<types::Decimal>{D("0.4926"), D("0.4926")}));
}

TEST(Breakout, RiskFractionLeavesRoomForCostsInTheAllocation) {
    // A tight stop: 0.1 of 100 over 0.12 a unit would buy 83.3333, costing
    // far more than the 100 allocated. The entry spends 99 of it instead,
    // leaving room for the fee and slippage: 99 / 12.96 = 7.6388.
    const auto tight = Bars({{"12", "14", "10", "12"},
                             {"12", "13", "10", "12"},
                             {"12", "15", "10", "12"},
                             {"12", "14", "10", "12"},
                             {"12.9", "12.95", "12.86", "12.9"},
                             {"12.9", "13.2", "12.88", "13.1"},
                             {"13.1", "13.1", "13.1", "13.1"}});
    auto params = RiskFractionParams("0.1");
    params.allocation = D("100");
    EXPECT_EQ(Quantities(tight, params),
              (std::vector<types::Decimal>{D("7.6388"), D("7.6388")}));
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

// The backtest fills a stop crossed on the way to the high, or gapped
// through at the open, at the bar's open time: that bar is still the one the
// entry filled inside, so it is not counted, as with a stop crossed later.
TEST(Breakout, MaxHoldBarsSkipsAnEntryBarFilledAtItsOpen) {
    auto params = Params();
    params.target_multiple = D("0");
    params.max_hold_bars = 1;
    for (const std::array<const char*, 4> entry_bar :
         {std::array<const char*, 4>{"12.9", "13.6", "12", "13.2"},     // high
          std::array<const char*, 4>{"13.5", "14", "13.4", "13.8"}}) {  // gap
        const auto trades = Backtest(Nr4Then({entry_bar,
                                              {"13.8", "14", "13.5", "13.9"},
                                              {"14", "14", "14", "14"}}),
                                     params);
        ASSERT_EQ(trades.size(), 2u) << entry_bar[0];
        EXPECT_EQ(trades[1].side, Side::kSell);
        EXPECT_EQ(trades[1].price, D("14")) << entry_bar[0];  // hour 7's open
    }
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

    const LogContext& Log() const override { return log; }
    bool MarketOpen() const override { return open; }
    void TrackOrder(const types::OrderStatus& order) override {
        tracked_orders.push_back(order.order_id);
    }

    kestrel::testing::RecordingVenue venue;
    std::vector<types::Bar> bars;
    std::vector<std::string> tracked_orders;
    bool open = true;
    LogContext log;
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

// Arming is logged: a resting stop entry is otherwise invisible until it
// fills.
TEST(Breakout, LogsTheArmedEntry) {
    std::vector<std::string> lines;
    Breakout strategy(Params());
    FakeContext context;
    context.log = LogContext(
        "breakout", [&](LogContext::Level level, std::string_view line) {
            if (level == LogContext::Level::kInfo) {
                lines.emplace_back(line);
            }
        });
    Feed(strategy, context);
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0],
              "breakout symbol=BTC/USD: armed: long 1 on a stop at 13.01 "
              "(order order-1); stop 10.98, for 1 bar");
}

// So is the way out: a stop resting on the venue fills with nothing else
// said, and a later setup would arm as if no trade had been.
TEST(Breakout, LogsWhichOrderClosedTheTrade) {
    std::vector<std::string> lines;
    Breakout strategy(Params());
    FakeContext context;
    context.log = LogContext(
        "breakout", [&](LogContext::Level level, std::string_view line) {
            if (level == LogContext::Level::kInfo) {
                lines.emplace_back(line);
            }
        });
    Feed(strategy, context);
    strategy.OnFill(context, FillOf("order-1", Side::kBuy, "1", "13.01"));
    // The stop (order-2) sells in two parts: said once, at the last.
    strategy.OnFill(context, FillOf("order-2", Side::kSell, "0.4", "10.98"));
    ASSERT_EQ(lines.size(), 2u);  // armed, long
    strategy.OnFill(context, FillOf("order-2", Side::kSell, "0.6", "10.9"));
    ASSERT_EQ(lines.size(), 3u);
    EXPECT_EQ(lines[2],
              "breakout symbol=BTC/USD: long closed by the protective stop at "
              "10.9; entered at 13.01");

    // An order the strategy does not know (the risk gate's close, say) is
    // named by its ID.
    lines.clear();
    Feed(strategy, context, {"12", "14", "10", "12"});
    Feed(strategy, context, {"12", "13", "10", "12"});
    Feed(strategy, context, {"12", "15", "10", "12"});
    Feed(strategy, context, {"12", "13", "11", "12"});  // armed: order-3
    strategy.OnFill(context, FillOf("order-3", Side::kBuy, "1", "13.01"));
    strategy.OnFill(context, FillOf("gate-1", Side::kSell, "1", "12"));
    ASSERT_FALSE(lines.empty());
    EXPECT_EQ(lines.back(),
              "breakout symbol=BTC/USD: long closed by order gate-1 at 12; "
              "entered at 13.01");

    // A resting target that fills in several parts (its cancel after the
    // first finds it already filled) is cancelled only once, ignores the
    // stop's expiry in between, and is still named as the target on the last.
    lines.clear();
    auto resting = Params();
    resting.resting_target = true;
    Breakout with_target(resting);
    FakeContext oco_context;
    oco_context.venue.oco = true;
    oco_context.venue.cancel_result = trading::CancelOutcome::kAlreadyFilled;
    oco_context.log = context.log;
    Feed(with_target, oco_context);
    with_target.OnFill(oco_context,
                       FillOf("order-1", Side::kBuy, "1", "13.01"));
    with_target.OnFill(oco_context,
                       FillOf("order-2", Side::kSell, "0.3", "15.01"));
    types::OrderStatus stop;
    stop.order_id = "order-3";
    stop.state = types::OrderState::kExpired;
    with_target.OnOrderUpdate(oco_context, stop);
    with_target.OnFill(oco_context,
                       FillOf("order-2", Side::kSell, "0.3", "15.01"));
    with_target.OnFill(oco_context,
                       FillOf("order-2", Side::kSell, "0.4", "15.01"));
    EXPECT_EQ(oco_context.venue.cancels, std::vector<std::string>{"order-2"});
    EXPECT_EQ(oco_context.venue.orders.size(), 3u);  // no market exit placed
    ASSERT_FALSE(lines.empty());
    EXPECT_EQ(lines.back(),
              "breakout symbol=BTC/USD: long closed by the target at 15.01; "
              "entered at 13.01");
}

TEST(Breakout, LogsAnExitAndATargetFilledAfterItsStopExpired) {
    std::vector<std::string> lines;
    const LogContext log("breakout",
                         [&](LogContext::Level level, std::string_view line) {
                             if (level == LogContext::Level::kInfo) {
                                 lines.emplace_back(line);
                             }
                         });
    // Held past max_hold_bars: sold at market, named as the exit.
    auto params = Params();
    params.target_multiple = D("0");
    params.max_hold_bars = 1;
    Breakout strategy(params);
    FakeContext context;
    context.log = log;
    context.venue.cancel_result = trading::CancelOutcome::kCancelled;
    Feed(strategy, context);  // armed: order-1
    strategy.OnFill(context, FillOf("order-1", Side::kBuy, "1", "13.01"));
    Feed(strategy, context, {"13", "13.5", "12.5", "13.2"});  // the entry bar
    ASSERT_EQ(context.venue.orders.size(), 2u);               // entry, stop
    Feed(strategy, context, kQuietBar);  // a whole bar: held long enough
    ASSERT_EQ(context.venue.orders.size(), 3u);  // entry, stop, exit
    EXPECT_EQ(context.venue.orders[2].type, types::OrderType::kMarket);
    strategy.OnFill(context, FillOf("order-3", Side::kSell, "1", "13.2"));
    ASSERT_FALSE(lines.empty());
    EXPECT_EQ(lines.back(),
              "breakout symbol=BTC/USD: long closed by the exit at 13.2; "
              "entered at 13.01");

    // The pair's stop (order-3) reported expired before the target's
    // (order-2) fill: still named as the target.
    lines.clear();
    auto resting = Params();
    resting.resting_target = true;
    Breakout with_target(resting);
    FakeContext oco_context;
    oco_context.venue.oco = true;
    oco_context.log = log;
    Feed(with_target, oco_context);
    with_target.OnFill(oco_context,
                       FillOf("order-1", Side::kBuy, "1", "13.01"));
    types::OrderStatus stop;
    stop.order_id = "order-3";
    stop.state = types::OrderState::kExpired;
    with_target.OnOrderUpdate(oco_context, stop);
    with_target.OnFill(oco_context,
                       FillOf("order-2", Side::kSell, "1", "15.01"));
    ASSERT_FALSE(lines.empty());
    EXPECT_EQ(lines.back(),
              "breakout symbol=BTC/USD: long closed by the target at 15.01; "
              "entered at 13.01");
}

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

// Binance.US takes a buy's fee in the coin: the entry is complete at the
// quantity filled, but the stop and the exit are for what is left, rounded
// down to the lot.
TEST(Breakout, AFeeInTheCoinIsTakenOffTheStopAndTheExit) {
    auto params = Params();
    params.target_multiple = D("0");
    params.max_hold_bars = 1;
    Breakout strategy(params);
    FakeContext context;
    Feed(strategy, context);  // armed: order-1
    auto part = FillOf("order-1", Side::kBuy, "0.5", "13.01");
    part.fee = D("0.0005");
    part.fee_asset = "BTC";
    strategy.OnFill(context, part);
    ASSERT_EQ(context.venue.orders.size(), 1u);  // not all bought yet
    part.fee = D("0.00055");
    strategy.OnFill(context, part);
    ASSERT_EQ(context.venue.orders.size(), 2u);
    EXPECT_EQ(context.venue.orders[1].type, types::OrderType::kStop);
    EXPECT_EQ(context.venue.orders[1].quantity, D("0.9989"));  // 0.99895

    context.venue.cancel_result = trading::CancelOutcome::kCancelled;
    Feed(strategy, context, kQuietBar);  // the entry bar
    ASSERT_EQ(context.venue.orders.size(), 2u);
    Feed(strategy, context, kQuietBar);  // held long enough
    ASSERT_EQ(context.venue.orders.size(), 3u);
    EXPECT_EQ(context.venue.orders[2].type, types::OrderType::kMarket);
    EXPECT_EQ(context.venue.orders[2].quantity, D("0.9989"));
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

TEST(Breakout, AFailedCancelIsRetriedEachBarUntilItLands) {
    Breakout strategy(Params());
    FakeContext context;
    Feed(strategy, context);
    strategy.OnFill(context, FillOf("order-1", Side::kBuy, "1", "13.01"));
    context.venue.cancel_result = trading::CancelOutcome::kCancelled;
    Feed(strategy, context, {"13", "15.5", "13", "15.2"});  // exit order-3
    // The stop sells out first, and the exit's cancel fails.
    context.venue.cancel_error = types::ErrorCategory::kNetwork;
    strategy.OnFill(context, FillOf("order-2", Side::kSell, "1", "10.98"));
    ASSERT_EQ(context.venue.cancels.back(), "order-3");
    Feed(strategy, context, kQuietBar);  // still failing
    EXPECT_EQ(std::ranges::count(context.venue.cancels, "order-3"), 2);
    context.venue.cancel_error.reset();
    context.venue.cancel_result = trading::CancelOutcome::kAlreadyClosed;
    Feed(strategy, context, kQuietBar);  // lands
    Feed(strategy, context, kQuietBar);  // and is not tried again
    EXPECT_EQ(std::ranges::count(context.venue.cancels, "order-3"), 3);
    // Its late fill belongs to the trade before: not counted.
    strategy.OnFill(context, FillOf("order-3", Side::kSell, "1", "15"));
}

TEST(Breakout, ABuyWhileFlatIsNotCountedInTheNextTrade) {
    Breakout strategy(Params());
    FakeContext context;
    strategy.OnFill(context, FillOf("elsewhere", Side::kBuy, "0.5", "12"));
    Feed(strategy, context);  // arms the entry, order-1
    strategy.OnFill(context, FillOf("order-1", Side::kBuy, "1", "13.01"));
    ASSERT_EQ(context.venue.orders.size(), 2u);
    EXPECT_EQ(context.venue.orders[1].quantity, D("1"));  // the stop
}

TEST(Breakout, ARestingTargetSellsWhereThePriceReachesIt) {
    // As EntersOnTheBreakAndTakesProfitAtTheTarget, but the target rests:
    // sold at 15.01 inside hour 6, not at hour 7's open.
    auto params = Params();
    params.resting_target = true;
    const auto trades = Backtest(Nr4Then({{"12.5", "14", "12.4", "13.8"},
                                          {"13.8", "15.5", "13.5", "15.2"},
                                          {"15", "15", "15", "15"}}),
                                 params);
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[1].side, Side::kSell);
    EXPECT_EQ(trades[1].price, D("15.01"));
}

TEST(Breakout, ARestingTargetIsPairedWithTheStop) {
    auto params = Params();
    params.resting_target = true;
    Breakout strategy(params);
    FakeContext context;
    context.venue.oco = true;
    Feed(strategy, context);  // armed: order-1
    strategy.OnFill(context, FillOf("order-1", Side::kBuy, "1", "13.01"));
    ASSERT_EQ(context.venue.ocos.size(), 1u);
    EXPECT_EQ(context.venue.ocos[0].side, Side::kSell);
    EXPECT_EQ(context.venue.ocos[0].quantity, D("1"));
    EXPECT_EQ(context.venue.ocos[0].limit_price, D("15.01"));
    EXPECT_EQ(context.venue.ocos[0].stop_price, D("10.98"));
    // A close past the target sells nothing more: the limit is there.
    Feed(strategy, context, {"13", "15.5", "13", "15.2"});
    EXPECT_EQ(context.venue.orders.size(), 3u);  // entry, limit, stop
    EXPECT_TRUE(context.venue.cancels.empty());
    // The limit (order-2) fills: flat, the stop (order-3) taken down.
    context.venue.cancel_result = trading::CancelOutcome::kAlreadyClosed;
    strategy.OnFill(context, FillOf("order-2", Side::kSell, "1", "15.01"));
    EXPECT_EQ(context.venue.cancels, std::vector<std::string>{"order-3"});
    Feed(strategy, context, {"12", "14", "10", "12"});
    Feed(strategy, context, {"12", "13", "10", "12"});
    Feed(strategy, context, {"12", "15", "10", "12"});
    Feed(strategy, context, {"12", "13", "11", "12"});
    ASSERT_EQ(context.venue.orders.size(), 4u);  // the next setup armed
    EXPECT_EQ(context.venue.orders[3].side, Side::kBuy);
    EXPECT_EQ(context.venue.orders[3].type, types::OrderType::kStop);
}

TEST(Breakout, ARestingTargetWithoutPairsFallsBackToTheClose) {
    auto params = Params();
    params.resting_target = true;
    Breakout strategy(params);
    FakeContext context;  // RecordingVenue has no OCO by default
    Feed(strategy, context);
    strategy.OnFill(context, FillOf("order-1", Side::kBuy, "1", "13.01"));
    ASSERT_EQ(context.venue.orders.size(), 2u);
    EXPECT_EQ(context.venue.orders[1].type, types::OrderType::kStop);
    context.venue.cancel_result = trading::CancelOutcome::kCancelled;
    Feed(strategy, context, {"13", "15.5", "13", "15.2"});
    ASSERT_EQ(context.venue.orders.size(), 3u);
    EXPECT_EQ(context.venue.orders[2].type, types::OrderType::kMarket);
}

TEST(Breakout, ATargetFilledInPartSellsTheRestAtMarket) {
    auto params = Params();
    params.resting_target = true;
    Breakout strategy(params);
    FakeContext context;
    context.venue.oco = true;
    Feed(strategy, context);
    strategy.OnFill(context, FillOf("order-1", Side::kBuy, "1", "13.01"));
    // The limit (order-2) sells 0.4: the pair is taken down at once and the
    // rest sold, whether or not the venue has closed the stop (order-3) yet.
    context.venue.cancel_result = trading::CancelOutcome::kCancelled;
    strategy.OnFill(context, FillOf("order-2", Side::kSell, "0.4", "15.01"));
    EXPECT_EQ(context.venue.cancels,
              (std::vector<std::string>{"order-2", "order-3"}));
    ASSERT_EQ(context.venue.orders.size(), 4u);
    EXPECT_EQ(context.venue.orders[3].type, types::OrderType::kMarket);
    EXPECT_EQ(context.venue.orders[3].quantity, D("0.6"));
    // The stop's expiry, arriving after, changes nothing.
    types::OrderStatus stop;
    stop.order_id = "order-3";
    stop.state = types::OrderState::kExpired;
    strategy.OnOrderUpdate(context, stop);
    EXPECT_EQ(context.venue.orders.size(), 4u);
}

TEST(Breakout, ARefusedPairFallsBackToAStopAtOnce) {
    auto params = Params();
    params.resting_target = true;
    Breakout strategy(params);
    FakeContext context;
    context.venue.oco = true;
    context.venue.oco_error = types::ErrorCategory::kRejected;  // a risk limit
    Feed(strategy, context);
    strategy.OnFill(context, FillOf("order-1", Side::kBuy, "1", "13.01"));
    ASSERT_EQ(context.venue.orders.size(), 2u);
    EXPECT_EQ(context.venue.orders[1].type, types::OrderType::kStop);
    EXPECT_EQ(context.venue.orders[1].stop_price, D("10.98"));
}

TEST(Breakout, APairWithAnUnknownOutcomeIsNotDoubledWithAStop) {
    for (const auto category :
         {types::ErrorCategory::kTimeout, types::ErrorCategory::kParse}) {
        auto params = Params();
        params.resting_target = true;
        Breakout strategy(params);
        FakeContext context;
        context.venue.oco = true;
        context.venue.oco_error = category;
        Feed(strategy, context);
        strategy.OnFill(context, FillOf("order-1", Side::kBuy, "1", "13.01"));
        EXPECT_EQ(context.venue.orders.size(), 1u);  // the pair may exist
    }
}

// An open order on kSymbol, as GetOpenOrders reports it.
types::OrderStatus OpenOrder(const char* id, Side side, types::OrderType type,
                             const char* quantity,
                             std::optional<const char*> price = std::nullopt) {
    types::OrderStatus order;
    order.order_id = id;
    order.symbol = kSymbol;
    order.side = side;
    order.type = type;
    order.quantity = D(quantity);
    order.state = types::OrderState::kNew;
    if (price && type == types::OrderType::kStop) {
        order.stop_price = D(*price);
    } else if (price) {
        order.limit_price = D(*price);
    }
    return order;
}

// FakeContext's setup arms at 13.01 with the stop at 10.98: 2.03 a unit.
BreakoutParams FractionOf(const char* symbol) {
    BreakoutParams params;
    params.symbol = symbol;
    params.quantity = D("100");
    params.risk_fraction = D("0.02");
    params.default_quote_asset = "USD";
    return params;
}

TEST(Breakout, RiskFractionPlacesNothingWhenBalancesCannotBeRead) {
    Breakout strategy(FractionOf("AAPL"));
    FakeContext context;
    context.venue.fail_balances = true;
    strategy.OnBar(context, "AAPL", Timeframe::kH1, context.bars.back());
    EXPECT_TRUE(context.venue.orders.empty());
}

TEST(Breakout, RiskFractionPlacesNothingWithoutAQuoteBalance) {
    Breakout strategy(FractionOf("AAPL"));
    FakeContext context;  // no balances at all
    strategy.OnBar(context, "AAPL", Timeframe::kH1, context.bars.back());
    EXPECT_TRUE(context.venue.orders.empty());
}

TEST(Breakout, RiskFractionSizesFromTheAvailableBalanceNotTheTotal) {
    // An equity takes default_quote_asset. 1,000 USD with 900 locked leaves
    // 100: 2 to risk, 0.9852 units.
    Breakout strategy(FractionOf("AAPL"));
    FakeContext context;
    context.venue.balances = {{"USD", D("1000"), D("100"), D("900")}};
    strategy.OnBar(context, "AAPL", Timeframe::kH1, context.bars.back());
    ASSERT_EQ(context.venue.orders.size(), 1u);
    EXPECT_EQ(context.venue.orders[0].quantity, D("0.9852"));
}

TEST(Breakout, RiskFractionReadsThePairsQuoteAssetUnderItsAllocation) {
    // BTC/USDT sizes from USDT, not USD, and from the 100 held rather than
    // the larger allocation of 500.
    auto params = FractionOf("BTC/USDT");
    params.allocation = D("500");
    Breakout strategy(params);
    FakeContext context;
    context.venue.balances = {{"USD", D("9000"), D("9000"), D("0")},
                              {"USDT", D("100"), D("100"), D("0")}};
    strategy.OnBar(context, "BTC/USDT", Timeframe::kH1, context.bars.back());
    ASSERT_EQ(context.venue.orders.size(), 1u);
    EXPECT_EQ(context.venue.orders[0].quantity, D("0.9852"));
}

// Long 1 at 13.01, its stop (stop 10.98) placed without an answer.
void LongWithAnUnansweredStop(Breakout& strategy, FakeContext& context) {
    Feed(strategy, context);  // the entry: order-1
    context.venue.reject_category = types::ErrorCategory::kTimeout;
    strategy.OnFill(context, FillOf("order-1", Side::kBuy, "1", "13.01"));
    context.venue.reject_category.reset();
    ASSERT_EQ(context.venue.orders.size(), 1u);
}

TEST(Breakout, AdoptsAnUnansweredStopFromOpenOrders) {
    Breakout strategy(Params());
    FakeContext context;
    LongWithAnUnansweredStop(strategy, context);
    context.venue.open_orders = {OpenOrder(
        "stop-live", Side::kSell, types::OrderType::kStop, "1", "10.98")};
    Feed(strategy, context, kQuietBar);
    EXPECT_EQ(context.venue.orders.size(), 1u);  // no second stop
    EXPECT_TRUE(context.venue.cancels.empty());
    EXPECT_EQ(context.tracked_orders, std::vector<std::string>{"stop-live"});
}

TEST(Breakout, AnAdoptedStopIsPlacedAgainWhenItClosesUnfilled) {
    Breakout strategy(Params());
    FakeContext context;
    LongWithAnUnansweredStop(strategy, context);
    auto stop = OpenOrder("stop-live", Side::kSell, types::OrderType::kStop,
                          "1", "10.98");
    context.venue.open_orders = {stop};
    Feed(strategy, context, kQuietBar);
    context.venue.open_orders.clear();
    stop.state = types::OrderState::kCancelled;
    strategy.OnOrderUpdate(context, stop);
    ASSERT_EQ(context.venue.orders.size(), 2u);
    EXPECT_EQ(context.venue.orders[1].type, types::OrderType::kStop);
    EXPECT_EQ(context.venue.orders[1].stop_price, D("10.98"));
}

TEST(Breakout, PlacesTheStopAgainWhenTheUnansweredOneIsNotOpen) {
    Breakout strategy(Params());
    FakeContext context;
    LongWithAnUnansweredStop(strategy, context);
    Feed(strategy, context, kQuietBar);  // GetOpenOrders: nothing
    ASSERT_EQ(context.venue.orders.size(), 2u);
    EXPECT_EQ(context.venue.orders[1].type, types::OrderType::kStop);
}

// Long 1 at 13.01 with resting_target, its pair placed without an answer
// (`category`).
void LongWithAnUnansweredPair(Breakout& strategy, FakeContext& context,
                              types::ErrorCategory category) {
    context.venue.oco = true;
    context.venue.oco_error = category;
    Feed(strategy, context);
    strategy.OnFill(context, FillOf("order-1", Side::kBuy, "1", "13.01"));
    context.venue.oco_error.reset();
    ASSERT_EQ(context.venue.orders.size(), 1u);
}

BreakoutParams WithTarget() {
    auto params = Params();
    params.resting_target = true;
    return params;
}

TEST(Breakout, AdoptsAnUnansweredPairFromOpenOrders) {
    Breakout strategy(WithTarget());
    FakeContext context;
    LongWithAnUnansweredPair(strategy, context, types::ErrorCategory::kParse);
    context.venue.open_orders = {
        OpenOrder("oco-limit", Side::kSell, types::OrderType::kLimit, "1",
                  "15.01"),
        OpenOrder("oco-stop", Side::kSell, types::OrderType::kStop, "1",
                  "10.98")};
    // A close past the target: the adopted limit is resting, so no market
    // sell for it.
    Feed(strategy, context, {"13", "15.5", "13", "15.2"});
    EXPECT_EQ(context.venue.orders.size(), 1u);
    EXPECT_TRUE(context.venue.ocos.empty());
    EXPECT_TRUE(context.venue.cancels.empty());
    EXPECT_EQ(context.tracked_orders,
              (std::vector<std::string>{"oco-limit", "oco-stop"}));
}

TEST(Breakout, AnAdoptedPairsTargetFillClosesItsStop) {
    Breakout strategy(WithTarget());
    FakeContext context;
    LongWithAnUnansweredPair(strategy, context, types::ErrorCategory::kParse);
    context.venue.open_orders = {
        OpenOrder("oco-limit", Side::kSell, types::OrderType::kLimit, "1",
                  "15.01"),
        OpenOrder("oco-stop", Side::kSell, types::OrderType::kStop, "1",
                  "10.98")};
    Feed(strategy, context, kQuietBar);
    context.venue.cancel_result = trading::CancelOutcome::kAlreadyClosed;
    strategy.OnFill(context, FillOf("oco-limit", Side::kSell, "1", "15.01"));
    EXPECT_EQ(context.venue.cancels, std::vector<std::string>{"oco-stop"});
}

TEST(Breakout, CancelsALoneTargetLegAndPlacesThePairAgain) {
    Breakout strategy(WithTarget());
    FakeContext context;
    LongWithAnUnansweredPair(strategy, context, types::ErrorCategory::kNetwork);
    context.venue.open_orders = {OpenOrder(
        "half-limit", Side::kSell, types::OrderType::kLimit, "1", "15.01")};
    context.venue.cancel_result = trading::CancelOutcome::kCancelled;
    Feed(strategy, context, kQuietBar);
    EXPECT_EQ(context.venue.cancels, std::vector<std::string>{"half-limit"});
    EXPECT_EQ(context.venue.ocos.size(), 1u);
}

TEST(Breakout, LeavesAnUnansweredClosersOrdersAloneWhileClosed) {
    Breakout strategy(WithTarget());
    FakeContext context;
    LongWithAnUnansweredPair(strategy, context, types::ErrorCategory::kNetwork);
    context.venue.open_orders = {OpenOrder(
        "half-limit", Side::kSell, types::OrderType::kLimit, "1", "15.01")};
    context.open = false;
    Feed(strategy, context, kQuietBar);
    EXPECT_TRUE(context.venue.cancels.empty());
    EXPECT_TRUE(context.venue.ocos.empty());
}

// Long with resting_target and breakeven_r 0.5, the pair (order-2 and
// order-3) resting; then a close raises the stop to 13.01 and the new pair
// gets no answer.
void RaisedStopUnanswered(Breakout& strategy, FakeContext& context) {
    context.venue.oco = true;
    Feed(strategy, context);
    strategy.OnFill(context, FillOf("order-1", Side::kBuy, "1", "13.01"));
    ASSERT_EQ(context.venue.ocos.size(), 1u);
    context.venue.cancel_result = trading::CancelOutcome::kCancelled;
    context.venue.oco_error = types::ErrorCategory::kTimeout;
    Feed(strategy, context, {"13.5", "14.6", "13.4", "14.5"});
    context.venue.oco_error.reset();
}

BreakoutParams WithTargetAndBreakeven() {
    auto params = WithTarget();
    params.breakeven_r = D("0.5");
    return params;
}

TEST(Breakout, ARaisedStopWithoutAnAnswerIsNotSoldAtMarket) {
    Breakout strategy(WithTargetAndBreakeven());
    FakeContext context;
    RaisedStopUnanswered(strategy, context);
    EXPECT_EQ(context.venue.cancels, std::vector<std::string>{"order-3"});
    EXPECT_EQ(context.venue.orders.size(), 3u);  // the pair; no market sell
}

TEST(Breakout, AdoptsALoneStopLegAndMovesItToTheRaisedLevel) {
    Breakout strategy(WithTargetAndBreakeven());
    FakeContext context;
    RaisedStopUnanswered(strategy, context);
    // Only the pair's stop leg is open, at the level before the raise: it is
    // adopted, the target is left to the close, and the stop moves to 13.01.
    context.venue.open_orders = {OpenOrder(
        "lone-stop", Side::kSell, types::OrderType::kStop, "1", "10.98")};
    Feed(strategy, context, kQuietBar);
    EXPECT_EQ(context.venue.cancels,
              (std::vector<std::string>{"order-3", "lone-stop"}));
    EXPECT_EQ(context.venue.ocos.size(), 1u);  // no second pair
    ASSERT_EQ(context.venue.orders.size(), 4u);
    EXPECT_EQ(context.venue.orders[3].type, types::OrderType::kStop);
    EXPECT_EQ(context.venue.orders[3].stop_price, D("13.01"));
}

BreakoutParams FlattenAtClose() {
    auto params = Params();
    params.flatten_at_close = true;
    params.target_multiple = D("0");
    return params;
}

// Long 0.99895 after a 0.00105 base-asset fee (0.9989 to the lot), its stop
// unanswered; then the closing flatten finds GetOpenOrders failing.
void FlattenWithOpenOrdersUnreadable(Breakout& strategy, FakeContext& context) {
    Feed(strategy, context);
    auto buy = FillOf("order-1", Side::kBuy, "1", "13.01");
    buy.fee = D("0.00105");
    buy.fee_asset = "BTC";
    context.venue.reject_category = types::ErrorCategory::kTimeout;
    strategy.OnFill(context, buy);
    context.venue.reject_category.reset();
    context.venue.fail_open_orders = true;
    strategy.OnSessionClosing(context, Hour(6));
    context.venue.fail_open_orders = false;
}

TEST(Breakout, AFlattenWaitsWhileOpenOrdersCannotBeRead) {
    Breakout strategy(FlattenAtClose());
    FakeContext context;
    std::vector<std::string> errors;
    context.log = LogContext(
        "breakout", [&](LogContext::Level level, std::string_view line) {
            if (level == LogContext::Level::kError) {
                errors.emplace_back(line);
            }
        });
    FlattenWithOpenOrdersUnreadable(strategy, context);
    EXPECT_EQ(context.venue.orders.size(), 1u);  // no sell beside the stop
    EXPECT_TRUE(std::ranges::any_of(errors, [](const std::string& line) {
        return line.find("cannot read open orders") != std::string::npos;
    }));
}

TEST(Breakout, ADeferredFlattenSellsAtTheOpenAfterAdoptingTheStop) {
    Breakout strategy(FlattenAtClose());
    FakeContext context;
    FlattenWithOpenOrdersUnreadable(strategy, context);
    // The stop that got no answer is open, at the fee-netted lot: adopted,
    // then cancelled so the flatten sells it.
    context.venue.open_orders = {OpenOrder(
        "stop-live", Side::kSell, types::OrderType::kStop, "0.9989", "10.98")};
    context.venue.cancel_result = trading::CancelOutcome::kCancelled;
    strategy.OnSessionOpen(context);
    EXPECT_EQ(context.venue.cancels, std::vector<std::string>{"stop-live"});
    ASSERT_EQ(context.venue.orders.size(), 2u);
    EXPECT_EQ(context.venue.orders[1].type, types::OrderType::kMarket);
    EXPECT_EQ(context.venue.orders[1].quantity, D("0.9989"));
}

TEST(Breakout, ALateEntryFillWaitsForTheOldSizedStopToSettle) {
    Breakout strategy(Params());
    FakeContext context;
    Feed(strategy, context);
    // 0.4 of the entry fills; the next bar disarms the rest, and the stop
    // for 0.4 gets no answer.
    strategy.OnFill(context, FillOf("order-1", Side::kBuy, "0.4", "13.01"));
    context.venue.cancel_result = trading::CancelOutcome::kAlreadyClosed;
    context.venue.reject_category = types::ErrorCategory::kTimeout;
    Feed(strategy, context, kQuietBar);
    context.venue.reject_category.reset();
    context.venue.open_orders = {OpenOrder(
        "stop-04", Side::kSell, types::OrderType::kStop, "0.4", "10.98")};

    // The rest of the entry (0.6) fills late. The 0.4 stop cannot serve 1,
    // and cancelling it fails: no new stop yet.
    context.venue.cancel_error = types::ErrorCategory::kNetwork;
    strategy.OnFill(context, FillOf("order-1", Side::kBuy, "0.6", "13.01"));
    EXPECT_EQ(context.venue.orders.size(), 1u);

    // Next bar, the 0.4 stop has filled: still no new stop until its fill
    // arrives.
    context.venue.cancel_error.reset();
    context.venue.cancel_result = trading::CancelOutcome::kAlreadyFilled;
    Feed(strategy, context, kQuietBar);
    EXPECT_EQ(context.venue.orders.size(), 1u);

    // Its fill leaves 0.6, which the next bar protects.
    strategy.OnFill(context, FillOf("stop-04", Side::kSell, "0.4", "10.98"));
    context.venue.open_orders.clear();
    Feed(strategy, context, kQuietBar);
    ASSERT_EQ(context.venue.orders.size(), 2u);
    EXPECT_EQ(context.venue.orders[1].type, types::OrderType::kStop);
    EXPECT_EQ(context.venue.orders[1].quantity, D("0.6"));
}

// A short's setup: the entry sells at 11.99, its stop buys at 14.02.
void ShortSetup(FakeContext& context) {
    context.bars.clear();
    for (const auto& b :
         std::vector<std::array<const char*, 4>>{{"13", "15", "11", "13"},
                                                 {"13", "15", "12", "13"},
                                                 {"13", "15", "10", "13"},
                                                 {"13", "15", "11", "13"},
                                                 {"13", "14", "12", "13"}}) {
        context.Add(b);
    }
}

BreakoutParams AsShort(BreakoutParams params) {
    params.direction = BreakoutParams::Direction::kShort;
    return params;
}

TEST(Breakout, AShortAdoptsAnUnansweredBuyStopAndIgnoresSells) {
    Breakout strategy(AsShort(Params()));
    FakeContext context;
    ShortSetup(context);
    Feed(strategy, context);
    context.venue.reject_category = types::ErrorCategory::kTimeout;
    strategy.OnFill(context, FillOf("order-1", Side::kSell, "1", "11.99"));
    context.venue.reject_category.reset();
    context.venue.open_orders = {
        OpenOrder("other-sell", Side::kSell, types::OrderType::kLimit, "1",
                  "20"),
        OpenOrder("short-stop", Side::kBuy, types::OrderType::kStop, "1",
                  "14.02")};
    Feed(strategy, context, {"12.5", "13", "12", "12.5"});
    EXPECT_EQ(context.tracked_orders, std::vector<std::string>{"short-stop"});
    EXPECT_EQ(context.venue.orders.size(), 1u);
    EXPECT_TRUE(context.venue.cancels.empty());
}

TEST(Breakout, AShortAdoptsAnUnansweredPair) {
    auto params = AsShort(WithTarget());
    Breakout strategy(params);
    FakeContext context;
    ShortSetup(context);
    context.venue.oco = true;
    context.venue.oco_error = types::ErrorCategory::kTimeout;
    Feed(strategy, context);  // short entry stop at 11.99, stop 14.02
    strategy.OnFill(context, FillOf("order-1", Side::kSell, "1", "11.99"));
    context.venue.oco_error.reset();
    ASSERT_EQ(context.venue.orders.size(), 1u);
    // The pair's legs buy: the target below, the stop above.
    context.venue.open_orders = {
        OpenOrder("oco-limit", Side::kBuy, types::OrderType::kLimit, "1",
                  "9.99"),
        OpenOrder("oco-stop", Side::kBuy, types::OrderType::kStop, "1",
                  "14.02")};
    Feed(strategy, context, {"12.5", "13", "12", "12.5"});
    EXPECT_EQ(context.tracked_orders,
              (std::vector<std::string>{"oco-limit", "oco-stop"}));
    EXPECT_EQ(context.venue.orders.size(), 1u);
    EXPECT_TRUE(context.venue.ocos.empty());
    EXPECT_TRUE(context.venue.cancels.empty());
}

TEST(Breakout, AStopWaitsWhileOpenOrdersCannotBeRead) {
    Breakout strategy(Params());
    FakeContext context;
    std::vector<std::string> errors;
    context.log = LogContext(
        "breakout", [&](LogContext::Level level, std::string_view line) {
            if (level == LogContext::Level::kError) {
                errors.emplace_back(line);
            }
        });
    LongWithAnUnansweredStop(strategy, context);
    // The unanswered stop may rest: nothing is placed beside it while the
    // open orders cannot be read, bar after bar, even below the stop.
    context.venue.fail_open_orders = true;
    Feed(strategy, context, kQuietBar);
    Feed(strategy, context, {"12", "12", "10.5", "10.6"});
    EXPECT_EQ(context.venue.orders.size(), 1u);
    const auto attempts =
        std::ranges::count_if(errors, [](const std::string& line) {
            return line.find("cannot read open orders") != std::string::npos;
        });
    EXPECT_EQ(attempts, 2);
    // Readable again and nothing resting: closed below the stop, so a stop
    // would be refused or sell at once. It sells at market.
    context.venue.fail_open_orders = false;
    Feed(strategy, context, {"10.6", "10.7", "10.4", "10.5"});
    ASSERT_EQ(context.venue.orders.size(), 2u);
    EXPECT_EQ(context.venue.orders[1].type, types::OrderType::kMarket);
    EXPECT_EQ(context.venue.orders[1].side, Side::kSell);
}

// Sell orders placed from the `first`th on.
std::ptrdiff_t SellsFrom(const FakeContext& context, std::size_t first) {
    return std::count_if(
        context.venue.orders.begin() + static_cast<std::ptrdiff_t>(first),
        context.venue.orders.end(), [](const types::OrderRequest& order) {
            return order.side == Side::kSell;
        });
}

// The venue's BTC balance: what StillHeld reads on a spot venue.
void HoldsBtc(FakeContext& context, const char* quantity) {
    context.venue.balances = {{"BTC", D(quantity), D(quantity), D("0")}};
}

// Long 1 with its stop (order-2) resting; the closing flatten cancels the
// stop and its market sell gets no answer.
void FlattenUnanswered(Breakout& strategy, FakeContext& context,
                       Side entry_side = Side::kBuy,
                       const char* entry_price = "13.01") {
    Feed(strategy, context);
    strategy.OnFill(context, FillOf("order-1", entry_side, "1", entry_price));
    ASSERT_EQ(context.venue.orders.size(), 2u);
    context.venue.cancel_result = trading::CancelOutcome::kCancelled;
    context.venue.reject_type = types::OrderType::kMarket;
    context.venue.reject_type_category = types::ErrorCategory::kTimeout;
    strategy.OnSessionClosing(context, Hour(6));
    context.venue.reject_type.reset();
    context.venue.reject_type_category.reset();
}

TEST(Breakout, AnUnansweredExitProtectsTheLongAgainAtOnce) {
    Breakout strategy(FlattenAtClose());
    FakeContext context;
    HoldsBtc(context, "1");  // the sale did not go through
    FlattenUnanswered(strategy, context);
    EXPECT_EQ(context.venue.cancels, std::vector<std::string>{"order-2"});
    ASSERT_EQ(context.venue.orders.size(), 3u);
    EXPECT_EQ(context.venue.orders[2].type, types::OrderType::kStop);
    EXPECT_EQ(context.venue.orders[2].quantity, D("1"));
    // The next bar tries the exit again: the new stop cancelled, then sold.
    Feed(strategy, context, kQuietBar);
    EXPECT_EQ(context.venue.cancels,
              (std::vector<std::string>{"order-2", "order-3"}));
    ASSERT_EQ(context.venue.orders.size(), 4u);
    EXPECT_EQ(context.venue.orders[3].type, types::OrderType::kMarket);
}

TEST(Breakout, AnUnansweredExitThatWentThroughGetsNoStop) {
    Breakout strategy(FlattenAtClose());
    FakeContext context;
    HoldsBtc(context, "0");  // sold: a stop now would sell short on margin
    FlattenUnanswered(strategy, context);
    EXPECT_EQ(context.venue.orders.size(), 2u);
    // Its fill, from an order the strategy never learned of, makes it flat,
    // so the next bar sells nothing (it may arm a new entry).
    strategy.OnFill(context, FillOf("exit-unseen", Side::kSell, "1", "12"));
    Feed(strategy, context, kQuietBar);
    EXPECT_EQ(SellsFrom(context, 2), 0);
}

TEST(Breakout, AnUnansweredExitGetsNoStopWhenTheHoldingIsUnknown) {
    Breakout strategy(FlattenAtClose());
    FakeContext context;  // no position or BTC balance listed
    FlattenUnanswered(strategy, context);
    EXPECT_EQ(context.venue.orders.size(), 2u);
}

TEST(Breakout, AnUnansweredExitPastTheStopGetsNoStop) {
    Breakout strategy(FlattenAtClose());
    FakeContext context;
    HoldsBtc(context, "1");
    Feed(strategy, context);
    strategy.OnFill(context, FillOf("order-1", Side::kBuy, "1", "13.01"));
    // The last close is under the stop (10.98): a stop there would be refused
    // or sell at once, so the exit is only tried again.
    context.Add({"11", "11", "10.5", "10.6"});
    context.venue.cancel_result = trading::CancelOutcome::kCancelled;
    context.venue.reject_type = types::OrderType::kMarket;
    context.venue.reject_type_category = types::ErrorCategory::kTimeout;
    strategy.OnSessionClosing(context, Hour(6));
    EXPECT_EQ(context.venue.cancels, std::vector<std::string>{"order-2"});
    EXPECT_EQ(context.venue.orders.size(), 2u);
}

TEST(Breakout, AnUnansweredExitStillWorkingIsAdopted) {
    Breakout strategy(FlattenAtClose());
    FakeContext context;
    HoldsBtc(context, "1");
    context.venue.open_orders = {
        OpenOrder("exit-live", Side::kSell, types::OrderType::kMarket, "1")};
    FlattenUnanswered(strategy, context);
    EXPECT_EQ(context.venue.orders.size(), 2u);  // no stop beside it
    EXPECT_EQ(context.tracked_orders, std::vector<std::string>{"exit-live"});
    // Its fill makes us flat: the next bar sells nothing (it may arm a new
    // entry).
    context.venue.open_orders.clear();
    strategy.OnFill(context, FillOf("exit-live", Side::kSell, "1", "12"));
    Feed(strategy, context, kQuietBar);
    EXPECT_EQ(SellsFrom(context, 2), 0);
}

TEST(Breakout, AnUnansweredExitShowingUpLateTakesDownTheStopPutBack) {
    Breakout strategy(FlattenAtClose());
    FakeContext context;
    HoldsBtc(context, "1");
    FlattenUnanswered(strategy, context);
    ASSERT_EQ(context.venue.orders.size(), 3u);  // the stop put back
    // The next bar finds the sale working after all: adopted as the exit, the
    // stop put back is cancelled, and no second sale is sent.
    context.venue.open_orders = {
        OpenOrder("exit-late", Side::kSell, types::OrderType::kMarket, "1")};
    Feed(strategy, context, kQuietBar);
    EXPECT_EQ(context.tracked_orders, std::vector<std::string>{"exit-late"});
    EXPECT_EQ(context.venue.cancels,
              (std::vector<std::string>{"order-2", "order-3"}));
    EXPECT_EQ(context.venue.orders.size(), 3u);
}

TEST(Breakout, AnUnansweredExitPutsBackAPlainStopNotAPair) {
    auto params = FlattenAtClose();
    params.resting_target = true;
    params.target_multiple = D("1");
    Breakout strategy(params);
    FakeContext context;
    context.venue.oco = true;
    HoldsBtc(context, "1");
    Feed(strategy, context);
    strategy.OnFill(context, FillOf("order-1", Side::kBuy, "1", "13.01"));
    ASSERT_EQ(context.venue.ocos.size(), 1u);  // target order-2, stop order-3
    context.venue.cancel_result = trading::CancelOutcome::kCancelled;
    context.venue.reject_type = types::OrderType::kMarket;
    context.venue.reject_type_category = types::ErrorCategory::kTimeout;
    strategy.OnSessionClosing(context, Hour(6));
    // With the exit due, a target would only be cancelled again.
    EXPECT_EQ(context.venue.ocos.size(), 1u);
    ASSERT_EQ(context.venue.orders.size(), 4u);
    EXPECT_EQ(context.venue.orders[3].type, types::OrderType::kStop);
}

TEST(Breakout, AnUnansweredExitReadsTheVenuesPositionFirst) {
    // A venue that lists positions (Schwab, the paper venue) is asked for the
    // symbol's: here it shows only part of the long, so the sale went
    // through at least in part, and no stop goes back, whatever the balances.
    Breakout strategy(FlattenAtClose());
    FakeContext context;
    HoldsBtc(context, "1");
    types::Position position;
    position.symbol = kSymbol;
    position.quantity = D("0.4");
    context.venue.positions = {position};
    FlattenUnanswered(strategy, context);
    EXPECT_EQ(context.venue.orders.size(), 2u);
}

TEST(Breakout, AnUnansweredExitPutsTheStopBackForAListedPosition) {
    Breakout strategy(FlattenAtClose());
    FakeContext context;
    types::Position position;
    position.symbol = kSymbol;
    position.quantity = D("1");
    context.venue.positions = {position};  // no balances listed
    FlattenUnanswered(strategy, context);
    ASSERT_EQ(context.venue.orders.size(), 3u);
    EXPECT_EQ(context.venue.orders[2].type, types::OrderType::kStop);
}

TEST(Breakout, AStopPutBackWithoutAnAnswerIsAdoptedNotCancelled) {
    Breakout strategy(FlattenAtClose());
    FakeContext context;
    HoldsBtc(context, "1");
    Feed(strategy, context);
    strategy.OnFill(context, FillOf("order-1", Side::kBuy, "1", "13.01"));
    // Both the exit and the stop put back get no answer.
    context.venue.cancel_result = trading::CancelOutcome::kCancelled;
    context.venue.reject_category = types::ErrorCategory::kTimeout;
    strategy.OnSessionClosing(context, Hour(6));
    context.venue.reject_category.reset();
    EXPECT_EQ(context.venue.orders.size(), 2u);
    // The stop turns up: adopted (not cancelled as a stray), then cancelled
    // by the exit's retry, which sells.
    context.venue.open_orders = {OpenOrder(
        "stop-late", Side::kSell, types::OrderType::kStop, "1", "10.98")};
    Feed(strategy, context, kQuietBar);
    EXPECT_EQ(context.tracked_orders, std::vector<std::string>{"stop-late"});
    EXPECT_EQ(context.venue.cancels,
              (std::vector<std::string>{"order-2", "stop-late"}));
    ASSERT_EQ(context.venue.orders.size(), 3u);
    EXPECT_EQ(context.venue.orders[2].type, types::OrderType::kMarket);
}

TEST(Breakout, AnUnansweredExitWaitsWhenOpenOrdersCannotBeRead) {
    Breakout strategy(FlattenAtClose());
    FakeContext context;
    HoldsBtc(context, "1");
    Feed(strategy, context);
    strategy.OnFill(context, FillOf("order-1", Side::kBuy, "1", "13.01"));
    context.venue.cancel_result = trading::CancelOutcome::kCancelled;
    context.venue.reject_type = types::OrderType::kMarket;
    context.venue.reject_type_category = types::ErrorCategory::kTimeout;
    context.venue.fail_open_orders = true;
    strategy.OnSessionClosing(context, Hour(6));
    context.venue.reject_type.reset();
    context.venue.reject_type_category.reset();
    context.venue.fail_open_orders = false;
    EXPECT_EQ(context.venue.orders.size(), 2u);  // nothing placed blind
    // Readable on the next bar: the late exit is still looked for.
    context.venue.open_orders = {
        OpenOrder("exit-late", Side::kSell, types::OrderType::kMarket, "1")};
    Feed(strategy, context, kQuietBar);
    EXPECT_EQ(context.tracked_orders, std::vector<std::string>{"exit-late"});
    EXPECT_EQ(context.venue.orders.size(), 2u);
}

TEST(Breakout, AShortsUnansweredCoverWaitsForTheNextBar) {
    Breakout strategy(AsShort(FlattenAtClose()));
    FakeContext context;
    ShortSetup(context);
    FlattenUnanswered(strategy, context, Side::kSell, "11.99");
    // No buy-stop beside a cover that may have gone through.
    EXPECT_EQ(context.venue.orders.size(), 2u);
}

TEST(Breakout, ARefusedRetryOfAnUnansweredExitIsNotRepeated) {
    Breakout strategy(FlattenAtClose());
    FakeContext context;
    HoldsBtc(context, "1");
    FlattenUnanswered(strategy, context);
    ASSERT_EQ(context.venue.orders.size(), 3u);  // the stop placed again
    // The retry is refused outright: the long is protected again, and the
    // one-off flatten is not tried a third time.
    context.venue.reject_type = types::OrderType::kMarket;
    Feed(strategy, context, kQuietBar);
    ASSERT_EQ(context.venue.orders.size(), 4u);
    EXPECT_EQ(context.venue.orders[3].type, types::OrderType::kStop);
    context.venue.reject_type.reset();
    Feed(strategy, context, kQuietBar);
    EXPECT_EQ(context.venue.orders.size(), 4u);
}

TEST(Breakout, APairRefusedStopFirstIsNotPlacedAgain) {
    auto params = Params();
    params.resting_target = true;
    Breakout strategy(params);
    FakeContext context;
    context.venue.oco = true;
    Feed(strategy, context);
    strategy.OnFill(context, FillOf("order-1", Side::kBuy, "1", "13.01"));
    context.venue.cancel_result = trading::CancelOutcome::kAlreadyClosed;
    types::OrderStatus status;
    status.state = types::OrderState::kRejected;
    status.order_id = "order-3";  // the stop's rejection first
    strategy.OnOrderUpdate(context, status);
    status.order_id = "order-2";
    strategy.OnOrderUpdate(context, status);
    ASSERT_EQ(context.venue.orders.size(), 4u);
    EXPECT_EQ(context.venue.orders[3].type, types::OrderType::kStop);
    EXPECT_EQ(context.venue.ocos.size(), 1u);
}

TEST(Breakout, AHalfPlacedPairIsTakenDown) {
    auto params = Params();
    params.resting_target = true;
    Breakout strategy(params);
    FakeContext context;
    context.venue.oco = true;
    context.venue.oco_limit_state = types::OrderState::kRejected;
    context.venue.cancel_result = trading::CancelOutcome::kCancelled;
    Feed(strategy, context);
    strategy.OnFill(context, FillOf("order-1", Side::kBuy, "1", "13.01"));
    EXPECT_EQ(context.venue.cancels, std::vector<std::string>{"order-3"});
    // The next bar tries the pair again.
    context.venue.oco_limit_state.reset();
    Feed(strategy, context, kQuietBar);
    EXPECT_EQ(context.venue.ocos.size(), 2u);
}

TEST(Breakout, ARefusedTargetLeavesTheStopAndTheClose) {
    auto params = Params();
    params.resting_target = true;
    Breakout strategy(params);
    FakeContext context;
    context.venue.oco = true;
    context.venue.oco_error = types::ErrorCategory::kWouldTake;
    Feed(strategy, context);
    strategy.OnFill(context, FillOf("order-1", Side::kBuy, "1", "13.01"));
    ASSERT_EQ(context.venue.orders.size(), 2u);
    EXPECT_EQ(context.venue.orders[1].type, types::OrderType::kStop);
    // Refused later, on arrival: the stop alone again.
    context.venue.oco_error.reset();
    FakeContext later;
    later.venue.oco = true;
    Breakout again(params);
    Feed(again, later);
    again.OnFill(later, FillOf("order-1", Side::kBuy, "1", "13.01"));
    ASSERT_EQ(later.venue.ocos.size(), 1u);
    types::OrderStatus status;
    status.order_id = "order-2";
    status.state = types::OrderState::kRejected;
    again.OnOrderUpdate(later, status);
    status.order_id = "order-3";
    again.OnOrderUpdate(later, status);
    ASSERT_EQ(later.venue.orders.size(), 4u);
    EXPECT_EQ(later.venue.orders[3].type, types::OrderType::kStop);
    EXPECT_EQ(later.venue.ocos.size(), 1u);
}

TEST(Breakout, ARaisedStopPlacesThePairAgain) {
    auto params = Params();
    params.resting_target = true;
    params.breakeven_r = D("0.5");
    Breakout strategy(params);
    FakeContext context;
    context.venue.oco = true;
    Feed(strategy, context);
    strategy.OnFill(context, FillOf("order-1", Side::kBuy, "1", "13.01"));
    context.venue.cancel_result = trading::CancelOutcome::kCancelled;
    // 14.5 is past half the risk (2.03) above the entry, under the target.
    Feed(strategy, context, {"13.5", "14.6", "13.4", "14.5"});
    EXPECT_EQ(context.venue.cancels, std::vector<std::string>{"order-3"});
    ASSERT_EQ(context.venue.ocos.size(), 2u);
    EXPECT_EQ(context.venue.ocos[1].stop_price, D("13.01"));
    EXPECT_EQ(context.venue.ocos[1].limit_price, D("15.01"));
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

// The NR4 at hour 4 has true ranges 3, 5, 4 and 2 behind it: over 2 bars,
// seeded (3 + 5) / 2 = 4, then 4, then 3. An ATR stop one ATR under the
// broken level, 13, is at 10.
BreakoutParams AtrStop(const char* multiple) {
    auto p = Params();
    p.atr_stop = D(multiple);
    p.atr_bars = 2;
    return p;
}

TEST(Breakout, AnAtrStopSitsThatManyAtrsUnderTheBrokenLevel) {
    // In at 13.01 in hour 5; hour 6 dips to 10.5, through the setup-low stop
    // (10.98) but not the ATR one (10); hour 7 falls through 10.
    const auto data = Nr4Then({{"12.5", "14", "12.4", "13.8"},
                               {"13", "13.2", "10.5", "11"},
                               {"11", "11", "9.5", "10"}});
    const auto trades = Backtest(data, AtrStop("1"));
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[1].side, Side::kSell);
    EXPECT_EQ(trades[1].price, D("10"));
    // Half an ATR: 13 - 1.5 = 11.5, hit in hour 6.
    const auto tighter = Backtest(data, AtrStop("0.5"));
    ASSERT_EQ(tighter.size(), 2u);
    EXPECT_EQ(tighter[1].price, D("11.5"));
}

TEST(Breakout, BreakevenMeasuresTheRiskFromTheAtrStop) {
    // UpThenBack: in at 13.01, hour 6 closes at 15.1. Against the setup-low
    // stop (risk 2.03) that is one risk up, and breakeven sells at 13.01 in
    // hour 7; against the ATR stop at 10 (risk 3.01) it is not, so the
    // position is held through hour 7's dip to 12.
    auto params = AtrStop("1");
    params.target_multiple = D("0");
    params.breakeven_r = D("1");
    EXPECT_EQ(Backtest(UpThenBack(), params).size(), 1u);
}

TEST(Breakout, AnAtrStopWiderThanThePriceArmsNothing) {
    // 13 - 5 * 3 is below zero.
    EXPECT_TRUE(
        Backtest(Nr4Then({{"12.5", "14", "12.4", "13.8"}}), AtrStop("5"))
            .empty());
}

TEST(Breakout, AnAtrStopAsksForSixPeriodsOfWarmup) {
    auto params = Params();
    params.atr_stop = D("2");
    EXPECT_EQ(Breakout(params).Needs().warmup_bars, 85u);  // 6 * 14 + 1
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

TEST(Breakout, AnInverseHeadAndShouldersBuysAboveItsNeckline) {
    auto params = Params();
    params.setup = BreakoutParams::Setup::kInverseHeadShoulders;
    params.range_bars = 12;
    params.swing_bars = 1;
    Breakout strategy(params);
    FakeContext context;
    context.bars.clear();
    // Shoulders at 10 and 10.2, the head at 8, and a neckline falling from 15
    // to 14 (0.25 a bar): 12.75 on the bar after the newest, under that bar's
    // high of 12.9, which the entry goes above instead.
    for (const auto& b :
         std::vector<std::array<const char*, 4>>{{"13", "14", "12", "13"},
                                                 {"12", "12", "10", "11"},
                                                 {"11", "13", "11", "12"},
                                                 {"13", "15", "13", "14"},
                                                 {"14", "13", "11", "12"},
                                                 {"12", "11", "8", "9"},
                                                 {"9", "13", "10", "12"},
                                                 {"12", "14", "12", "13"},
                                                 {"13", "13", "11", "12"},
                                                 {"12", "12", "10.2", "11"},
                                                 {"11", "13", "11", "12"}}) {
        context.Add(b);
    }
    Feed(strategy, context, {"12", "12.9", "12.5", "12.8"});
    ASSERT_EQ(context.venue.orders.size(), 1u);
    EXPECT_EQ(context.venue.orders[0].type, types::OrderType::kStop);
    EXPECT_EQ(context.venue.orders[0].stop_price, D("12.91"));
    // Filled: the stop is under the head, 8 less 10 bps.
    strategy.OnFill(context, FillOf("order-1", Side::kBuy, "1", "12.91"));
    ASSERT_EQ(context.venue.orders.size(), 2u);
    EXPECT_EQ(context.venue.orders[1].stop_price, D("7.99"));
}

TEST(Breakout, AnInverseHeadAndShouldersRoundsItsNecklineUpToTheTick) {
    auto params = Params();
    params.setup = BreakoutParams::Setup::kInverseHeadShoulders;
    params.range_bars = 12;
    params.swing_bars = 1;
    Breakout strategy(params);
    FakeContext context;
    context.bars.clear();
    // A neckline rising from 14 to 15.01: 16.2725 at the next bar.
    for (const auto& b :
         std::vector<std::array<const char*, 4>>{{"13", "14", "12", "13"},
                                                 {"12", "12", "10", "11"},
                                                 {"11", "13", "11", "12"},
                                                 {"13", "14", "13", "13.5"},
                                                 {"13.5", "13", "11", "12"},
                                                 {"12", "11", "8", "9"},
                                                 {"9", "13", "10", "12"},
                                                 {"12", "15.01", "12", "13"},
                                                 {"13", "13", "11", "12"},
                                                 {"12", "12", "10.2", "11"},
                                                 {"11", "13", "11", "12"}}) {
        context.Add(b);
    }
    Feed(strategy, context, {"12", "14", "12.5", "13.5"});
    ASSERT_EQ(context.venue.orders.size(), 1u);
    EXPECT_EQ(context.venue.orders[0].stop_price, D("16.29"));  // 16.28 + tick
}

TEST(Breakout, AnAscendingTriangleBuysAboveItsResistance) {
    auto params = Params();
    params.setup = BreakoutParams::Setup::kTriangle;
    params.range_bars = 9;
    params.swing_bars = 1;
    Breakout strategy(params);
    FakeContext context;
    context.bars.clear();
    // Highs of 14.9 and 15, then lows rising from 11.5 to 12.5.
    for (const auto& b :
         std::vector<std::array<const char*, 4>>{{"12", "13", "11", "12"},
                                                 {"13", "15", "13", "14"},
                                                 {"13", "13", "10", "11"},
                                                 {"12", "14.9", "12", "14"},
                                                 {"13", "13.5", "11.5", "12"},
                                                 {"13", "15", "13", "14"},
                                                 {"14", "14", "12.5", "13"},
                                                 {"13", "14.5", "13", "14"}}) {
        context.Add(b);
    }
    Feed(strategy, context, {"14", "14.6", "13.5", "14.2"});
    ASSERT_EQ(context.venue.orders.size(), 1u);
    EXPECT_EQ(context.venue.orders[0].stop_price, D("15.01"));
    // Filled: the stop is under the newest swing low, 12.5 less 10 bps.
    strategy.OnFill(context, FillOf("order-1", Side::kBuy, "1", "15.01"));
    ASSERT_EQ(context.venue.orders.size(), 2u);
    EXPECT_EQ(context.venue.orders[1].stop_price, D("12.48"));
}

TEST(Breakout, ACupAndHandleBuysAboveItsRightRim) {
    auto params = Params();
    params.setup = BreakoutParams::Setup::kCupHandle;
    params.range_bars = 12;
    params.swing_bars = 1;
    Breakout strategy(params);
    FakeContext context;
    context.bars.clear();
    // Rims of 15 and 15.1, a rounded cup to 10, a handle down to 13.6.
    for (const auto& b : std::vector<std::array<const char*, 4>>{
             {"11", "12", "10", "11"},
             {"14", "15", "13", "14"},
             {"13", "14", "12", "13"},
             {"12", "13", "11", "12"},
             {"11", "12", "10.2", "11"},
             {"11", "11.8", "10", "11"},
             {"11", "12", "10.1", "11"},
             {"12", "13", "11", "12"},
             {"13", "14", "12", "13"},
             {"14", "15.1", "13.5", "14"},
             {"14", "14.5", "13.6", "14"}}) {
        context.Add(b);
    }
    Feed(strategy, context, {"14", "14.8", "14", "14.5"});
    ASSERT_EQ(context.venue.orders.size(), 1u);
    EXPECT_EQ(context.venue.orders[0].stop_price, D("15.11"));
    // Filled: the stop is under the handle, 13.6 less 10 bps.
    strategy.OnFill(context, FillOf("order-1", Side::kBuy, "1", "15.11"));
    ASSERT_EQ(context.venue.orders.size(), 2u);
    EXPECT_EQ(context.venue.orders[1].stop_price, D("13.58"));
}

TEST(Breakout, AFairValueGapBuysAboveTheThirdBarAndStopsUnderTheGap) {
    auto params = Params();
    params.setup = BreakoutParams::Setup::kFairValueGap;
    Breakout strategy(params);
    EXPECT_EQ(strategy.Needs().warmup_bars, 3u);
    FakeContext context;
    context.bars.clear();
    // Middle bar's high (17) exceeds the third bar's high (16): the stop-entry
    // still triggers one tick above the third bar's high (16.01), while the
    // height spans the full 3-bar range (17 - 10 = 7).
    context.Add({"10.5", "12", "10", "11.5"});  // first bar: high 12, low 10
    context.Add({"11.5", "17", "11", "15"});    // impulse bar: high 17
    Feed(strategy, context, {"15", "16", "13", "15.5"});  // low 13 > 12
    ASSERT_EQ(context.venue.orders.size(), 1u);
    EXPECT_EQ(context.venue.orders[0].type, types::OrderType::kStop);
    EXPECT_EQ(context.venue.orders[0].stop_price, D("16.01"));
    // Filled at 16.01: the stop is under the gap's bottom (12 less 10 bps =
    // 11.98), and the target is 16.01 + 7 = 23.01.
    strategy.OnFill(context, FillOf("order-1", Side::kBuy, "1", "16.01"));
    ASSERT_EQ(context.venue.orders.size(), 2u);
    EXPECT_EQ(context.venue.orders[1].stop_price, D("11.98"));
    context.venue.cancel_result = trading::CancelOutcome::kCancelled;
    Feed(strategy, context, {"16.5", "23.1", "16.5", "23.01"});
    ASSERT_EQ(context.venue.orders.size(), 3u);
    EXPECT_EQ(context.venue.orders[2].type, types::OrderType::kMarket);
    EXPECT_EQ(context.venue.orders[2].side, Side::kSell);
}

TEST(Breakout, AFairValueGapThrowbackRestsAtTheTopOfTheGapOnTheThirdBar) {
    auto params = Throwback();
    params.setup = BreakoutParams::Setup::kFairValueGap;
    Breakout strategy(params);
    FakeContext context;
    context.bars.clear();
    context.Add({"10.5", "12", "10", "11.5"});
    context.Add({"11.5", "15", "11", "14.5"});
    Feed(strategy, context, {"14.5", "16", "13", "15.5"});
    // The third bar's close (15.5) already cleared the gap's top (13): the
    // post-only limit buy at 13 rests at once, without waiting a fourth bar.
    ASSERT_EQ(context.venue.orders.size(), 1u);
    EXPECT_EQ(context.venue.orders[0].type, types::OrderType::kLimit);
    EXPECT_EQ(context.venue.orders[0].limit_price, D("13"));
    EXPECT_TRUE(context.venue.orders[0].post_only);
    // Filled on a pullback to 13: stop at 11.98 (under 12), target at 13 + 6
    // = 19.
    strategy.OnFill(context, FillOf("order-1", Side::kBuy, "1", "13"));
    ASSERT_EQ(context.venue.orders.size(), 2u);
    EXPECT_EQ(context.venue.orders[1].stop_price, D("11.98"));
    context.venue.cancel_result = trading::CancelOutcome::kCancelled;
    Feed(strategy, context, {"14", "19.2", "14", "19"});
    ASSERT_EQ(context.venue.orders.size(), 3u);
    EXPECT_EQ(context.venue.orders[2].type, types::OrderType::kMarket);
    EXPECT_EQ(context.venue.orders[2].side, Side::kSell);
}

TEST(Breakout, AFairValueGapNarrowerThanMinGapBpsIsNoSetup) {
    // The gap is 12 to 13: 1 / 13 of its top, about 769 bps.
    for (const auto& [bps, armed] : std::vector<std::pair<const char*, bool>>{
             {"760", true}, {"780", false}}) {
        auto params = Throwback();
        params.setup = BreakoutParams::Setup::kFairValueGap;
        params.min_gap_bps = D(bps);
        Breakout strategy(params);
        FakeContext context;
        context.bars.clear();
        context.Add({"10.5", "12", "10", "11.5"});
        context.Add({"11.5", "15", "11", "14.5"});
        Feed(strategy, context, {"14.5", "16", "13", "15.5"});
        EXPECT_EQ(context.venue.orders.size(), armed ? 1u : 0u) << bps;
    }
    auto bad = Throwback();
    bad.setup = BreakoutParams::Setup::kFairValueGap;
    bad.min_gap_bps = D("-1");
    EXPECT_THROW(Breakout{bad}, std::invalid_argument);
}

TEST(Breakout,
     AFairValueGapThrowbackKeepsArmBarsWhenTheThirdBarDoesNotConfirm) {
    // Third bar closes on its low (13, not above trigger 13.01): with the
    // default arm_bars = 1, the fourth bar still gets its bar to confirm.
    auto params = Throwback();
    params.setup = BreakoutParams::Setup::kFairValueGap;
    Breakout strategy(params);
    FakeContext context;
    context.bars.clear();
    context.Add({"10.5", "12", "10", "11.5"});
    context.Add({"11.5", "15", "11", "14.5"});
    Feed(strategy, context, {"14.5", "16", "13", "13"});
    EXPECT_TRUE(context.venue.orders.empty());
    // Fourth bar closes above 13.01 (and has low 12.5, so not a new FVG):
    // confirms and arms the throwback limit at 13.
    Feed(strategy, context, {"13", "14", "12.5", "13.5"});
    ASSERT_EQ(context.venue.orders.size(), 1u);
    EXPECT_EQ(context.venue.orders[0].type, types::OrderType::kLimit);
    EXPECT_EQ(context.venue.orders[0].limit_price, D("13"));

    // confirm = percent (0.5% above 13 -> trigger 13.07): a third close at
    // 13.05 does not confirm yet, and the fourth bar's close at 13.2 does.
    auto pct_params = params;
    pct_params.confirm = BreakoutParams::Confirm::kPercent;
    Breakout pct_strategy(pct_params);
    FakeContext pct_ctx;
    pct_ctx.bars.clear();
    pct_ctx.Add({"10.5", "12", "10", "11.5"});
    pct_ctx.Add({"11.5", "15", "11", "14.5"});
    Feed(pct_strategy, pct_ctx, {"14.5", "16", "13", "13.05"});
    EXPECT_TRUE(pct_ctx.venue.orders.empty());
    Feed(pct_strategy, pct_ctx, {"13.05", "13.5", "12.5", "13.2"});
    ASSERT_EQ(pct_ctx.venue.orders.size(), 1u);
    EXPECT_EQ(pct_ctx.venue.orders[0].limit_price, D("13"));
}

TEST(Breakout,
     AFairValueGapThrowbackCountsConfirmingClosesAndVolumeFromTheThirdBar) {
    // confirm = closes (2 closes): the third bar is close 1, the fourth is
    // close 2, and the throwback limit rests on the fourth bar.
    auto params = Throwback();
    params.setup = BreakoutParams::Setup::kFairValueGap;
    params.confirm = BreakoutParams::Confirm::kCloses;
    params.confirm_closes = 2;
    Breakout strategy(params);
    FakeContext context;
    context.bars.clear();
    context.Add({"10.5", "12", "10", "11.5"});
    context.Add({"11.5", "15", "11", "14.5"});
    Feed(strategy, context, {"14.5", "16", "13", "15.5"});
    EXPECT_TRUE(context.venue.orders.empty());
    Feed(strategy, context, {"15.5", "15.8", "13.5", "14"});
    ASSERT_EQ(context.venue.orders.size(), 1u);
    EXPECT_EQ(context.venue.orders[0].limit_price, D("13"));

    // volume_ratio: judged on the third bar when it confirms.
    auto vol_params = Throwback();
    vol_params.setup = BreakoutParams::Setup::kFairValueGap;
    vol_params.volume_ratio = D("2");
    vol_params.volume_bars = 2;
    FakeContext low_vol;
    low_vol.bars.clear();
    auto add_vol = [](FakeContext& ctx, std::array<const char*, 4> b,
                      const char* v) {
        ctx.Add(b);
        ctx.bars.back().volume = D(v);
        return ctx.bars.back();
    };
    add_vol(low_vol, {"10.5", "12", "10", "11.5"}, "100");
    add_vol(low_vol, {"11.5", "15", "11", "14.5"}, "100");
    Breakout low_strategy(vol_params);
    low_strategy.OnBar(
        low_vol, kSymbol, Timeframe::kH1,
        add_vol(low_vol, {"14.5", "16", "13", "15.5"}, "150"));  // < 2x 100
    EXPECT_TRUE(low_vol.venue.orders.empty());

    FakeContext high_vol;
    high_vol.bars.clear();
    add_vol(high_vol, {"10.5", "12", "10", "11.5"}, "100");
    add_vol(high_vol, {"11.5", "15", "11", "14.5"}, "100");
    Breakout high_strategy(vol_params);
    high_strategy.OnBar(
        high_vol, kSymbol, Timeframe::kH1,
        add_vol(high_vol, {"14.5", "16", "13", "15.5"}, "200"));  // >= 2x 100
    ASSERT_EQ(high_vol.venue.orders.size(), 1u);
    EXPECT_EQ(high_vol.venue.orders[0].limit_price, D("13"));
}

TEST(Breakout, ADailyFairValueGapThrowbackRestsFromTheNextOpen) {
    // Days 3, 4, 5 form a fair value gap (day 3 high 14, day 5 low 14.5, close
    // 15.5): the throwback limit at 14.5 pends at day 5's midnight close,
    // goes in at day 6's open (15, above 14.5), and fills as a maker on day
    // 6's dip to 14.2.
    auto params = Throwback();
    params.setup = BreakoutParams::Setup::kFairValueGap;
    const auto result =
        Daily({{"14.5", "16", "14.5", "15.5"}, {"15", "15.2", "14.2", "14.8"}},
              7, params);
    ASSERT_EQ(result.fills.size(), 1u);
    EXPECT_EQ(result.fills[0].side, Side::kBuy);
    EXPECT_EQ(result.fills[0].price, D("14.5"));
    EXPECT_EQ(result.fills[0].liquidity, types::Liquidity::kMaker);
}

TEST(Breakout, RefusesBadParameters) {
    auto bad = Params();
    bad.quantity = D("0");
    EXPECT_THROW(Breakout{bad}, std::invalid_argument);
    bad = Params();
    bad.atr_stop = D("-1");
    EXPECT_THROW(Breakout{bad}, std::invalid_argument);
    bad = Params();
    bad.atr_bars = 0;
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
    bad.setup = BreakoutParams::Setup::kInverseHeadShoulders;
    EXPECT_THROW(Breakout{bad}, std::invalid_argument);  // needs 11
    bad.range_bars = 11;
    EXPECT_NO_THROW(Breakout{bad});
    bad.setup = BreakoutParams::Setup::kTriangle;
    bad.range_bars = 8;  // TriangleBars(2, 2) is 9
    EXPECT_THROW(Breakout{bad}, std::invalid_argument);
    bad.range_bars = 9;
    EXPECT_NO_THROW(Breakout{bad});
    bad.min_touches = 1;
    EXPECT_THROW(Breakout{bad}, std::invalid_argument);
    bad = Params();
    bad.setup = BreakoutParams::Setup::kCupHandle;
    bad.range_bars = 9;  // CupHandleBars(2) is 10
    EXPECT_THROW(Breakout{bad}, std::invalid_argument);
    bad.range_bars = 10;
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
    bad = Params();
    bad.risk_fraction = D("-0.01");
    EXPECT_THROW(Breakout{bad}, std::invalid_argument);
    bad.risk_fraction = D("0.11");  // above 0.1
    EXPECT_THROW(Breakout{bad}, std::invalid_argument);
    bad.risk_fraction = D("0.1");
    EXPECT_NO_THROW(Breakout{bad});
    bad.risk_per_trade = D("10");  // exclusive with risk_fraction
    EXPECT_THROW(Breakout{bad}, std::invalid_argument);
    bad = Params();
    bad.allocation = D("1000");  // needs risk_fraction
    EXPECT_THROW(Breakout{bad}, std::invalid_argument);
    bad.risk_fraction = D("0.02");
    EXPECT_NO_THROW(Breakout{bad});
    bad.allocation = D("-1");
    EXPECT_THROW(Breakout{bad}, std::invalid_argument);
}

}  // namespace
// Runs the backtests of a test with shorting allowed.
struct AllowShort {
    AllowShort() { gAllowShort = true; }
    ~AllowShort() { gAllowShort = false; }
    AllowShort(const AllowShort&) = delete;
    AllowShort& operator=(const AllowShort&) = delete;
};

BreakoutParams ShortParams() {
    auto p = Params();
    p.direction = BreakoutParams::Direction::kShort;
    return p;
}

TEST(BreakoutShort, SellsTheBreakDownAndCoversAtTheTarget) {
    AllowShort shorts;
    // Nr4's first test upside down: hour 5 breaks 11.99, hour 6 closes at
    // 9.8, past the target 9.99, and hour 7 opens at 10.
    const auto trades =
        Backtest(MirroredNr4Then({{"12.5", "14", "12.4", "13.8"},
                                  {"13.8", "15.5", "13.5", "15.2"},
                                  {"15", "15", "15", "15"}}),
                 ShortParams());
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].side, Side::kSell);
    EXPECT_EQ(trades[0].price, D("11.99"));
    EXPECT_EQ(trades[1].side, Side::kBuy);
    EXPECT_EQ(trades[1].price, D("10"));
}

TEST(BreakoutShort, TheProtectiveStopBuysAboveTheSetup) {
    AllowShort shorts;
    const auto trades =
        Backtest(MirroredNr4Then({{"12.5", "14", "12.4", "13.8"},
                                  {"13", "13.2", "10.5", "11"},
                                  {"11", "11", "11", "11"}}),
                 ShortParams());
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[1].side, Side::kBuy);
    EXPECT_EQ(trades[1].price, D("14.02"));
}

TEST(BreakoutShort, AVenueThatCannotShortRefusesTheEntry) {
    const auto trades =
        Backtest(MirroredNr4Then({{"12.5", "14", "12.4", "13.8"},
                                  {"13.8", "15.5", "13.5", "15.2"},
                                  {"15", "15", "15", "15"}}),
                 ShortParams());
    EXPECT_TRUE(trades.empty());
}

TEST(BreakoutShort, TheTrendFilterKeepsItOutAboveTheAverage) {
    AllowShort shorts;
    auto params = ShortParams();
    params.trend_sma = 3;  // the setup bar closes at 13, the average is 13
    const auto trades =
        Backtest(MirroredNr4Then({{"12.5", "14", "12.4", "13.8"},
                                  {"13.8", "13.8", "13.8", "13.8"}}),
                 params);
    EXPECT_TRUE(trades.empty());
}

TEST(BreakoutShort, ARestingTargetBuysWhereThePriceReachesIt) {
    AllowShort shorts;
    auto params = ShortParams();
    params.resting_target = true;
    const auto trades =
        Backtest(MirroredNr4Then({{"12.5", "14", "12.4", "13.8"},
                                  {"13.8", "15.5", "13.5", "15.2"},
                                  {"15", "15", "15", "15"}}),
                 params);
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[1].side, Side::kBuy);
    EXPECT_EQ(trades[1].price, D("9.99"));
}

TEST(BreakoutShort, AFailedLongReversesOnceIntoAShort) {
    AllowShort shorts;
    auto params = Params();
    params.reverse = true;
    // Long at 13.01 in hour 5; hour 6 falls through the stop at 10.98 and
    // the short is sold there, with its stop above the NR4's high (13, and
    // 10 bps: 13.02). Hour 7 rises through that: covered, and not reversed
    // again.
    const auto trades = Backtest(Nr4Then({{"12.5", "14", "12.4", "13.8"},
                                          {"13", "13.2", "10.5", "11"},
                                          {"11", "13.5", "11", "13.5"},
                                          {"13.5", "13.5", "13.5", "13.5"}}),
                                 params, 9);
    ASSERT_EQ(trades.size(), 4u);
    EXPECT_EQ(trades[0].side, Side::kBuy);
    EXPECT_EQ(trades[0].price, D("13.01"));
    EXPECT_EQ(trades[1].side, Side::kSell);
    EXPECT_EQ(trades[1].price, D("10.98"));
    EXPECT_EQ(trades[2].side, Side::kSell);
    EXPECT_EQ(trades[2].price, D("10.98"));
    EXPECT_EQ(trades[3].side, Side::kBuy);
    EXPECT_EQ(trades[3].price, D("13.02"));

    // Reversal sized by risk_fraction uses the post-stop balance and the stop
    // fill price (10.98 -> stop 13.02, distance 2.04): 10,000 USD at 0.02
    // risks 200 USD (98.5221 units), loses ~200 USD on the stop, then risks
    // 0.02 of ~9,800 USD = 196 USD (96.0784 units) on the reversal.
    auto rf = params;
    rf.quantity = D("200");
    rf.risk_fraction = D("0.02");
    auto rf_res =
        backtest::RunBacktest(Nr4Then({{"12.5", "14", "12.4", "13.8"},
                                       {"13", "13.2", "10.5", "11"},
                                       {"11", "13.5", "11", "13.5"},
                                       {"13.5", "13.5", "13.5", "13.5"}}),
                              std::make_unique<Breakout>(rf), Options(9));
    ASSERT_OK(rf_res);
    ASSERT_EQ(rf_res->fills.size(), 4u);
    EXPECT_EQ(rf_res->fills[0].quantity, D("98.5221"));
    EXPECT_EQ(rf_res->fills[2].quantity, D("96.0784"));
}

TEST(BreakoutShort, AFailedShortReversesIntoALong) {
    AllowShort shorts;
    auto params = ShortParams();
    params.reverse = true;
    // Short at 11.99; hour 6 rises through its stop at 14.02 and the long is
    // bought there, its stop below the NR4's low upside down (12, less 10
    // bps: 11.98). Hour 7 falls through that.
    const auto trades =
        Backtest(MirroredNr4Then({{"12.5", "14", "12.4", "13.8"},
                                  {"13", "13.2", "10.5", "11"},
                                  {"11", "13.5", "11", "13.5"},
                                  {"13.5", "13.5", "13.5", "13.5"}}),
                 params, 9);
    ASSERT_EQ(trades.size(), 4u);
    EXPECT_EQ(trades[1].side, Side::kBuy);
    EXPECT_EQ(trades[1].price, D("14.02"));
    EXPECT_EQ(trades[2].side, Side::kBuy);
    EXPECT_EQ(trades[2].price, D("14.02"));
    EXPECT_EQ(trades[3].side, Side::kSell);
    EXPECT_EQ(trades[3].price, D("11.98"));
}

TEST(BreakoutShort, ATargetOrARaisedStopDoesNotReverse) {
    AllowShort shorts;
    auto params = Params();
    params.reverse = true;
    const auto target = Backtest(Nr4Then({{"12.5", "14", "12.4", "13.8"},
                                          {"13.8", "15.5", "13.5", "15.2"},
                                          {"15", "15", "15", "15"}}),
                                 params);
    EXPECT_EQ(target.size(), 2u);
    // Breakeven at 1R (2.03 above the entry): hour 5 closes at 15.1, the
    // stop goes to 13.01, and hour 6 falls through it.
    params.breakeven_r = D("1");
    params.target_multiple = D("0");
    const auto raised = Backtest(Nr4Then({{"12.5", "15.1", "12.4", "15.1"},
                                          {"15", "15", "12", "12"},
                                          {"12", "12", "12", "12"}}),
                                 params);
    ASSERT_EQ(raised.size(), 2u);
    EXPECT_EQ(raised[1].price, D("13.01"));
}

TEST(BreakoutShort, AFairValueGapShortsTheBounceAndReversesAnInversionGap) {
    AllowShort shorts;
    // Bearish FVG on hours 2..4 (mirrored about 25 from a bullish FVG: hour 2
    // low 13, hour 4 high 12, close 9.5): throwback rests a post-only sell
    // limit at 12 (the bottom of the bearish gap), with the protective stop
    // above 13 (13.02). Hour 5 bounces to 12.2 (fills at 12 as a maker), and
    // hour 6 drops to 5.8, closing at 6 (the target: 12 - 6 = 6), covered at
    // hour 7's open.
    auto short_fvg = ShortParams();
    short_fvg.setup = BreakoutParams::Setup::kFairValueGap;
    short_fvg.entry = BreakoutParams::Entry::kThrowback;
    const auto trades = Backtest(Mirrored({{"12", "14", "10", "12"},
                                           {"12", "13", "10", "12"},
                                           {"10.5", "12", "10", "11.5"},
                                           {"11.5", "15", "11", "14.5"},
                                           {"14.5", "16", "13", "15.5"},
                                           {"15.5", "15.8", "12.8", "14.5"},
                                           {"14.5", "19.2", "14.5", "19"},
                                           {"19", "19", "19", "19"}}),
                                 short_fvg);
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].side, Side::kSell);
    EXPECT_EQ(trades[0].price, D("12"));
    EXPECT_EQ(trades[1].side, Side::kBuy);
    EXPECT_EQ(trades[1].price, D("6"));
    // min_gap_bps on the negated prices: the gap is 12 to 13, 1 / 12 of its
    // bottom (the mirrored top), about 833 bps.
    for (const auto& [bps, count] :
         std::vector<std::pair<const char*, std::size_t>>{{"820", 2},
                                                          {"840", 0}}) {
        auto gapped = short_fvg;
        gapped.min_gap_bps = D(bps);
        EXPECT_EQ(Backtest(Mirrored({{"12", "14", "10", "12"},
                                     {"12", "13", "10", "12"},
                                     {"10.5", "12", "10", "11.5"},
                                     {"11.5", "15", "11", "14.5"},
                                     {"14.5", "16", "13", "15.5"},
                                     {"15.5", "15.8", "12.8", "14.5"},
                                     {"14.5", "19.2", "14.5", "19"},
                                     {"19", "19", "19", "19"}}),
                           gapped)
                      .size(),
                  count)
            << bps;
    }

    // Inversion FVG (long throwback with reverse = true): hours 3..5 form a
    // gap from 14 to 15; hour 6 fills the limit buy at 15 and falls through
    // the protective stop under 14 (13.98), reversing into a short at 13.98
    // with its protective stop above the gap's top (15 + 10 bps = 15.02),
    // which hour 7 hits.
    auto inv = Throwback();
    inv.setup = BreakoutParams::Setup::kFairValueGap;
    inv.reverse = true;
    const auto reversed = Backtest(Nr4Then({{"12.5", "17", "15", "16.5"},
                                            {"16", "16.2", "13.5", "13.8"},
                                            {"13.8", "15.5", "13.8", "15.2"},
                                            {"15.2", "15.2", "15.2", "15.2"}}),
                                   inv, 9);
    ASSERT_EQ(reversed.size(), 4u);
    EXPECT_EQ(reversed[0].side, Side::kBuy);
    EXPECT_EQ(reversed[0].price, D("15"));
    EXPECT_EQ(reversed[1].side, Side::kSell);
    EXPECT_EQ(reversed[1].price, D("13.98"));
    EXPECT_EQ(reversed[2].side, Side::kSell);
    EXPECT_EQ(reversed[2].price, D("13.98"));
    EXPECT_EQ(reversed[3].side, Side::kBuy);
    EXPECT_EQ(reversed[3].price, D("15.02"));
}

}  // namespace kestrel::strategy
