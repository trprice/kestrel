#include "kestrel/strategy/trend_ensemble.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "kestrel/backtest/backtest.hpp"
#include "kestrel/backtest/replay_data_source.hpp"
#include "kestrel/strategy/patterns.hpp"
#include "strategy_fakes.hpp"
#include "test_helpers.hpp"

namespace kestrel::strategy {
namespace {

using backtest::ReplayDataSource;
using kestrel::testing::D;
using types::Decimal;
using types::Side;
using types::Timeframe;

types::Timestamp Day(int d) {
    return types::FromEpochSeconds(std::int64_t{d} * 86400);
}

// Daily bars from closes, day 0 first, each opening at the close before it.
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

// Two models (3 and 5 bars), sized by 3 bars of returns. A target_vol far
// above any volatility here makes each model's weight max_weight (1), so a
// symbol's target is its share (half the equity) times its models long.
TrendEnsembleParams Params() {
    TrendEnsembleParams p;
    p.symbols = {"A/USD", "B/USD"};
    p.lookbacks = {3, 5};
    p.vol_bars = 3;
    p.target_vol = D("100");
    p.invest_fraction = D("1");
    return p;
}

// A goes long on the 3-bar model on day 2 (a flat window's close is its
// highest) and on the 5-bar one on day 4, then falls back; B only falls.
std::vector<Series> Trending() {
    return {
        {"A/USD", {"10", "10", "10", "11", "12", "13", "12", "11", "10", "9"}},
        {"B/USD", {"10", "9", "8", "7", "6", "5", "4", "3", "2", "1"}}};
}

// A context over fixed daily bars and a RecordingVenue.
class FakeContext : public Context {
   public:
    types::Timestamp Now() const override { return Day(day); }
    trading::TradingVenue& Venue() override { return venue; }
    types::Result<types::SymbolInfo> SymbolInfo(
        const types::Symbol& symbol) override {
        return types::SymbolInfo{symbol, D("0.01"), D("0.001"), D("0.01"),
                                 D("1")};
    }
    // Up to the symbol's last bar delivered (or, before any, the start
    // day), as the runner's window would hold.
    std::span<const types::Bar> Bars(const types::Symbol& symbol,
                                     Timeframe) const override {
        const auto it = bars.find(symbol);
        if (it == bars.end()) {
            return {};
        }
        const auto seen = delivered.find(symbol);
        const auto upto = seen == delivered.end() ? Day(day) : seen->second;
        std::size_t n = 0;
        while (n < it->second.size() && it->second[n].open_time <= upto) {
            ++n;
        }
        return std::span<const types::Bar>(it->second.data(), n);
    }
    void Set(const std::vector<Series>& series) {
        for (const auto& s : series) {
            bars[s.symbol] = Daily(s.closes);
        }
    }
    // Delivers each symbol's bar for `d`.
    void Close(TrendEnsemble& strategy, int d) {
        day = d;
        for (const auto& [symbol, series] : bars) {
            for (const auto& bar : series) {
                if (bar.open_time == Day(d)) {
                    delivered[symbol] = bar.open_time;
                    strategy.OnBar(*this, symbol, Timeframe::kD1, bar);
                }
            }
        }
    }
    Decimal Close(const types::Symbol& symbol) const {
        return Bars(symbol, Timeframe::kD1).back().close;
    }
    void SetBalance(const std::string& asset, const Decimal& amount) {
        for (auto& balance : venue.balances) {
            if (balance.asset == asset) {
                balance.total = balance.available = amount;
                return;
            }
        }
        venue.balances.push_back({asset, amount, amount, Decimal()});
    }
    Decimal BalanceOf(const std::string& asset) const {
        for (const auto& balance : venue.balances) {
            if (balance.asset == asset) {
                return balance.total;
            }
        }
        return Decimal();
    }

    kestrel::testing::RecordingVenue venue;
    std::map<types::Symbol, std::vector<types::Bar>> bars;
    std::map<types::Symbol, types::Timestamp> delivered;
    int day = 0;
};

struct Live {
    TrendEnsemble strategy;
    FakeContext context;
    std::size_t filled = 0;  // orders filled so far

