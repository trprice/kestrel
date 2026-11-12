#include "kestrel/strategy/mean_reversion.hpp"

#include "strategy_fakes.hpp"
#include "test_helpers.hpp"

namespace {

using namespace kestrel;
using kestrel::testing::D;
using kestrel::testing::RecordingVenue;
using strategy::MeanReversion;
using strategy::MeanReversionParams;
using types::Side;
using types::Timeframe;

class FakeContext : public strategy::Context {
   public:
    types::Timestamp Now() const override { return types::FromEpochSeconds(0); }
    trading::TradingVenue& Venue() override { return venue; }
    types::Result<types::SymbolInfo> SymbolInfo(const types::Symbol&) override {
        if (symbol_info) {
            return *symbol_info;
        }
        return types::MakeError(types::ErrorCategory::kNotFound, "none");
    }
    std::span<const types::Bar> Bars(const types::Symbol&,
                                     Timeframe) const override {
        return bars;
    }
    bool MarketOpen() const override { return market_open; }

    const types::Bar& Add(const char* close, const char* high = nullptr,
                          const char* low = nullptr) {
        types::Bar bar;
        bar.open_time = types::FromEpochSeconds(3600 * bars.size());
        bar.open = bar.close = D(close);
        bar.high = high ? D(high) : bar.close;
        bar.low = low ? D(low) : bar.close;
        bars.push_back(bar);
        return bars.back();
    }

