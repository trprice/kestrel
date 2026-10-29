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

TEST(ReplayDataSourceTest, SortsBars) {
    ReplayDataSource data;
    data.AddBars(kSymbol, Timeframe::kH1, {BarAt(2, "2"), BarAt(1, "1")});
    EXPECT_EQ(data.Series(kSymbol, Timeframe::kH1).front().open_time, Hour(1));
    EXPECT_TRUE(data.Series("ETH/USD", Timeframe::kH1).empty());

    auto info = data.GetSymbolInfo(kSymbol);
    ASSERT_FALSE(info);
    EXPECT_EQ(info.error().category, ErrorCategory::kNotFound);
}

types::Quote QuoteAt(int minute, const char* bid, const char* ask,
                     std::int64_t seq = 0) {
    types::Quote q;
    q.symbol = kSymbol;
    q.venue = "replay";
    q.seq = seq;
    q.received_time = Hour(0) + std::chrono::minutes(minute);
    q.bid = D(bid);
    q.ask = D(ask);
    return q;
}

TEST(ReplayDataSourceTest, SortsQuotesAndEmitsToTheSubscription) {
    ReplayDataSource data;
    data.AddQuotes(kSymbol, {QuoteAt(5, "1", "2", 3), QuoteAt(1, "1", "2", 2),
                             QuoteAt(1, "1", "2", 1)});
    const auto& stored = data.QuotesFor(kSymbol);
    ASSERT_EQ(stored.size(), 3u);
    EXPECT_EQ(stored[0].seq, 1);  // ties on time keep arrival (seq) order
    EXPECT_EQ(stored[1].seq, 2);
    EXPECT_EQ(stored[2].seq, 3);
    EXPECT_TRUE(data.QuotesFor("ETH/USD").empty());

    EXPECT_FALSE(data.EmitQuote(stored[0]));  // nothing subscribed yet
    std::vector<std::int64_t> got;
    auto sub = data.StreamQuotes(
        {kSymbol}, [&](const types::Quote& q) { got.push_back(q.seq); });
    ASSERT_OK(sub);
    EXPECT_TRUE(data.EmitQuote(stored[0]));
    auto other = stored[1];
    other.symbol = "ETH/USD";
    EXPECT_FALSE(data.EmitQuote(other));
    EXPECT_EQ(got, std::vector<std::int64_t>{1});

    (*sub)->Stop();
    EXPECT_FALSE(data.EmitQuote(stored[1]));
}

types::MarketTrade TradeAt(std::int64_t id, int ms, const char* price,
                           Side taker) {
    types::MarketTrade t;
    t.trade_id = id;
    t.time = types::FromEpochMillis(1'700'000'000'000 + ms);
    t.price = D(price);
    t.quantity = D("0.004");
    t.taker_side = taker;
    return t;
}

TEST(QuotesFromTradesTest, EachTradeSetsTheSideItTookAndIsTheLastTrade) {
    const std::vector<types::MarketTrade> trades = {
        TradeAt(1, 0, "100.00", Side::kSell),   // bid 100, no ask yet
        TradeAt(2, 10, "100.05", Side::kBuy),   // ask 100.05
        TradeAt(3, 20, "100.01", Side::kSell),  // bid moves up
    };
    const auto quotes =
        backtest::QuotesFromTrades("ZEC/USDT", "binanceus", trades, D("0.01"));
    ASSERT_EQ(quotes.size(), 3u);
    EXPECT_EQ(quotes[0].bid, D("100.00"));
    EXPECT_FALSE(quotes[0].ask);
    EXPECT_EQ(quotes[1].bid, D("100.00"));
    EXPECT_EQ(quotes[1].ask, D("100.05"));
    EXPECT_EQ(quotes[2].bid, D("100.01"));
    EXPECT_EQ(quotes[2].ask, D("100.05"));
    for (std::size_t i = 0; i < quotes.size(); ++i) {
        EXPECT_EQ(quotes[i].symbol, "ZEC/USDT");
        EXPECT_EQ(quotes[i].venue, "binanceus");
        EXPECT_EQ(quotes[i].seq, static_cast<std::int64_t>(i) + 1);
        EXPECT_EQ(quotes[i].last, trades[i].price);
        EXPECT_EQ(quotes[i].last_size, trades[i].quantity);
        EXPECT_EQ(quotes[i].exchange_time, trades[i].time);
        EXPECT_EQ(quotes[i].received_time, trades[i].time);
    }
}