    explicit Live(const std::vector<Series>& series,
                  TrendEnsembleParams params = Params(), int start = 0)
        : strategy(std::move(params)) {
        context.Set(series);
        context.SetBalance("USD", D("1000"));
        context.day = start;
        strategy.OnStart(context);
    }
    // Fills order `i` at the day's close, as a venue would, and moves cash.
    void Fill(std::size_t i) {
        // A copy: OnFill can place orders, which may move the vector.
        const types::OrderRequest order = context.venue.orders.at(i);
        const std::string id = "order-" + std::to_string(i + 1);
        const Decimal price = context.Close(order.symbol);
        types::Fill fill;
        fill.order_id = id;
        fill.symbol = order.symbol;
        fill.side = order.side;
        fill.quantity = order.quantity;
        fill.price = price;
        fill.fee_asset = "USD";
        const Decimal notional = order.quantity * price;
        const std::string base = order.symbol.substr(0, 1);
        const bool buy = order.side == Side::kBuy;
        context.SetBalance(
            "USD", context.BalanceOf("USD") + (buy ? -notional : notional));
        context.SetBalance(base, context.BalanceOf(base) +
                                     (buy ? order.quantity : -order.quantity));
        strategy.OnFill(context, fill);
        types::OrderStatus status;
        status.order_id = id;
        status.symbol = order.symbol;
        status.state = types::OrderState::kFilled;
        status.filled_quantity = order.quantity;
        strategy.OnOrderUpdate(context, status);
    }
    // Closes day d and fills every order it (and the fills) placed.
    void CloseDay(int d) {
        context.Close(strategy, d);
        while (filled < context.venue.orders.size()) {
            Fill(filled++);
        }
    }
    Decimal Equity() const {
        Decimal equity = context.BalanceOf("USD");
        for (const auto* symbol : {"A/USD", "B/USD"}) {
            equity += context.BalanceOf(std::string(symbol).substr(0, 1)) *
                      context.Close(symbol);
        }
        return equity;
    }
};

// Each model goes long on a close at its channel's highest close and stays
// long until a close at or below its trailing stop, which starts at the
// channel's midpoint and only rises:
// - 3 bars: long on day 2 (stop 10), raised to 10.5, 11 and 12 on days 3-5;
//   day 6 closes at 12, at the stop: out.
// - 5 bars: long on day 4 (stop 11), raised to 11.5 on day 5; day 6's
//   midpoint is 11.5 too; day 7 closes at 11, under it: out.
TEST(TrendEnsemble, GoesLongOnANewHighAndOutOnTheTrailingStop) {
    Live live(Trending());
    const std::vector<const char*> expected = {"0", "0",   "0.5", "0.5", "1",
                                               "1", "0.5", "0",   "0",   "0"};
    for (int d = 1; d < 10; ++d) {
        live.CloseDay(d);
        EXPECT_EQ(live.strategy.Conviction("A/USD"), D(expected[d]))
            << "day " << d;
        EXPECT_EQ(live.strategy.Conviction("B/USD"), D("0")) << "day " << d;
    }
}

// Started on day 5, the models replay the bars before it and agree with a
// run that never stopped (here the replay has every bar; at startup the
// runner gives it three times the longest lookback).
TEST(TrendEnsemble, ReplaysTheWarmupBarsAtStart) {
    Live live(Trending(), Params(), 5);
    EXPECT_EQ(live.strategy.Conviction("A/USD"), D("1"));
    live.CloseDay(6);
    EXPECT_EQ(live.strategy.Conviction("A/USD"), D("0.5"));
    live.CloseDay(7);
    EXPECT_EQ(live.strategy.Conviction("A/USD"), D("0"));
}

// Day 3 is the first with a volatility (3 returns): A's 3-bar model is long,
// so A's target is a quarter of the equity. Day 4 adds the 5-bar model, and
// it buys up to half.
TEST(TrendEnsemble, SizesEachSymbolByItsModelsLong) {
    Live live(Trending());
    live.CloseDay(1);
    live.CloseDay(2);
    EXPECT_TRUE(live.context.venue.orders.empty());  // no volatility yet
    live.CloseDay(3);
    ASSERT_EQ(live.context.venue.orders.size(), 1u);
    const auto& first = live.context.venue.orders[0];
    EXPECT_EQ(first.symbol, "A/USD");
    EXPECT_EQ(first.side, Side::kBuy);
    EXPECT_EQ(first.type, types::OrderType::kMarket);
    EXPECT_EQ(first.quantity, D("22.727"));  // 250 / 11, to the lot
    const Decimal equity = live.Equity();
    live.CloseDay(4);
    ASSERT_EQ(live.context.venue.orders.size(), 2u);
    const auto& second = live.context.venue.orders[1];
    EXPECT_EQ(second.side, Side::kBuy);
    // Half the equity at day 4's close, less what is held.
    const Decimal target = (equity + D("22.727")) * D("0.5");  // A rose 1
    const Decimal want = Decimal::Divide(target - D("22.727") * D("12"),
                                         D("12"), 3, types::Rounding::kDown);
    EXPECT_EQ(second.quantity, want);
}

// With the signal unchanged, a holding is resized only once it is more than
// rebalance_threshold (20%) off its target.
TEST(TrendEnsemble, ResizesOnlyPastTheThreshold) {
    Live live({{"A/USD", {"10", "10", "10", "11", "12", "13", "20", "21"}},
               {"B/USD", {"10", "9", "8", "7", "6", "5", "4", "3"}}});
    for (int d = 1; d <= 4; ++d) {
        live.CloseDay(d);
    }
    ASSERT_EQ(live.context.venue.orders.size(), 2u);
    live.CloseDay(5);  // 13: about 6% over its target
    EXPECT_EQ(live.context.venue.orders.size(), 2u);
    live.CloseDay(6);  // 20: about 27% over
    ASSERT_EQ(live.context.venue.orders.size(), 3u);
    const auto& sell = live.context.venue.orders[2];
    EXPECT_EQ(sell.side, Side::kSell);
    // Back to half the equity.
    const Decimal held = live.context.BalanceOf("A");
    EXPECT_NEAR(std::stod((held * D("20")).ToString()),
                std::stod((live.Equity() * D("0.5")).ToString()), 1.0);
}

// The model out of the trend on day 6 is a signal change, which trades even
// though the gap is under the threshold.
TEST(TrendEnsemble, ASignalChangeTradesWhateverTheThreshold) {
    auto params = Params();
    params.rebalance_threshold = D("10");  // never by size alone
    Live live(Trending(), params);
    for (int d = 1; d <= 5; ++d) {
        live.CloseDay(d);
    }
    const std::size_t before = live.context.venue.orders.size();
    ASSERT_EQ(before, 2u);  // days 3 and 4
    live.CloseDay(6);
    ASSERT_EQ(live.context.venue.orders.size(), 3u);
    EXPECT_EQ(live.context.venue.orders[2].side, Side::kSell);
    live.CloseDay(7);  // the last model out: sells the rest
    ASSERT_EQ(live.context.venue.orders.size(), 4u);
    EXPECT_EQ(live.context.venue.orders[3].side, Side::kSell);
    EXPECT_EQ(live.context.BalanceOf("A"), D("0"));
}

// On day 7 A's last model goes out and B's both go long: A is sold first,
// and B bought only once that sell's fill has been seen.
TEST(TrendEnsemble, SellsFirstAndBuysOnceTheSellHasFilled) {
    Live live({{"A/USD", {"10", "10", "10", "11", "12", "13", "12", "11"}},
               {"B/USD", {"10", "9", "8", "7", "6", "5", "4", "7"}}});
    for (int d = 1; d <= 6; ++d) {
        live.CloseDay(d);
    }
    const std::size_t before = live.context.venue.orders.size();
    live.context.Close(live.strategy, 7);
    ASSERT_EQ(live.context.venue.orders.size(), before + 1);
    EXPECT_EQ(live.context.venue.orders.back().symbol, "A/USD");
    EXPECT_EQ(live.context.venue.orders.back().side, Side::kSell);
    live.Fill(before);
    ASSERT_EQ(live.context.venue.orders.size(), before + 2);
    EXPECT_EQ(live.context.venue.orders.back().symbol, "B/USD");
    EXPECT_EQ(live.context.venue.orders.back().side, Side::kBuy);
}

// adopt_holdings counts the coins the account holds: B, in no trend, is
// sold at the first rebalance. Without it, B is neither counted nor sold.
TEST(TrendEnsemble, AdoptsAndSellsAHoldingOutOfTrend) {
    for (const bool adopt : {true, false}) {
        auto params = Params();
        params.adopt_holdings = adopt;
        FakeContext context;
        context.Set(Trending());
        context.SetBalance("USD", D("1000"));
        context.SetBalance("B", D("10"));
        TrendEnsemble strategy(params);
        strategy.OnStart(context);
        for (int d = 1; d <= 3; ++d) {
            context.Close(strategy, d);
        }
        const bool sold_b = std::ranges::any_of(
            context.venue.orders, [](const types::OrderRequest& order) {
                return order.symbol == "B/USD" && order.side == Side::kSell;
            });
        EXPECT_EQ(sold_b, adopt);
    }
}

// Run through the backtester: only the rising symbol is bought, and it is
// sold once it turns down.
TEST(TrendEnsemble, BacktestsBuyingTheTrendAndSellingItsEnd) {
    std::vector<const char*> up;
    std::vector<const char*> down;
    static const std::vector<std::string> kUp = [] {
        std::vector<std::string> v;
        for (int i = 0; i < 30; ++i) {
            v.push_back(
                std::to_string(100 + (i < 20 ? 3 * i : 60 - 4 * (i - 20))));
        }
        return v;
    }();
    static const std::vector<std::string> kDown = [] {
        std::vector<std::string> v;
        for (int i = 0; i < 30; ++i) {
            v.push_back(std::to_string(100 - 2 * i));
        }
        return v;
    }();
    for (int i = 0; i < 30; ++i) {
        up.push_back(kUp[i].c_str());
        down.push_back(kDown[i].c_str());
    }
    auto data = std::make_shared<ReplayDataSource>();
    data->AddBars("A/USD", Timeframe::kD1, Daily(up));
    data->AddBars("B/USD", Timeframe::kD1, Daily(down));
    for (const auto* symbol : {"A/USD", "B/USD"}) {
        data->SetSymbolInfo({symbol, D("0.01"), D("0.001"), D("0.001"), {}});
    }
    backtest::BacktestOptions o;
    o.start = Day(11);
    o.end = Day(30);
    o.paper.initial_balances = {{"USD", D("1000")}};
    o.paper.slippage = D("0");
    o.paper.maker_fee_rate = D("0");
    o.paper.taker_fee_rate = D("0");
    auto result = backtest::RunBacktest(
        data, std::make_unique<TrendEnsemble>(Params()), o);
    ASSERT_TRUE(result.has_value()) << result.error().ToString();
    ASSERT_FALSE(result->fills.empty());
    EXPECT_TRUE(std::ranges::none_of(result->fills, [](const auto& fill) {
        return fill.symbol == "B/USD";
    }));
    EXPECT_EQ(result->fills.front().side, Side::kBuy);
    EXPECT_EQ(result->fills.back().side, Side::kSell);
    Decimal held;
    for (const auto& fill : result->fills) {
        held += fill.side == Side::kBuy ? fill.quantity : -fill.quantity;
    }
    EXPECT_EQ(held, D("0"));  // out once both models stopped out
}

// Below the max_weight cap a model holds target_vol / volatility of its
// share: here 0.5 over day 3's volatility, one model of two long.
TEST(TrendEnsemble, SizesToTheTargetVolatility) {
    auto params = Params();
    params.target_vol = D("0.5");
    params.invest_fraction = D("0.9");
    Live live(Trending(), params);
    for (int d = 1; d <= 3; ++d) {
        live.CloseDay(d);
    }
    ASSERT_EQ(live.context.venue.orders.size(), 1u);
    const auto vol =
        Volatility(live.context.Bars("A/USD", Timeframe::kD1), 3, 365);
    ASSERT_TRUE(vol);
    const Decimal per_model =
        Decimal::Divide(D("0.5"), *vol, 8, types::Rounding::kDown);
    ASSERT_LT(per_model, D("1"));  // the cap does not bind
    // 1000 x (1 of 2 models) x per_model x 0.9 / 2 symbols, at 11.
    const Decimal target =
        D("1000") * Decimal::Divide(per_model * D("0.45"), D("2"), 8,
                                    types::Rounding::kDown);
    EXPECT_EQ(live.context.venue.orders[0].quantity,
              Decimal::Divide(target, D("11"), 3, types::Rounding::kDown));
}

// A buy the cash could not pay for is tried again at the next rebalance;
// once made, the holding is left until it drifts past the threshold.
TEST(TrendEnsemble, RetriesASignalItCouldNotTrade) {
    auto params = Params();
    params.lookbacks = {3};
    params.rebalance_threshold = D("0.9");
    Live live({{"A/USD", {"10", "10", "10", "11", "12", "13"}},
               {"B/USD", {"10", "9", "8", "7", "6", "5"}}},
              params);
    live.context.venue.balances = {{"USD", D("1000"), D("0.5"), D("999.5")}};
    for (int d = 1; d <= 3; ++d) {
        live.CloseDay(d);
    }
    EXPECT_TRUE(live.context.venue.orders.empty());  // 0.5 USD free
    live.context.SetBalance("USD", D("1000"));
    live.CloseDay(4);
    ASSERT_EQ(live.context.venue.orders.size(), 1u);
    EXPECT_EQ(live.context.venue.orders[0].side, Side::kBuy);
    live.CloseDay(5);  // traded now: nothing more
    EXPECT_EQ(live.context.venue.orders.size(), 1u);
}

// A has no bar for day 4, so day 4 completes only when A's day-5 bar
// arrives. Then C, whose newest bar is day 4's, trades (its 5-bar model went
// long), and A, whose newest is day 5's, is left as it is until day 5.
TEST(TrendEnsemble, LeavesASymbolWithoutABarAsItIs) {
    Live live({{"A/USD", {"10", "10", "10", "11", nullptr, "13"}},
               {"C/USD", {"10", "10", "10", "11", "12", "13"}}},
              [] {
                  auto p = Params();
                  p.symbols = {"A/USD", "C/USD"};
                  return p;
              }());
    for (int d = 1; d <= 3; ++d) {
        live.CloseDay(d);
    }
    ASSERT_EQ(live.context.venue.orders.size(), 2u);  // both bought on day 3
    live.CloseDay(4);
    EXPECT_EQ(live.context.venue.orders.size(), 2u);  // day 4 not complete
    live.context.Close(live.strategy, 5);
    ASSERT_GE(live.context.venue.orders.size(), 3u);
    EXPECT_EQ(live.context.venue.orders[2].symbol, "C/USD");
}

// B stops sending bars after day 2 (halted). Only day 3 waits for it,
// completing on A's bar of day 4; from then on A trades on its own day: out
// of its 3-bar model on day 6 and its 5-bar one on day 7.
TEST(TrendEnsemble, AHaltedSymbolDoesNotHoldUpTheOthers) {
    Live live(
        {{"A/USD", {"10", "10", "10", "11", "12", "13", "12", "11", "10", "9"}},
         {"B/USD", {"10", "9", "8"}}});
    for (int d = 1; d <= 5; ++d) {
        live.CloseDay(d);
    }
    const std::size_t before = live.context.venue.orders.size();
    live.CloseDay(6);
    ASSERT_EQ(live.context.venue.orders.size(), before + 1);
    EXPECT_EQ(live.context.venue.orders.back().side, Side::kSell);
    live.CloseDay(7);
    ASSERT_EQ(live.context.venue.orders.size(), before + 2);
    EXPECT_EQ(live.context.BalanceOf("A"), D("0"));
}

// No bars from day 9 to day 30 (a delisting): the models start again on
// day 30 rather than reading 50 as a new high over the closes before the
// gap, and the volatility waits for 3 returns after it.
TEST(TrendEnsemble, ARelistedSymbolStartsAfresh) {
    std::vector<const char*> closes(34, nullptr);
    const char* before[] = {"10", "11", "12", "13", "14",
                            "15", "16", "17", "18", "19"};
    for (int d = 0; d < 10; ++d) {
        closes[d] = before[d];
    }
    closes[30] = "50";
    closes[31] = "51";
    closes[32] = "52";
    closes[33] = "53";
    auto params = Params();
    params.symbols = {"A/USD"};
    Live live({{"A/USD", closes}}, params);
    for (int d = 1; d <= 9; ++d) {
        live.CloseDay(d);
    }
    EXPECT_EQ(live.strategy.Conviction("A/USD"), D("1"));
    const std::size_t held = live.context.venue.orders.size();
    live.CloseDay(30);
    EXPECT_EQ(live.strategy.Conviction("A/USD"), D("0"));
    ASSERT_EQ(live.context.venue.orders.size(), held + 1);  // sold
    EXPECT_EQ(live.context.venue.orders.back().side, Side::kSell);
    live.CloseDay(31);
    live.CloseDay(32);  // 3 closes since the gap: the 3-bar model is long
    EXPECT_EQ(live.strategy.Conviction("A/USD"), D("0.5"));
    EXPECT_EQ(live.context.venue.orders.size(), held + 1);  // no volatility
    live.CloseDay(33);  // 3 returns since the gap
    ASSERT_EQ(live.context.venue.orders.size(), held + 2);
    EXPECT_EQ(live.context.venue.orders.back().side, Side::kBuy);
}

// Day 4 (A's bar missing) completes late, on A's bar of day 5, and buys C;
// day 5 completes as C's bar arrives, while that buy is still open, so it
// waits, and runs once the buy has filled: no second buy of C, and A's
// 5-bar model, long on day 5, is bought.
TEST(TrendEnsemble, ARebalancePutOffForOpenOrdersRunsOnceTheyClose) {
    auto params = Params();
    params.symbols = {"A/USD", "C/USD"};
    Live live({{"A/USD", {"10", "10", "10", "11", nullptr, "13"}},
               {"C/USD", {"10", "10", "10", "11", "12", "13"}}},
              params);
    for (int d = 1; d <= 4; ++d) {
        live.CloseDay(d);
    }
    ASSERT_EQ(live.context.venue.orders.size(), 2u);  // A and C on day 3
    types::OrderStatus open;
    open.order_id = "order-3";
    open.state = types::OrderState::kNew;
    live.context.venue.order_states["order-3"] = open;  // as the venue says
    live.context.Close(live.strategy, 5);
    ASSERT_EQ(live.context.venue.orders.size(), 3u);
    EXPECT_EQ(live.context.venue.orders[2].symbol, "C/USD");
    live.Fill(2);
    ASSERT_EQ(live.context.venue.orders.size(), 4u);
    EXPECT_EQ(live.context.venue.orders[3].symbol, "A/USD");
    EXPECT_EQ(live.context.venue.orders[3].side, Side::kBuy);
}

// A market order the venue reports filled at once has moved the cash before
// its fill is seen. A period completing in between (bars backfilled after a
// disconnect) waits for that fill, or asks the venue, rather than buying
// again: A, bought on day 3 (its 3-bar model long), is not bought again on
// day 4, where neither model changes and A is within the threshold.
TEST(TrendEnsemble, ABuyFilledAtOnceIsNotBoughtAgainBeforeItsFill) {
    Live live({{"A/USD", {"10", "10", "10", "11", "10.8"}},
               {"B/USD", {"10", "9", "8", "7", "6"}}});
    live.context.venue.ack_state = types::OrderState::kFilled;
    for (int d = 1; d <= 3; ++d) {
        live.context.Close(live.strategy, d);
    }
    ASSERT_EQ(live.context.venue.orders.size(), 1u);
    const auto& buy = live.context.venue.orders[0];
    EXPECT_EQ(buy.symbol, "A/USD");
    // The venue's side of the fill: cash and coin moved, the fill not yet
    // delivered.
    const Decimal notional = buy.quantity * live.context.Close("A/USD");
    live.context.SetBalance("USD", live.context.BalanceOf("USD") - notional);
    live.context.SetBalance("A", buy.quantity);
    types::OrderStatus filled;
    filled.order_id = "order-1";
    filled.symbol = "A/USD";
    filled.state = types::OrderState::kFilled;
    filled.filled_quantity = buy.quantity;
    live.context.venue.order_states["order-1"] = filled;
    live.context.Close(live.strategy, 4);
    EXPECT_EQ(live.context.venue.orders.size(), 1u);
    EXPECT_EQ(live.strategy.Conviction("A/USD"), D("0.5"));
}

TEST(TrendEnsemble, RefusesBadParameters) {
    auto bad = [](auto change) {
        auto params = Params();
        change(params);
        EXPECT_THROW(TrendEnsemble{params}, std::invalid_argument);
    };
    bad([](TrendEnsembleParams& p) { p.symbols.clear(); });
    bad([](TrendEnsembleParams& p) { p.symbols = {"A/USD", "A/USD"}; });
    bad([](TrendEnsembleParams& p) { p.symbols = {"A/USD", "B/EUR"}; });
    bad([](TrendEnsembleParams& p) { p.lookbacks.clear(); });
    bad([](TrendEnsembleParams& p) { p.lookbacks = {5, 1}; });
    bad([](TrendEnsembleParams& p) { p.lookbacks = {5, 10, 5}; });
    bad([](TrendEnsembleParams& p) { p.lookbacks = {5, 10001}; });
    bad([](TrendEnsembleParams& p) { p.vol_bars = 10001; });
    bad([](TrendEnsembleParams& p) { p.vol_bars = 1; });
    bad([](TrendEnsembleParams& p) { p.target_vol = D("0"); });
    bad([](TrendEnsembleParams& p) { p.max_weight = D("1.5"); });
    bad([](TrendEnsembleParams& p) { p.max_weight = D("0"); });
    bad([](TrendEnsembleParams& p) { p.rebalance_threshold = D("-0.1"); });
    bad([](TrendEnsembleParams& p) { p.invest_fraction = D("0"); });
    bad([](TrendEnsembleParams& p) { p.timeframe = Timeframe::kMo1; });
}

}  // namespace
}  // namespace kestrel::strategy
