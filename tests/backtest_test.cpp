#include "kestrel/backtest/backtest.hpp"

#include <array>

#include "kestrel/strategy/sma_crossover.hpp"
#include "kestrel/types/market_hours.hpp"
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
    data->SetSymbolInfo({kSymbol, D("0.01"), D("0.00000001"), D("0.0001"), {}});
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
        EXPECT_EQ(quotes[i].last_taker_side, trades[i].taker_side);
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

// These fill at the close an order was placed on (fill_at_close), the
// arithmetic the expectations below use; the default is checked further on.
BacktestOptions AtClose() {
    auto o = Options();
    o.fill_at_close = true;
    return o;
}

TEST(BacktestTest, RunsASmaCrossoverEndToEnd) {
    auto result = RunBacktest(Data(kCloses), Sma(), AtClose());
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

TEST(BacktestTest, SummarizesTheRun) {
    auto result = RunBacktest(Data(kCloses), Sma(), AtClose());
    ASSERT_OK(result);
    ASSERT_OK(result->summary);
    const auto& s = *result->summary;

    EXPECT_EQ(s.quote_asset, "USD");
    EXPECT_EQ(s.start_time, Hour(5));  // the first bar replayed closes
    EXPECT_EQ(s.end_time, Hour(8));
    EXPECT_EQ(s.start_equity, D("10000"));
    EXPECT_EQ(s.end_equity, D("9992.5"));  // the venue's USD balance
    EXPECT_EQ(s.hold_equity, D("10000"));  // nothing but cash to hold
    EXPECT_EQ(s.realized, D("-7.5"));
    EXPECT_EQ(s.unrealized, D("0"));
    EXPECT_EQ(s.fills, 2u);
    EXPECT_EQ(s.closing_sells, 1u);
    EXPECT_EQ(s.winning_sells, 0u);
    // 0.5 BTC bought at 20 is worth 10 at hour 7's close of 20 and 2.5 at
    // hour 8's close of 5: equity falls from 10000 to 9992.5.
    EXPECT_EQ(s.max_drawdown, D("7.5"));
    // Every minute from hour 5 to hour 8.
    ASSERT_EQ(s.curve.size(), 181u);
    EXPECT_EQ(s.curve.front().time, Hour(5));
    EXPECT_EQ(s.curve.back().time, Hour(8));
    EXPECT_EQ(s.curve.back().equity, D("9992.5"));
}

TEST(BacktestTest, AppliesSlippageAndFees) {
    auto options = AtClose();
    options.paper.slippage = D("0.01");
    options.paper.taker_fee_rate = D("0.001");
    auto result = RunBacktest(Data(kCloses), Sma(), options);
    ASSERT_OK(result);
    ASSERT_EQ(result->fills.size(), 2u);
    EXPECT_EQ(result->fills[0].price, D("20.2"));  // ask * 1.01
    EXPECT_EQ(result->fills[0].fee, D("0.0101"));  // 0.1% of 10.1
    EXPECT_EQ(result->fills[1].price, D("4.95"));  // bid * 0.99
}

TEST(BacktestTest, AnOrderPlacedOnACloseFillsAtTheNextOpen) {
    // The up-cross at hour 5's close buys at hour 6's open; the down-cross at
    // hour 7's close (5) sells at hour 8's open, 6, not at that close.
    auto data = std::make_shared<ReplayDataSource>();
    auto bars = Series(kCloses);
    types::Bar next = BarAt(8, "7");
    next.open = D("6");
    next.low = D("6");
    bars.push_back(next);
    data->AddBars(kSymbol, Timeframe::kH1, bars);
    data->SetSymbolInfo({kSymbol, D("0.01"), D("0.00000001"), D("0.0001"), {}});
    auto options = Options();
    options.end = Hour(9);
    auto result = RunBacktest(data, Sma(), options);
    ASSERT_OK(result);
    ASSERT_EQ(result->fills.size(), 2u);
    EXPECT_EQ(result->fills[0].price, D("20"));
    EXPECT_EQ(result->fills[0].time, Hour(6));
    EXPECT_EQ(result->fills[1].side, Side::kSell);
    EXPECT_EQ(result->fills[1].price, D("6"));
    EXPECT_EQ(result->fills[1].time, Hour(8));
}

TEST(BacktestTest, AnOrderPlacedOnTheLastCloseDoesNotFill) {
    // Nothing is replayed after hour 7's close, so its sell never trades.
    auto result = RunBacktest(Data(kCloses), Sma(), Options());
    ASSERT_OK(result);
    ASSERT_EQ(result->fills.size(), 1u);
    EXPECT_EQ(result->fills[0].side, Side::kBuy);
    ASSERT_EQ(result->positions.size(), 1u);
    EXPECT_EQ(result->positions[0].quantity, D("0.5"));
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
    // The runner checks every symbol before it starts.
    auto result = RunBacktest(data, Sma(), Options());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().category, ErrorCategory::kNotFound);
    EXPECT_NE(result.error().message.find(kSymbol), std::string::npos);
}

