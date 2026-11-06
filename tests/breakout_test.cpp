#include "kestrel/strategy/breakout.hpp"

#include <gtest/gtest.h>

#include <array>
#include <memory>
#include <vector>

#include "kestrel/backtest/backtest.hpp"
#include "kestrel/backtest/replay_data_source.hpp"
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
    // target 13.01 + 2 = 15.01: sold at that close.
    const auto trades = Backtest(Nr4Then({{"12.5", "14", "12.4", "13.8"},
                                          {"13.8", "15.5", "13.5", "15.2"},
                                          {"15", "15", "15", "15"}}),
                                 Params());
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].side, Side::kBuy);
    EXPECT_EQ(trades[0].price, D("13.01"));
    EXPECT_EQ(trades[1].side, Side::kSell);
    EXPECT_EQ(trades[1].price, D("15.2"));
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
    EXPECT_EQ(trades[1].price, D("13.9"));  // hour 6's close
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
// the exit waits (the stop resting overnight) and goes at the close of the
// next session's first bar, hour 28's (04:00 to 05:00 the next day).
TEST(Breakout, ATargetReachedAfterTheCloseExitsInTheNextSession) {
    std::vector<std::array<const char*, 4>> after = {
        {"12.5", "14", "12.4", "13.8"}, {"13.8", "15.5", "13.5", "15.2"}};
    for (int h = 7; h <= 28; ++h) {
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
}

}  // namespace
}  // namespace kestrel::strategy