TEST(QuotesFromTradesTest, ATradeThroughTheOtherSideMovesItATickAway) {
    const std::vector<types::MarketTrade> trades = {
        TradeAt(1, 0, "100.00", Side::kSell),
        TradeAt(2, 10, "100.05", Side::kBuy),
        TradeAt(3, 20, "100.10", Side::kSell),  // the bid is now above the ask
        TradeAt(4, 30, "99.90", Side::kBuy),    // the ask is now below the bid
    };
    const auto quotes =
        backtest::QuotesFromTrades("ZEC/USDT", "binanceus", trades, D("0.01"));
    EXPECT_EQ(quotes[2].bid, D("100.10"));
    EXPECT_EQ(quotes[2].ask, D("100.11"));
    EXPECT_EQ(quotes[3].ask, D("99.90"));
    EXPECT_EQ(quotes[3].bid, D("99.89"));
    for (const auto& q : quotes) {
        if (q.bid && q.ask) {
            EXPECT_LT(*q.bid, *q.ask);
        }
    }
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

TEST(BacktestTest, RejectsCalendarTimeframes) {
    auto monthly = RunBacktest(Data(kCloses),
                               std::make_unique<MonthlyStrategy>(), Options());
    ASSERT_FALSE(monthly);
    EXPECT_EQ(monthly.error().category, ErrorCategory::kUnsupported);
}

std::shared_ptr<ReplayDataSource> QuoteData() {
    auto data = Data(kCloses);
    data->AddQuotes(kSymbol, {QuoteAt(10, "99", "101"),
                              QuoteAt(20, "199", "201"),
                              QuoteAt(30, "299", "301")});
    return data;
}

// Wants quotes, and hourly bars too if `with_bars`. Records every bar and quote
// it is handed as a (kind, time) pair, where a bar's time is when it closed,
// and buys 1 unit at market on the second one.
class RecordingBuyer : public strategy::Strategy {
   public:
    using Event = std::pair<const char*, types::Timestamp>;
    // `events` outlives the strategy, which RunBacktest destroys.
    explicit RecordingBuyer(std::vector<Event>& events, bool with_bars = false)
        : events(events), with_bars(with_bars) {}
    std::string_view Name() const override { return "recording-buyer"; }
    strategy::Requirements Needs() const override {
        strategy::Requirements r;
        r.symbols = {kSymbol};
        r.quotes = true;
        if (with_bars) {
            r.bar_timeframes = {Timeframe::kH1};
        }
        return r;
    }
    void OnBar(strategy::Context& ctx, const types::Symbol&,
               types::Timeframe timeframe, const types::Bar& bar) override {
        Record(ctx, "bar", bar.open_time + *types::FixedDuration(timeframe));
    }
    void OnQuote(strategy::Context& ctx, const types::Quote& quote) override {
        Record(ctx, "quote", quote.received_time);
    }
    std::vector<Event>& events;
    bool with_bars;

   private:
    void Record(strategy::Context& ctx, const char* kind,
                types::Timestamp time) {
        events.emplace_back(kind, time);
        if (events.size() == 2) {
            types::OrderRequest order;
            order.symbol = kSymbol;
            order.quantity = D("1");
            (void)ctx.Venue().PlaceOrder(order);
        }
    }
};

// Rests a buy limit at 99 on the first quote it sees.
class RestingBuyer : public strategy::Strategy {
   public:
    std::string_view Name() const override { return "resting-buyer"; }
    strategy::Requirements Needs() const override {
        strategy::Requirements r;
        r.symbols = {kSymbol};
        r.quotes = true;
        return r;
    }
    void OnQuote(strategy::Context& ctx, const types::Quote&) override {
        if (placed) {
            return;
        }
        placed = true;
        types::OrderRequest order;
        order.symbol = kSymbol;
        order.type = types::OrderType::kLimit;
        order.quantity = D("1");
        order.limit_price = D("99");
        (void)ctx.Venue().PlaceOrder(order);
    }
    bool placed = false;
};

TEST(BacktestTest, TheLimitFillModelDecidesWhenARestingOrderFills) {
    auto data = [] {
        auto d = Data(kCloses);
        // The ask touches the limit at 0:20 and goes through it at 0:30.
        d->AddQuotes(kSymbol, {QuoteAt(10, "98.9", "101"),
                               QuoteAt(20, "98", "99"),
                               QuoteAt(30, "97", "98.9")});
        return d;
    };
    auto options = Options();
    options.start = Hour(0);
    options.end = Hour(1);

    auto optimistic = RunBacktest(data(), std::make_unique<RestingBuyer>(),
                                  options);
    ASSERT_OK(optimistic);
    ASSERT_EQ(optimistic->fills.size(), 1u);
    EXPECT_EQ(optimistic->fills[0].time, Hour(0) + std::chrono::minutes(20));

    options.paper.limit_fill_model = trading::LimitFillModel::kConservative;
    auto conservative = RunBacktest(data(), std::make_unique<RestingBuyer>(),
                                    options);
    ASSERT_OK(conservative);
    ASSERT_EQ(conservative->fills.size(), 1u);
    EXPECT_EQ(conservative->fills[0].time, Hour(0) + std::chrono::minutes(30));
    EXPECT_EQ(conservative->fills[0].price, D("99"));  // its limit, not 98.9
}

// Quotes at 0:30, 1:00 and 1:30; hour 0's bar closes at 1:00 and hour 1's at
// 2:00, so the 1:00 quote and hour 0's close land on the same instant.
std::shared_ptr<ReplayDataSource> MixedData() {
    auto data = Data(kCloses);
    data->AddQuotes(kSymbol, {QuoteAt(30, "99", "101"),
                              QuoteAt(60, "199", "201"),
                              QuoteAt(90, "299", "301")});
    return data;
}

TEST(BacktestTest, ReplaysStoredQuotesToAQuoteStrategy) {
    std::vector<RecordingBuyer::Event> events;
    auto strategy = std::make_unique<RecordingBuyer>(events);
    auto options = Options();
    options.start = Hour(0);
    options.end = Hour(1);
    auto result = RunBacktest(QuoteData(), std::move(strategy), options);
    ASSERT_OK(result);

    EXPECT_EQ(result->quotes_replayed, 3u);
    EXPECT_EQ(result->bars_replayed, 0u);
    ASSERT_EQ(events.size(), 3u);
    EXPECT_EQ(events[0].second, Hour(0) + std::chrono::minutes(10));
    // The market buy placed on the second quote fills at its recorded ask,
    // not at a bar close.
    ASSERT_EQ(result->fills.size(), 1u);
    EXPECT_EQ(result->fills[0].side, Side::kBuy);
    EXPECT_EQ(result->fills[0].price, D("201"));
    EXPECT_EQ(result->fills[0].time, Hour(0) + std::chrono::minutes(20));
}

TEST(BacktestTest, OnlyReplaysQuotesInTheRange) {
    std::vector<RecordingBuyer::Event> events;
    auto strategy = std::make_unique<RecordingBuyer>(events);
    auto options = Options();
    options.start = Hour(0) + std::chrono::minutes(15);
    options.end = Hour(0) + std::chrono::minutes(30);  // end is exclusive
    auto result = RunBacktest(QuoteData(), std::move(strategy), options);
    ASSERT_OK(result);
    EXPECT_EQ(result->quotes_replayed, 1u);
    EXPECT_EQ(events.size(), 1u);
    EXPECT_TRUE(result->fills.empty());
}

TEST(BacktestTest, ReplaysBarsAndQuotesInOneTimeline) {
    std::vector<RecordingBuyer::Event> events;
    auto strategy = std::make_unique<RecordingBuyer>(events, true);
    auto options = Options();
    options.start = Hour(0);
    options.end = Hour(2);
    auto result = RunBacktest(MixedData(), std::move(strategy), options);
    ASSERT_OK(result);
    EXPECT_EQ(result->bars_replayed, 2u);
    EXPECT_EQ(result->quotes_replayed, 3u);

    // The quote arriving at 1:00 is delivered before the bar that closes at
    // the same instant; everything else runs in arrival / close order.
    EXPECT_EQ(events, (std::vector<RecordingBuyer::Event>{
                        {"quote", Hour(0) + std::chrono::minutes(30)},
                        {"quote", Hour(1)},
                        {"bar", Hour(1)},  // hour 0's bar closes at 1:00 too
                        {"quote", Hour(0) + std::chrono::minutes(90)},
                        {"bar", Hour(2)},  // hour 1's bar closes at the end
                    }));
}

TEST(BacktestTest, FillsAMarketOrderAtTheQuoteTiedWithABarClose) {
    std::vector<RecordingBuyer::Event> events;
    auto strategy = std::make_unique<RecordingBuyer>(events, true);
    auto options = Options();
    options.start = Hour(0);
    options.end = Hour(2);
    auto result = RunBacktest(MixedData(), std::move(strategy), options);
    ASSERT_OK(result);

    // The buy placed on the 1:00 quote fills at its recorded ask (201), not at
    // hour 0's close (10), even though a bar closes at the same instant.
    ASSERT_EQ(result->fills.size(), 1u);
    EXPECT_EQ(result->fills[0].side, Side::kBuy);
    EXPECT_EQ(result->fills[0].price, D("201"));
    EXPECT_EQ(result->fills[0].time, Hour(1));
}

TEST(BacktestTest, RejectsAnEndThatIsNotAfterTheStart) {
    auto options = Options();
    options.end = options.start;
    auto result = RunBacktest(Data(kCloses), Sma(), options);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().category, ErrorCategory::kInvalidArgument);
}