// A market open from 04:30 to 06:30 UTC on 1970-01-01 only.
class EarlyCalendar : public types::TradingCalendar {
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

// Buys on every bar, and notes the session hooks and when they came.
class SessionBuyer : public strategy::Strategy {
   public:
    std::string_view Name() const override { return "session_buyer"; }
    strategy::Requirements Needs() const override {
        strategy::Requirements r;
        r.symbols = {kSymbol};
        r.bar_timeframes = {Timeframe::kH1};
        r.warmup_bars = 1;
        return r;
    }
    void OnBar(strategy::Context& context, const types::Symbol&, Timeframe,
               const types::Bar&) override {
        types::OrderRequest r;
        r.symbol = kSymbol;
        r.side = types::Side::kBuy;
        r.type = types::OrderType::kMarket;
        r.quantity = D("0.01");
        events.push_back(
            "bar " + types::FormatIso8601(context.Now()) +
            (context.Venue().PlaceOrder(r) ? " bought" : " refused"));
    }
    void OnSessionOpen(strategy::Context& context) override {
        events.push_back("open " + types::FormatIso8601(context.Now()));
    }
    void OnSessionClosing(strategy::Context& context,
                          types::Timestamp) override {
        events.push_back("closing " + types::FormatIso8601(context.Now()));
    }
    std::vector<std::string>& events;
    explicit SessionBuyer(std::vector<std::string>& out) : events(out) {}
};

TEST(BacktestTest, SessionHooksAtTheirReplayTimesAndNoOrdersWhileClosed) {
    auto data = Data(kCloses);
    data->SetHours(std::make_shared<types::MarketHours>(
        std::make_shared<EarlyCalendar>(), false));
    std::vector<std::string> events;
    auto result =
        RunBacktest(data, std::make_unique<SessionBuyer>(events), Options());
    ASSERT_OK(result);
    // Bars close on the hour: 05:00 and 06:00 are in the session, 07:00 and
    // 08:00 after it.
    EXPECT_EQ(events, (std::vector<std::string>{
                          "open 1970-01-01T04:30:00.000Z",
                          "bar 1970-01-01T05:00:00.000Z bought",
                          "bar 1970-01-01T06:00:00.000Z bought",
                          "closing 1970-01-01T06:25:00.000Z",
                          "bar 1970-01-01T07:00:00.000Z refused",
                          "bar 1970-01-01T08:00:00.000Z refused"}));
    EXPECT_EQ(result->fills.size(), 2u);
}

// A market open from 07:30 to 08:30 UTC on 1970-01-01 only.
class LateCalendar : public types::TradingCalendar {
   public:
    std::optional<types::TradingDay> On(
        std::chrono::year_month_day date) const override {
        if (date != std::chrono::year_month_day{std::chrono::sys_days{}}) {
            return std::nullopt;
        }
        const auto open = Hour(7) + std::chrono::minutes(30);
        const auto close = Hour(8) + std::chrono::minutes(30);
        return types::TradingDay{open, open, close, close};
    }
    std::chrono::year_month_day DateAt(types::Timestamp t) const override {
        return std::chrono::year_month_day{
            std::chrono::floor<std::chrono::days>(t)};
    }
};

// The last bar closes at 08:00; the closing hook, at 08:25, is still before
// the backtest's end and must come.
TEST(BacktestTest, SessionHooksAfterTheLastEventStillCome) {
    auto data = Data(kCloses);
    data->SetHours(std::make_shared<types::MarketHours>(
        std::make_shared<LateCalendar>(), false));
    std::vector<std::string> events;
    auto options = Options();
    options.end = Hour(9);
    auto result =
        RunBacktest(data, std::make_unique<SessionBuyer>(events), options);
    ASSERT_OK(result);
    ASSERT_FALSE(events.empty());
    EXPECT_EQ(events.back(), "closing 1970-01-01T08:25:00.000Z");
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
// Hourly bars with their own open, high, low and close.
std::shared_ptr<ReplayDataSource> OhlcData(
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
    data->SetSymbolInfo({kSymbol, D("0.01"), D("0.00000001"), D("0.0001"), {}});
    return data;
}

// Places `orders` on the first bar it sees (hour 4's, closing at 05:00);
// with `protect`, a sell-stop at that price once its buy fills.
class BarOrders : public strategy::Strategy {
   public:
    BarOrders(std::vector<types::OrderRequest> orders,
              std::optional<const char*> protect = std::nullopt,
              std::vector<Timeframe> timeframes = {Timeframe::kH1})
        : mOrders(std::move(orders)),
          mProtect(protect),
          mTimeframes(std::move(timeframes)) {}
    std::string_view Name() const override { return "bar-orders"; }
    strategy::Requirements Needs() const override {
        strategy::Requirements r;
        r.symbols = {kSymbol};
        r.bar_timeframes = mTimeframes;
        r.warmup_bars = 1;
        return r;
    }
    void OnBar(strategy::Context& ctx, const types::Symbol&, Timeframe,
               const types::Bar&) override {
        for (const auto& order : mOrders) {
            EXPECT_TRUE(ctx.Venue().PlaceOrder(order).has_value());
        }
        mOrders.clear();
    }
    void OnFill(strategy::Context& ctx, const types::Fill& fill) override {
        if (mProtect && fill.side == types::Side::kBuy) {
            types::OrderRequest stop;
            stop.symbol = kSymbol;
            stop.side = types::Side::kSell;
            stop.type = types::OrderType::kStop;
            stop.quantity = fill.quantity;
            stop.stop_price = D(*mProtect);
            EXPECT_TRUE(ctx.Venue().PlaceOrder(stop).has_value());
            mProtect.reset();
        }
    }

   private:
    std::vector<types::OrderRequest> mOrders;
    std::optional<const char*> mProtect;
    std::vector<Timeframe> mTimeframes;
};

types::OrderRequest Stop(types::Side side, const char* price) {
    types::OrderRequest r;
    r.symbol = kSymbol;
    r.side = side;
    r.type = types::OrderType::kStop;
    r.quantity = D("1");
    r.stop_price = D(price);
    return r;
}

types::OrderRequest LimitBuy(const char* price) {
    types::OrderRequest r;
    r.symbol = kSymbol;
    r.side = types::Side::kBuy;
    r.type = types::OrderType::kLimit;
    r.quantity = D("1");
    r.limit_price = D(price);
    return r;
}

// Hours 0-4 are flat at 10; hour 5's bar is `bar` (the one walked after the
// orders are placed at 05:00).
std::vector<std::array<const char*, 4>> WithBar5(
    std::array<const char*, 4> bar) {
    std::vector<std::array<const char*, 4>> bars(5, {"10", "10", "10", "10"});
    bars.push_back(bar);
    bars.push_back({"12", "12", "12", "12"});
    return bars;
}

// Without recorded quotes the venue walks each bar: open, the extreme nearer
// the open, the other extreme, close, stopping at every resting order's
// price. A stop inside the bar fills at its own price, not at the close.
TEST(BacktestTest, AStopInsideABarFillsAtItsPrice) {
    // 10 -> 9 (the nearer extreme) -> 20 -> 12: the buy-stop at 15 is
    // crossed on the way up, though the bar closes below it.
    auto result = RunBacktest(
        OhlcData(WithBar5({"10", "20", "9", "12"})),
        std::make_unique<BarOrders>(std::vector{Stop(types::Side::kBuy, "15")}),
        Options());
    ASSERT_OK(result);
    ASSERT_EQ(result->fills.size(), 1u);
    EXPECT_EQ(result->fills[0].price, D("15"));
    // At the start of its leg: the second third of hour 5.
    EXPECT_EQ(result->fills[0].time, Hour(5) + std::chrono::minutes(20));
}

TEST(BacktestTest, AGapThroughAStopFillsAtTheOpen) {
    auto result = RunBacktest(
        OhlcData(WithBar5({"18", "20", "17", "19"})),
        std::make_unique<BarOrders>(std::vector{Stop(types::Side::kBuy, "15")}),
        Options());
    ASSERT_OK(result);
    ASSERT_EQ(result->fills.size(), 1u);
    EXPECT_EQ(result->fills[0].price, D("18"));
    EXPECT_EQ(result->fills[0].time, Hour(5));
}

TEST(BacktestTest, AStopPlacedOnAFillTradesInTheRestOfTheBar) {
    // Entry at 15 on the way up to 20, then the protective stop at 14 on the
    // way down to 12, all in hour 5.
    auto result =
        RunBacktest(OhlcData(WithBar5({"10", "20", "9", "12"})),
                    std::make_unique<BarOrders>(
                        std::vector{Stop(types::Side::kBuy, "15")}, "14"),
                    Options());
    ASSERT_OK(result);
    ASSERT_EQ(result->fills.size(), 2u);
    EXPECT_EQ(result->fills[0].side, types::Side::kBuy);
    EXPECT_EQ(result->fills[0].price, D("15"));
    EXPECT_EQ(result->fills[1].side, types::Side::kSell);
    EXPECT_EQ(result->fills[1].price, D("14"));
    EXPECT_EQ(result->fills[1].time, Hour(5) + std::chrono::minutes(40));
}

// The walk uses each symbol's finest series that has bars: a strategy asking
// for 30-minute bars it does not get still trades on the hourly ones.
TEST(BacktestTest, ASymbolWithoutTheFinestSeriesWalksItsCoarserOne) {
    auto result = RunBacktest(
        OhlcData(WithBar5({"10", "20", "9", "12"})),
        std::make_unique<BarOrders>(
            std::vector{Stop(types::Side::kBuy, "15")}, std::nullopt,
            std::vector{Timeframe::kM30, Timeframe::kH1}),
        Options());
    ASSERT_OK(result);
    ASSERT_EQ(result->fills.size(), 1u);
    EXPECT_EQ(result->fills[0].price, D("15"));
}

TEST(BacktestTest, ARestingLimitFillsWhenTheBarReachesIt) {
    // The low of 9 passes the buy limit at 9.5: filled at 9.5, as a maker.
    auto result = RunBacktest(
        OhlcData(WithBar5({"10", "11", "9", "10.5"})),
        std::make_unique<BarOrders>(std::vector{LimitBuy("9.5")}), Options());
    ASSERT_OK(result);
    ASSERT_EQ(result->fills.size(), 1u);
    EXPECT_EQ(result->fills[0].price, D("9.5"));
    EXPECT_EQ(result->fills[0].liquidity, types::Liquidity::kMaker);
}

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

TEST(BacktestTest, SummarizesAQuoteStrategyAtTheMid) {
    std::vector<RecordingBuyer::Event> events;
    auto options = Options();
    options.start = Hour(0);
    options.end = Hour(1);
    auto result = RunBacktest(QuoteData(), std::make_unique<RecordingBuyer>(events),
                              options);
    ASSERT_OK(result);
    ASSERT_OK(result->summary);
    ASSERT_EQ(result->fills.size(), 1u);
    const auto& last = result->summary->symbols.at(0);
    // The end equity is the venue's balances valued at the last quote's mid.
    types::Decimal want;
    for (const auto& balance : result->balances) {
        want += balance.asset == "USD" ? balance.total
                                       : balance.total * last.last_mark;
    }
    EXPECT_EQ(result->summary->end_equity, want);
    // What the fills made by the end is what trading added beyond holding,
    // before the fees.
    const auto& s = *result->summary;
    ASSERT_FALSE(s.markouts.empty());
    EXPECT_FALSE(s.markouts.back().horizon);
    EXPECT_EQ(s.markouts.back().fills, 1u);
    EXPECT_EQ(s.markouts.back().pnl, s.end_equity - s.hold_equity + s.fees);
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