    RecordingVenue venue;
    std::vector<types::Bar> bars;
    std::optional<types::SymbolInfo> symbol_info;
    bool market_open = true;
};

MeanReversionParams Params() {
    MeanReversionParams p;
    p.symbol = "BTC/USD";
    p.timeframe = Timeframe::kH1;
    p.rsi_period = 2;
    p.oversold = D("25");
    p.overbought = D("75");
    p.quantity = D("0.5");
    return p;
}

void Feed(MeanReversion& strategy, FakeContext& context, const char* close,
          const types::Symbol& symbol = "BTC/USD") {
    const types::Bar& bar = context.Add(close);
    strategy.OnBar(context, symbol, Timeframe::kH1, bar);
}

types::Fill FillOf(Side side, const char* price = "100") {
    types::Fill fill;
    fill.symbol = "BTC/USD";
    fill.side = side;
    fill.price = D(price);
    fill.quantity = D("0.5");
    return fill;
}

TEST(MeanReversionTest, RejectsBadParameters) {
    auto p = Params();
    p.quantity = D("0");
    EXPECT_THROW(MeanReversion{p}, std::invalid_argument);

    p = Params();
    p.rsi_period = 0;
    EXPECT_THROW(MeanReversion{p}, std::invalid_argument);

    p = Params();
    p.oversold = D("0");
    EXPECT_THROW(MeanReversion{p}, std::invalid_argument);

    p = Params();
    p.oversold = D("75");
    p.overbought = D("75");
    EXPECT_THROW(MeanReversion{p}, std::invalid_argument);

    p = Params();
    p.overbought = D("100");
    EXPECT_THROW(MeanReversion{p}, std::invalid_argument);

    p = Params();
    p.bollinger_period = 1;
    EXPECT_THROW(MeanReversion{p}, std::invalid_argument);

    p = Params();
    p.bollinger_width = D("0");
    EXPECT_THROW(MeanReversion{p}, std::invalid_argument);

    p = Params();
    p.atr_stop = D("-1");
    EXPECT_THROW(MeanReversion{p}, std::invalid_argument);

    p = Params();
    p.atr_bars = 0;
    EXPECT_THROW(MeanReversion{p}, std::invalid_argument);

    p = Params();
    p.exit_sma = 1;  // the close itself
    EXPECT_THROW(MeanReversion{p}, std::invalid_argument);

    p = Params();
    p.trend_sma = 1;
    EXPECT_THROW(MeanReversion{p}, std::invalid_argument);
}

TEST(MeanReversionTest, ComputesWarmupBarsFromEnabledIndicators) {
    MeanReversion plain(Params());
    EXPECT_EQ(plain.Needs().symbols, std::vector<types::Symbol>{"BTC/USD"});
    EXPECT_EQ(plain.Needs().bar_timeframes,
              std::vector<Timeframe>{Timeframe::kH1});
    EXPECT_EQ(plain.Needs().warmup_bars, 13u);  // 6 * 2 + 1

    auto trend = Params();
    trend.trend_sma = 200;
    EXPECT_EQ(MeanReversion(trend).Needs().warmup_bars, 200u);

    auto atr = Params();
    atr.atr_stop = D("2");
    atr.atr_bars = 14;
    EXPECT_EQ(MeanReversion(atr).Needs().warmup_bars, 85u);  // 6 * 14 + 1
}

TEST(MeanReversionTest, BuysWhenOversoldAndSellsWhenOverbought) {
    MeanReversion strategy(Params());
    FakeContext context;

    // Two consecutive drops drive 2-period RSI to 0 (<= 25): buy.
    Feed(strategy, context, "100");
    Feed(strategy, context, "95");
    EXPECT_EQ(context.venue.orders.size(), 0u);  // only 2 bars (< period + 1)

    Feed(strategy, context, "90");  // RSI(2) = 0 <= 25
    ASSERT_EQ(context.venue.orders.size(), 1u);
    EXPECT_EQ(context.venue.orders[0].side, Side::kBuy);
    EXPECT_EQ(context.venue.orders[0].quantity, D("0.5"));

    strategy.OnFill(context, FillOf(Side::kBuy, "90"));

    // Still low: stays long.
    Feed(strategy, context, "89");
    EXPECT_EQ(context.venue.orders.size(), 1u);

    // Sharp bounce drives RSI(2) above 75: sell.
    Feed(strategy, context, "100");
    ASSERT_EQ(context.venue.orders.size(), 2u);
    EXPECT_EQ(context.venue.orders[1].side, Side::kSell);
}

TEST(MeanReversionTest, BollingerBandFilterRequiresCloseAtOrBelowLowerBand) {
    auto p = Params();
    p.bollinger_period = 4;
    p.bollinger_width = D("1.5");
    MeanReversion strategy(p);
    FakeContext context;

    // Gentle dip: RSI(2) is 0, but close is not below the 1.5-sigma lower band.
    Feed(strategy, context, "100");
    Feed(strategy, context, "105");
    Feed(strategy, context, "100");
    Feed(strategy, context, "99");  // mean=101, std~2.35, lower~97.48
    EXPECT_EQ(context.venue.orders.size(), 0u);

    // Sharp plunge pierces the lower Bollinger band: buys.
    Feed(strategy, context, "85");
    ASSERT_EQ(context.venue.orders.size(), 1u);
    EXPECT_EQ(context.venue.orders[0].side, Side::kBuy);
}

TEST(MeanReversionTest, TrendSmaFilterBlocksEntryBelowLongTermTrend) {
    auto p = Params();
    p.trend_sma = 7;
    MeanReversion strategy(p);
    FakeContext context;

    // Downtrend: close 90 is below the 7-bar SMA (97.14), so no buy.
    for (const char* close : {"100", "100", "100", "100", "98", "95", "90"}) {
        Feed(strategy, context, close);
    }
    EXPECT_EQ(context.venue.orders.size(), 0u);

    // Uptrend pullback: SMA(7) = 82.86 < 90, and RSI(2) is 21.57 <= 25.
    FakeContext up_context;
    MeanReversion up_strategy(p);
    for (const char* close : {"10", "10", "120", "120", "120", "110", "90"}) {
        Feed(up_strategy, up_context, close);
    }
    ASSERT_EQ(up_context.venue.orders.size(), 1u);
    EXPECT_EQ(up_context.venue.orders[0].side, Side::kBuy);
}

TEST(MeanReversionTest, ExitSmaClosesPositionWhenPriceReclaimsShortMean) {
    auto p = Params();
    p.overbought = D("99");  // keep RSI exit out of the way
    p.exit_sma = 3;
    MeanReversion strategy(p);
    FakeContext context;

    Feed(strategy, context, "100");
    Feed(strategy, context, "95");
    Feed(strategy, context, "90");
    ASSERT_EQ(context.venue.orders.size(), 1u);
    strategy.OnFill(context, FillOf(Side::kBuy, "90"));

    // Next bar closes at 88: below 3-bar SMA (95 + 90 + 88) / 3 = 91.
    Feed(strategy, context, "88");
    EXPECT_EQ(context.venue.orders.size(), 1u);

    // Rebound to 91: reaches 3-bar SMA (90 + 88 + 91) / 3 = 89.67 -> exits.
    Feed(strategy, context, "91");
    ASSERT_EQ(context.venue.orders.size(), 2u);
    EXPECT_EQ(context.venue.orders[1].side, Side::kSell);
}

TEST(MeanReversionTest, AtrStopExitsWhenCloseBreaksStopLevel) {
    auto p = Params();
    p.atr_stop = D("2");
    p.atr_bars = 2;
    MeanReversion strategy(p);
    FakeContext context;

    context.Add("100", "102", "98");
    strategy.OnBar(context, "BTC/USD", Timeframe::kH1, context.bars.back());
    context.Add("96", "98", "94");
    strategy.OnBar(context, "BTC/USD", Timeframe::kH1, context.bars.back());
    context.Add("92", "94", "90");  // ATR(2) = 6, entry at 92 -> stop at 80
    strategy.OnBar(context, "BTC/USD", Timeframe::kH1, context.bars.back());
    ASSERT_EQ(context.venue.orders.size(), 1u);
    strategy.OnFill(context, FillOf(Side::kBuy, "92"));

    // Drops to 86 (above stop 80): holds.
    context.Add("86", "88", "85");
    strategy.OnBar(context, "BTC/USD", Timeframe::kH1, context.bars.back());
    EXPECT_EQ(context.venue.orders.size(), 1u);

    // Drops to 79 (<= stop 80): exits even though RSI is still oversold.
    context.Add("79", "81", "78");
    strategy.OnBar(context, "BTC/USD", Timeframe::kH1, context.bars.back());
    ASSERT_EQ(context.venue.orders.size(), 2u);
    EXPECT_EQ(context.venue.orders[1].side, Side::kSell);
}

TEST(MeanReversionTest, MaxHoldBarsAndFlattenAtCloseExitPosition) {
    auto p = Params();
    p.max_hold_bars = 2;
    p.flatten_at_close = true;
    MeanReversion strategy(p);
    FakeContext context;

    Feed(strategy, context, "100");
    Feed(strategy, context, "95");
    Feed(strategy, context, "90");
    ASSERT_EQ(context.venue.orders.size(), 1u);
    strategy.OnFill(context, FillOf(Side::kBuy, "90"));

    Feed(strategy, context, "89");  // bar 1 held
    EXPECT_EQ(context.venue.orders.size(), 1u);
    Feed(strategy, context, "88");  // bar 2 held -> max_hold_bars exit
    ASSERT_EQ(context.venue.orders.size(), 2u);
    EXPECT_EQ(context.venue.orders[1].side, Side::kSell);
    strategy.OnFill(context, FillOf(Side::kSell, "88"));

    // Enter again and flatten before the session closes.
    Feed(strategy, context, "85");
    ASSERT_EQ(context.venue.orders.size(), 3u);
    strategy.OnFill(context, FillOf(Side::kBuy, "85"));

    strategy.OnSessionClosing(context, types::FromEpochSeconds(36000));
    ASSERT_EQ(context.venue.orders.size(), 4u);
    EXPECT_EQ(context.venue.orders[3].side, Side::kSell);
}

TEST(MeanReversionTest, DefersOrderUntilSessionOpenAndDeductsBaseFee) {
    MeanReversion strategy(Params());
    FakeContext context;
    context.market_open = false;
    context.symbol_info =
        types::SymbolInfo{"BTC/USD", D("0.01"), D("0.01"), D("0.01"), {}};

    Feed(strategy, context, "100");
    Feed(strategy, context, "95");
    Feed(strategy, context, "90");
    EXPECT_EQ(context.venue.orders.size(), 0u);  // market closed

    context.market_open = true;
    strategy.OnSessionOpen(context);
    ASSERT_EQ(context.venue.orders.size(), 1u);
    EXPECT_EQ(context.venue.orders[0].side, Side::kBuy);

    types::Fill buy = FillOf(Side::kBuy, "90");
    buy.fee = D("0.005");
    buy.fee_asset = "BTC";
    strategy.OnFill(context, buy);  // held = 0.495, lot_size = 0.01 -> 0.49

    Feed(strategy, context, "110");
    ASSERT_EQ(context.venue.orders.size(), 2u);
    EXPECT_EQ(context.venue.orders[1].side, Side::kSell);
    EXPECT_EQ(context.venue.orders[1].quantity, D("0.49"));
}

TEST(MeanReversionTest, AnEntryDueAtTheOpenIsDroppedWhenItsSignalGoes) {
    MeanReversion strategy(Params());
    FakeContext context;
    context.market_open = false;

    Feed(strategy, context, "100");
    Feed(strategy, context, "95");
    Feed(strategy, context, "90");   // oversold while closed: due at the open
    Feed(strategy, context, "110");  // RSI(2) back up: no longer oversold
    context.market_open = true;
    strategy.OnSessionOpen(context);
    EXPECT_EQ(context.venue.orders.size(), 0u);
}

TEST(MeanReversionTest, NoEntryWhileAnExitIsStillFilling) {
    MeanReversion strategy(Params());
    FakeContext context;

    Feed(strategy, context, "100");
    Feed(strategy, context, "95");
    Feed(strategy, context, "90");
    strategy.OnFill(context, FillOf(Side::kBuy, "90"));
    Feed(strategy, context, "89");
    Feed(strategy, context, "100");  // overbought: sells
    ASSERT_EQ(context.venue.orders.size(), 2u);

    types::Fill part = FillOf(Side::kSell, "100");
    part.order_id = "order-2";
    part.quantity = D("0.2");
    strategy.OnFill(context, part);
    Feed(strategy, context, "80");  // oversold again, but the sell is working
    Feed(strategy, context, "70");
    EXPECT_EQ(context.venue.orders.size(), 2u);

    part.quantity = D("0.3");
    strategy.OnFill(context, part);  // the sell is done
    Feed(strategy, context, "60");
    ASSERT_EQ(context.venue.orders.size(), 3u);
    EXPECT_EQ(context.venue.orders[2].side, Side::kBuy);
}

TEST(MeanReversionTest, TheStopIsSetFromTheAveragePricePaid) {
    auto p = Params();
    p.atr_stop = D("1");
    p.atr_bars = 2;
    MeanReversion strategy(p);
    FakeContext context;

    context.Add("100", "102", "98");
    strategy.OnBar(context, "BTC/USD", Timeframe::kH1, context.bars.back());
    context.Add("96", "98", "94");
    strategy.OnBar(context, "BTC/USD", Timeframe::kH1, context.bars.back());
    context.Add("92", "94", "90");  // ATR(2) = 6
    strategy.OnBar(context, "BTC/USD", Timeframe::kH1, context.bars.back());
    ASSERT_EQ(context.venue.orders.size(), 1u);

    types::Fill part = FillOf(Side::kBuy, "90");
    part.order_id = "order-1";
    part.quantity = D("0.25");
    strategy.OnFill(context, part);
    part.price = D("94");
    strategy.OnFill(context, part);  // average 92: stop at 86

    context.Add("86.5", "88", "86");
    strategy.OnBar(context, "BTC/USD", Timeframe::kH1, context.bars.back());
    EXPECT_EQ(context.venue.orders.size(), 1u);
    context.Add("86", "87", "85");
    strategy.OnBar(context, "BTC/USD", Timeframe::kH1, context.bars.back());
    ASSERT_EQ(context.venue.orders.size(), 2u);
    EXPECT_EQ(context.venue.orders[1].side, Side::kSell);
}

TEST(MeanReversionTest, NoEntryWhenTheStopWouldBeAtOrUnderZero) {
    auto p = Params();
    p.atr_stop = D("20");
    p.atr_bars = 2;
    MeanReversion strategy(p);
    FakeContext context;

    context.Add("100", "102", "98");
    strategy.OnBar(context, "BTC/USD", Timeframe::kH1, context.bars.back());
    context.Add("96", "98", "94");
    strategy.OnBar(context, "BTC/USD", Timeframe::kH1, context.bars.back());
    context.Add("92", "94", "90");  // ATR(2) = 6: 20 ATRs under 92 is -28
    strategy.OnBar(context, "BTC/USD", Timeframe::kH1, context.bars.back());
    EXPECT_EQ(context.venue.orders.size(), 0u);
}

TEST(MeanReversionTest, AStopFromAnEarlierSignalIsNotUsedForAnotherBuy) {
    auto p = Params();
    p.atr_stop = D("2");
    p.atr_bars = 2;
    MeanReversion strategy(p);
    FakeContext context;
    context.venue.reject_orders = true;  // the signal's buy is refused

    context.Add("100", "102", "98");
    strategy.OnBar(context, "BTC/USD", Timeframe::kH1, context.bars.back());
    context.Add("96", "98", "94");
    strategy.OnBar(context, "BTC/USD", Timeframe::kH1, context.bars.back());
    context.Add("92", "94", "90");  // signal: ATR 6, stop 80
    strategy.OnBar(context, "BTC/USD", Timeframe::kH1, context.bars.back());
    context.Add("120", "121", "119");  // no signal: its ATR and stop go
    strategy.OnBar(context, "BTC/USD", Timeframe::kH1, context.bars.back());

    // Another order buys (bought by hand, say), then the price falls under
    // the old stop: no stop of this strategy's is set, so nothing is sold.
    context.venue.reject_orders = false;
    types::Fill other = FillOf(Side::kBuy, "119");
    other.order_id = "manual-1";
    strategy.OnFill(context, other);
    context.Add("79", "80", "78");
    strategy.OnBar(context, "BTC/USD", Timeframe::kH1, context.bars.back());
    EXPECT_TRUE(context.venue.orders.empty());
}

}  // namespace