TEST(LoadReplayDataTest, LoadsInfoAndBarsIncludingTheWarmup) {
    auto source = Data(kCloses);  // hours 0-7
    auto needs = Sma()->Needs();  // 4 warmup bars (slow period + 1)
    // Replaying from hour 6: the warmup reaches back to hour 1 at least.
    auto loaded = backtest::LoadReplayData(*source, needs, Hour(6), Hour(8));
    ASSERT_OK(loaded);
    EXPECT_TRUE((*loaded)->GetSymbolInfo(kSymbol).has_value());
    const auto& bars = (*loaded)->Series(kSymbol, Timeframe::kH1);
    ASSERT_FALSE(bars.empty());
    EXPECT_LE(bars.front().open_time, Hour(2));
    EXPECT_EQ(bars.back().open_time, Hour(7));
}

TEST(LoadReplayDataTest, OnlyLoadsBarsBeforeTheEnd) {
    auto loaded = backtest::LoadReplayData(*Data(kCloses), Sma()->Needs(),
                                           Hour(4), Hour(6));
    ASSERT_OK(loaded);
    EXPECT_EQ((*loaded)->Series(kSymbol, Timeframe::kH1).back().open_time,
              Hour(5));
}

TEST(LoadReplayDataTest, ReportsWhatCannotBeLoaded) {
    auto data = Data(kCloses);
    auto backwards =
        backtest::LoadReplayData(*data, Sma()->Needs(), Hour(6), Hour(6));
    ASSERT_FALSE(backwards);
    EXPECT_EQ(backwards.error().category, ErrorCategory::kInvalidArgument);

    auto monthly = backtest::LoadReplayData(*data, MonthlyStrategy().Needs(),
                                            Hour(1), Hour(5));
    ASSERT_FALSE(monthly);
    EXPECT_EQ(monthly.error().category, ErrorCategory::kUnsupported);

    // No symbol info for the symbol: the source's own error comes through.
    auto bare = std::make_shared<ReplayDataSource>();
    auto missing =
        backtest::LoadReplayData(*bare, Sma()->Needs(), Hour(1), Hour(5));
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().category, ErrorCategory::kNotFound);
}

}  // namespace
