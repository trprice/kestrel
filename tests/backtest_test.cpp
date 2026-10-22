#include "kestrel/backtest/backtest.hpp"

#include "kestrel/strategy/sma_crossover.hpp"
#include "test_helpers.hpp"

namespace {

using namespace kestrel;
using backtest::BacktestOptions;
using backtest::ReplayDataSource;
using backtest::RunBacktest;
using kestrel::testing::D;
using types::ErrorCategory;
using types::Side;
using types::Timeframe;

constexpr const char* kSymbol = "BTC/USD";

types::Timestamp Hour(int h) { return types::FromEpochSeconds(h * 3600); }

types::Bar BarAt(int hour, const char* close) {
    types::Bar bar;
    bar.open_time = Hour(hour);
    bar.open = bar.high = bar.low = bar.close = D(close);
    return bar;
}

// Hourly bars; the index in `closes` is the hour.
std::vector<types::Bar> Series(const std::vector<const char*>& closes) {
    std::vector<types::Bar> bars;
    for (std::size_t i = 0; i < closes.size(); ++i) {
        bars.push_back(BarAt(static_cast<int>(i), closes[i]));
    }
    return bars;
}

std::shared_ptr<ReplayDataSource> Data(const std::vector<const char*>& closes) {
    auto data = std::make_shared<ReplayDataSource>();
    data->AddBars(kSymbol, Timeframe::kH1, Series(closes));
    data->SetSymbolInfo({kSymbol, D("0.01"), D("0.00000001"), D("0.0001")});
    return data;
}

std::unique_ptr<strategy::SmaCrossover> Sma() {
    strategy::SmaCrossoverParams p;
    p.symbol = kSymbol;
    p.timeframe = Timeframe::kH1;
    p.fast_period = 2;
    p.slow_period = 3;
    p.quantity = D("0.5");
    return std::make_unique<strategy::SmaCrossover>(p);
}

// Hours 0-3 are warmup. Fast crosses above slow when hour 5 closes at 20, and
// back below when hour 7 closes at 5.
const std::vector<const char*> kCloses = {"10", "10", "10", "10",
                                          "10", "20", "20", "5"};

BacktestOptions Options() {
    BacktestOptions o;
    o.start = Hour(4);
    o.end = Hour(8);
    o.paper.slippage = D("0");
    o.paper.maker_fee_rate = D("0");
    o.paper.taker_fee_rate = D("0");
    return o;
}

TEST(ReplayDataSourceTest, GetBarsNeverShowsTheFuture) {
    auto data = Data(kCloses);
    auto all = data->GetBars(kSymbol, Timeframe::kH1, Hour(0), Hour(8));
    ASSERT_OK(all);
    EXPECT_EQ(all->size(), 8u);  // no clock set yet

    data->SetTime(Hour(5));  // hour 4's bar closes exactly now
    auto seen = data->GetBars(kSymbol, Timeframe::kH1, Hour(0), Hour(8));
    ASSERT_OK(seen);
    ASSERT_EQ(seen->size(), 5u);
    EXPECT_EQ(seen->back().open_time, Hour(4));

    data->SetTime(Hour(5) + std::chrono::minutes(30));  // hour 5 still forming
    seen = data->GetBars(kSymbol, Timeframe::kH1, Hour(0), Hour(8));
    ASSERT_OK(seen);
    EXPECT_EQ(seen->size(), 5u);
}

TEST(ReplayDataSourceTest, EmitReachesOnlyMatchingSubscriptions) {
    auto data = Data(kCloses);
    std::vector<types::Timestamp> got;
    auto sub = data->StreamBars(
        {kSymbol}, Timeframe::kH1,
        [&](const types::Symbol&, Timeframe, const types::Bar& b) {
            got.push_back(b.open_time);
        });
    ASSERT_OK(sub);
    EXPECT_TRUE(data->Emit(kSymbol, Timeframe::kH1, BarAt(1, "1")));
    EXPECT_FALSE(data->Emit("ETH/USD", Timeframe::kH1, BarAt(2, "1")));
    EXPECT_FALSE(data->Emit(kSymbol, Timeframe::kM5, BarAt(3, "1")));
    EXPECT_EQ(got, std::vector<types::Timestamp>{Hour(1)});

    (*sub)->Stop();
    EXPECT_FALSE(data->Emit(kSymbol, Timeframe::kH1, BarAt(4, "1")));
}

TEST(ReplayDataSourceTest, SortsBarsAndHasNoQuotes) {
    ReplayDataSource data;
    data.AddBars(kSymbol, Timeframe::kH1, {BarAt(2, "2"), BarAt(1, "1")});
    EXPECT_EQ(data.Series(kSymbol, Timeframe::kH1).front().open_time, Hour(1));
    EXPECT_TRUE(data.Series("ETH/USD", Timeframe::kH1).empty());

    auto q = data.StreamQuotes({kSymbol}, [](const types::Quote&) {});
    ASSERT_FALSE(q);
    EXPECT_EQ(q.error().category, ErrorCategory::kNotImplemented);
    auto info = data.GetSymbolInfo(kSymbol);
    ASSERT_FALSE(info);
    EXPECT_EQ(info.error().category, ErrorCategory::kNotFound);
}

TEST(BacktestTest, RunsASmaCrossoverEndToEnd) {
    auto result = RunBacktest(Data(kCloses), Sma(), Options());
    ASSERT_OK(result);

    EXPECT_EQ(result->bars_replayed, 4u);
    ASSERT_EQ(result->fills.size(), 2u);
    EXPECT_EQ(result->fills[0].side, Side::kBuy);
    EXPECT_EQ(result->fills[0].price, D("20"));  // hour 5's close
    EXPECT_EQ(result->fills[0].time, Hour(6));
    EXPECT_EQ(result->fills[0].quantity, D("0.5"));
    EXPECT_EQ(result->fills[1].side, Side::kSell);
    EXPECT_EQ(result->fills[1].price, D("5"));  // hour 7's close
    EXPECT_EQ(result->fills[1].time, Hour(8));

    // The paper venue reports open positions only, so the closed trade shows
    // up in the cash balance: 0.5 * (5 - 20) = -7.5.
    EXPECT_TRUE(result->positions.empty());

    bool found_usd = false;
    for (const auto& balance : result->balances) {
        if (balance.asset == "USD") {
            found_usd = true;
            EXPECT_EQ(balance.total, D("9992.5"));
        }
    }
    EXPECT_TRUE(found_usd);
}

TEST(BacktestTest, AppliesSlippageAndFees) {
    auto options = Options();
    options.paper.slippage = D("0.01");
    options.paper.taker_fee_rate = D("0.001");
    auto result = RunBacktest(Data(kCloses), Sma(), options);
    ASSERT_OK(result);
    ASSERT_EQ(result->fills.size(), 2u);
    EXPECT_EQ(result->fills[0].price, D("20.2"));  // ask * 1.01
    EXPECT_EQ(result->fills[0].fee, D("0.0101"));  // 0.1% of 10.1
    EXPECT_EQ(result->fills[1].price, D("4.95"));  // bid * 0.99
}

TEST(BacktestTest, IsRepeatable) {
    auto a = RunBacktest(Data(kCloses), Sma(), Options());
    auto b = RunBacktest(Data(kCloses), Sma(), Options());
    ASSERT_OK(a);
    ASSERT_OK(b);
    ASSERT_EQ(a->fills.size(), b->fills.size());
    for (std::size_t i = 0; i < a->fills.size(); ++i) {
        EXPECT_EQ(a->fills[i].price, b->fills[i].price);
        EXPECT_EQ(a->fills[i].time, b->fills[i].time);
    }
}

TEST(BacktestTest, OnlyReplaysTheRequestedRange) {
    auto options = Options();
    options.start = Hour(6);  // the up-cross at hour 5 happens before this
    auto result = RunBacktest(Data(kCloses), Sma(), options);
    ASSERT_OK(result);
    EXPECT_EQ(result->bars_replayed, 2u);
    EXPECT_TRUE(result->fills.empty());
}

TEST(BacktestTest, TheRiskGateAppliesInBacktests) {
    auto options = Options();
    options.risk = trading::RiskLimits{};
    options.risk->max_order_notional = D("5");  // the buy is 0.5 * 20 = 10
    auto result = RunBacktest(Data(kCloses), Sma(), options);
    ASSERT_OK(result);
    EXPECT_TRUE(result->fills.empty());
}

TEST(BacktestTest, FailsWithoutSymbolInfo) {
    auto data = std::make_shared<ReplayDataSource>();
    data->AddBars(kSymbol, Timeframe::kH1, Series(kCloses));
    // The buy is rejected for lack of symbol info, so nothing fills.
    auto result = RunBacktest(data, Sma(), Options());
    ASSERT_OK(result);
    EXPECT_TRUE(result->fills.empty());
}

class QuoteStrategy : public strategy::Strategy {
   public:
    std::string_view Name() const override { return "quotes"; }
    strategy::Requirements Needs() const override {
        strategy::Requirements r;
        r.symbols = {kSymbol};
        r.quotes = true;
        return r;
    }
};

class MonthlyStrategy : public QuoteStrategy {
   public:
    strategy::Requirements Needs() const override {
        strategy::Requirements r;
        r.symbols = {kSymbol};
        r.bar_timeframes = {Timeframe::kMo1};
        return r;
    }
};

TEST(BacktestTest, RejectsStrategiesItCannotReplay) {
    auto quotes = RunBacktest(Data(kCloses), std::make_unique<QuoteStrategy>(),
                              Options());
    ASSERT_FALSE(quotes);
    EXPECT_EQ(quotes.error().category, ErrorCategory::kUnsupported);

    auto monthly = RunBacktest(Data(kCloses),
                               std::make_unique<MonthlyStrategy>(), Options());
    ASSERT_FALSE(monthly);
    EXPECT_EQ(monthly.error().category, ErrorCategory::kUnsupported);
}

}  // namespace
