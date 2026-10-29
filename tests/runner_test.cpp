#include "kestrel/strategy/runner.hpp"

#include <atomic>
#include <future>
#include <thread>
#include <tuple>

#include "strategy_fakes.hpp"
#include "test_helpers.hpp"

namespace {

using namespace kestrel;
using kestrel::testing::RecordingVenue;
using strategy::Requirements;
using types::ErrorCategory;
using types::Timeframe;

constexpr const char* kSymbol = "BTC/USD";

types::Timestamp Hour(int h) { return types::FromEpochSeconds(h * 3600); }

types::Bar BarAt(int hour) {
    types::Bar bar;
    bar.open_time = Hour(hour);
    bar.open = bar.high = bar.low = bar.close = types::Decimal::FromInt(hour);
    return bar;
}

class FakeDataSource : public data::DataSource {
   public:
    // Like the real ones, closes on Stop() or destruction, once.
    class Sub : public data::Subscription {
       public:
        explicit Sub(std::atomic<int>& stops) : mStops(stops) {}
        ~Sub() override { Stop(); }
        void Stop() override {
            if (!mStopped.exchange(true)) {
                ++mStops;
            }
        }

       private:
        std::atomic<int>& mStops;
        std::atomic<bool> mStopped{false};
    };

    std::string_view Name() const override { return "fake"; }

    types::Result<std::vector<types::Bar>> GetBars(
        const types::Symbol&, Timeframe, types::Timestamp start,
        types::Timestamp end) override {
        if (fail_get_bars) {
            return types::MakeError(ErrorCategory::kNetwork, "down");
        }
        get_bars_ranges.emplace_back(start, end);
        std::vector<types::Bar> out;
        for (const auto& bar : history) {
            if (bar.open_time >= start && bar.open_time < end) {
                out.push_back(bar);
            }
        }
        return out;
    }
    types::Result<std::unique_ptr<data::Subscription>> StreamQuotes(
        const std::vector<std::string>&, data::QuoteCallback cb) override {
        quote_callback = std::move(cb);
        return std::unique_ptr<data::Subscription>(new Sub(stops));
    }
    types::Result<std::unique_ptr<data::Subscription>> StreamBars(
        const std::vector<std::string>&, Timeframe,
        data::BarCallback cb) override {
        if (fail_stream_bars) {
            return types::MakeError(ErrorCategory::kNotImplemented, "no");
        }
        bar_callback = std::move(cb);
        return std::unique_ptr<data::Subscription>(new Sub(stops));
    }
    types::Result<types::SymbolInfo> GetSymbolInfo(
        const types::Symbol&) override {
        return types::MakeError(ErrorCategory::kNotFound, "none");
    }

    void Close(int hour) { bar_callback(kSymbol, Timeframe::kH1, BarAt(hour)); }

    bool fail_get_bars = false;
    bool fail_stream_bars = false;
    std::vector<types::Bar> history;
    std::vector<std::pair<types::Timestamp, types::Timestamp>> get_bars_ranges;
    data::BarCallback bar_callback;
    data::QuoteCallback quote_callback;
    std::atomic<int> stops{0};
};

// Records what the runner does; only the runner thread touches it until the
// test has joined that thread.
class RecordingStrategy : public strategy::Strategy {
   public:
    std::string_view Name() const override { return "recording"; }
    Requirements Needs() const override { return requirements; }

    void OnStart(strategy::Context& context) override {
        for (int i = 0; i < orders_on_start; ++i) {
            types::OrderRequest request;
            request.symbol = kSymbol;
            request.type = types::OrderType::kLimit;
            request.quantity = types::Decimal::FromInt(1);
            request.limit_price = types::Decimal::FromInt(10);
            (void)context.Venue().PlaceOrder(request);
        }
        auto bars = context.Bars(kSymbol, Timeframe::kH1);
        for (const auto& bar : bars) {
            start_window.push_back(bar.open_time);
        }
        started.set_value();
    }
    void OnBar(strategy::Context& context, const types::Symbol&, Timeframe tf,
               const types::Bar& bar) override {
        if (throw_on_bar && bars.empty()) {
            bars.push_back(bar.open_time);  // count it, then fail
            throw std::runtime_error("boom");
        }
        bars.push_back(bar.open_time);
        window_sizes.push_back(context.Bars(kSymbol, tf).size());
    }
    void OnQuote(strategy::Context&, const types::Quote& quote) override {
        quotes.push_back(quote.symbol);
        full_quotes.push_back(quote);
    }
    void OnFill(strategy::Context&, const types::Fill& fill) override {
        fills.push_back(fill.fill_id);
        if (throw_on_fill) {
            throw std::runtime_error("boom");
        }
    }
    void OnOrderUpdate(strategy::Context&,
                       const types::OrderStatus& status) override {
        updates.emplace_back(status.order_id, status.state,
                             status.filled_quantity);
    }
    void OnStop(strategy::Context&) override { stopped = true; }

    Requirements requirements;
    bool throw_on_bar = false;
    bool throw_on_fill = false;
    int orders_on_start = 0;
    std::vector<std::tuple<std::string, types::OrderState, types::Decimal>>
        updates;
    std::vector<std::string> fills;
    std::promise<void> started;
    std::vector<types::Timestamp> start_window;
    std::vector<types::Timestamp> bars;
    std::vector<std::size_t> window_sizes;
    std::vector<types::Symbol> quotes;
    std::vector<types::Quote> full_quotes;
    bool stopped = false;
};

class RunnerTest : public ::testing::Test {
   protected:
    RunnerTest() {
        for (int h = 0; h <= 10; ++h) {
            data->history.push_back(BarAt(h));
        }
        strategy_owner = std::make_unique<RecordingStrategy>();
        strategy = strategy_owner.get();
        strategy->requirements.symbols = {kSymbol};
        strategy->requirements.bar_timeframes = {Timeframe::kH1};
        strategy->requirements.warmup_bars = 3;
    }

    ~RunnerTest() override {
        if (thread.joinable()) {
            runner->Stop();
            thread.join();
        }
    }

    // The clock is at hour 5, so the warmup sees hours 0-4.
    void Build() {
        runner = std::make_unique<strategy::Runner>(
            data, venue, std::move(strategy_owner), options,
            [] { return Hour(5); });
    }

    // Runs the runner on its own thread and returns once OnStart has run, so
    // the stream callbacks are set.
    void Start() {
        Build();
        auto started = strategy->started.get_future();
        thread = std::thread([this] { result = runner->Run(); });
        started.wait();
    }

    void Finish() {
        runner->Stop();
        thread.join();
    }

    strategy::RunnerOptions options;
    std::shared_ptr<FakeDataSource> data = std::make_shared<FakeDataSource>();
    std::shared_ptr<RecordingVenue> venue = std::make_shared<RecordingVenue>();
    std::unique_ptr<RecordingStrategy> strategy_owner;
    RecordingStrategy* strategy = nullptr;
    std::unique_ptr<strategy::Runner> runner;
    std::thread thread;
    types::Result<void> result;
};

TEST_F(RunnerTest, LoadsTheNewestWarmupBarsBeforeOnStart) {
    Start();
    Finish();
    ASSERT_OK(result);
    EXPECT_EQ(strategy->start_window,
              (std::vector<types::Timestamp>{Hour(2), Hour(3), Hour(4)}));
}

TEST_F(RunnerTest, DeliversBarsInOrderWithABoundedWindow) {
    Start();
    data->Close(5);
    data->Close(6);
    Finish();
    EXPECT_EQ(strategy->bars,
              (std::vector<types::Timestamp>{Hour(5), Hour(6)}));
    EXPECT_EQ(strategy->window_sizes, (std::vector<std::size_t>{3, 3}));
    EXPECT_TRUE(strategy->stopped);
}

TEST_F(RunnerTest, DropsBarsItHasAlreadySeen) {
    Start();
    data->Close(5);
    data->Close(5);  // reconnect duplicate
    data->Close(4);  // already in the warmup window
    data->Close(6);
    Finish();
    EXPECT_EQ(strategy->bars,
              (std::vector<types::Timestamp>{Hour(5), Hour(6)}));
}

TEST_F(RunnerTest, BackfillsBarsMissedWhileDisconnected) {
    Start();
    data->Close(8);  // hours 5-7 closed while the stream was down
    Finish();
    EXPECT_EQ(strategy->bars, (std::vector<types::Timestamp>{
                                  Hour(5), Hour(6), Hour(7), Hour(8)}));
    ASSERT_EQ(data->get_bars_ranges.size(), 2u);  // warmup, then the gap
    EXPECT_EQ(data->get_bars_ranges[1].first, Hour(5));
    EXPECT_EQ(data->get_bars_ranges[1].second, Hour(8));
}

TEST_F(RunnerTest, StillDeliversTheBarWhenBackfillFails) {
    Start();
    data->fail_get_bars = true;
    data->Close(8);
    Finish();
    EXPECT_EQ(strategy->bars, std::vector<types::Timestamp>{Hour(8)});
}

TEST_F(RunnerTest, DeliversQuotesOnlyWhenAskedFor) {
    strategy->requirements.quotes = true;
    Start();
    types::Quote quote;
    quote.symbol = kSymbol;
    data->quote_callback(quote);
    Finish();
    EXPECT_EQ(strategy->quotes, std::vector<types::Symbol>{kSymbol});
}

TEST_F(RunnerTest, ConflatesQueuedQuotesPerSymbol) {
    strategy->requirements.quotes = true;
    Build();
    ASSERT_OK(runner->Begin());

    auto quote = [](const char* symbol, int second) {
        types::Quote q;
        q.symbol = symbol;
        q.received_time = types::FromEpochSeconds(second);
        return q;
    };
    types::Quote first = quote(kSymbol, 1);
    first.bid = types::Decimal::FromInt(100);
    first.ask = types::Decimal::FromInt(102);
    types::Quote second = quote(kSymbol, 2);
    second.bid = types::Decimal::FromInt(101);  // no ask: keeps the first's
    types::Quote third = quote(kSymbol, 3);
    third.last = types::Decimal::FromInt(101);
    types::Quote other = quote("ETH/USD", 2);
    other.bid = types::Decimal::FromInt(5);

    data->quote_callback(first);
    data->quote_callback(other);
    data->quote_callback(second);
    data->quote_callback(third);
    EXPECT_EQ(runner->ConflatedQuotes(), 2u);

    EXPECT_TRUE(runner->Drain());
    // One quote per symbol, in the order the symbols first arrived.
    ASSERT_EQ(strategy->full_quotes.size(), 2u);
    const auto& merged = strategy->full_quotes[0];
    EXPECT_EQ(merged.symbol, kSymbol);
    EXPECT_EQ(merged.received_time, types::FromEpochSeconds(3));
    EXPECT_EQ(merged.bid, types::Decimal::FromInt(101));
    EXPECT_EQ(merged.ask, types::Decimal::FromInt(102));
    EXPECT_EQ(merged.last, types::Decimal::FromInt(101));
    EXPECT_EQ(strategy->full_quotes[1].symbol, "ETH/USD");

    // Once delivered, the next quote starts afresh rather than merging into
    // the old one.
    types::Quote later = quote(kSymbol, 9);
    later.bid = types::Decimal::FromInt(50);
    data->quote_callback(later);
    EXPECT_TRUE(runner->Drain());
    ASSERT_EQ(strategy->full_quotes.size(), 3u);
    EXPECT_EQ(strategy->full_quotes[2].bid, types::Decimal::FromInt(50));
    EXPECT_FALSE(strategy->full_quotes[2].ask);
    EXPECT_EQ(runner->ConflatedQuotes(), 2u);
    runner->End();
}

types::OrderStatus StatusOf(const char* id, types::OrderState state,
                            int filled = 0) {
    types::OrderStatus status;
    status.order_id = id;
    status.state = state;
    status.quantity = types::Decimal::FromInt(1);
    status.filled_quantity = types::Decimal::FromInt(filled);
    return status;
}

using Update = std::tuple<std::string, types::OrderState, types::Decimal>;

TEST_F(RunnerTest, ReportsAnOrderRejectedAfterItWasAccepted) {
    strategy->orders_on_start = 1;  // acknowledged as kNew: "order-1"
    Build();
    ASSERT_OK(runner->Begin());

    // Still resting: nothing to report.
    venue->open_orders = {StatusOf("order-1", types::OrderState::kNew)};
    EXPECT_TRUE(runner->Drain());
    EXPECT_TRUE(strategy->updates.empty());

    // It leaves the open list, and the venue says why.
    venue->open_orders.clear();
    venue->order_states["order-1"] =
        StatusOf("order-1", types::OrderState::kRejected);
    EXPECT_TRUE(runner->Drain());
    ASSERT_EQ(strategy->updates.size(), 1u);
    EXPECT_EQ(strategy->updates[0],
              Update("order-1", types::OrderState::kRejected,
                     types::Decimal::FromInt(0)));

    // A closed order is not looked at again.
    const int lookups = venue->get_order_calls;
    EXPECT_TRUE(runner->Drain());
    EXPECT_EQ(venue->get_order_calls, lookups);
    EXPECT_EQ(strategy->updates.size(), 1u);
    runner->End();
}

TEST_F(RunnerTest, ReportsPartialFillsThenTheFinalState) {
    strategy->orders_on_start = 1;
    Build();
    ASSERT_OK(runner->Begin());

    venue->open_orders = {
        StatusOf("order-1", types::OrderState::kPartiallyFilled, 0)};
    venue->open_orders[0].filled_quantity = types::Decimal(5, 1);
    EXPECT_TRUE(runner->Drain());
    EXPECT_TRUE(runner->Drain());  // unchanged: not reported again
    ASSERT_EQ(strategy->updates.size(), 1u);
    EXPECT_EQ(std::get<1>(strategy->updates[0]),
              types::OrderState::kPartiallyFilled);
    EXPECT_EQ(std::get<2>(strategy->updates[0]), types::Decimal(5, 1));

    venue->open_orders.clear();
    venue->order_states["order-1"] =
        StatusOf("order-1", types::OrderState::kFilled, 1);
    EXPECT_TRUE(runner->Drain());
    ASSERT_EQ(strategy->updates.size(), 2u);
    EXPECT_EQ(std::get<1>(strategy->updates[1]), types::OrderState::kFilled);
    runner->End();
}

TEST_F(RunnerTest, DoesNotReportAnOrderThatWasAlreadyClosedWhenAcknowledged) {
    strategy->orders_on_start = 1;
    venue->ack_state = types::OrderState::kRejected;
    Build();
    ASSERT_OK(runner->Begin());
    EXPECT_TRUE(runner->Drain());
    EXPECT_TRUE(strategy->updates.empty());
    EXPECT_EQ(venue->get_order_calls, 0);
    runner->End();
}

TEST_F(RunnerTest, GivesUpOnAnOrderTheVenueDoesNotKnow) {
    strategy->orders_on_start = 1;
    Build();
    ASSERT_OK(runner->Begin());
    EXPECT_TRUE(runner->Drain());  // not open, not found
    EXPECT_TRUE(runner->Drain());
    EXPECT_EQ(venue->get_order_calls, 1);
    EXPECT_TRUE(strategy->updates.empty());
    runner->End();
}

TEST_F(RunnerTest, RetriesAnOrderLookupThatFailedForOtherReasons) {
    strategy->orders_on_start = 1;
    Build();
    ASSERT_OK(runner->Begin());
    venue->fail_reads = true;  // GetOpenOrders fails
    EXPECT_TRUE(runner->Drain());
    EXPECT_TRUE(strategy->updates.empty());

    venue->fail_reads = false;
    venue->order_states["order-1"] =
        StatusOf("order-1", types::OrderState::kCancelled);
    EXPECT_TRUE(runner->Drain());
    ASSERT_EQ(strategy->updates.size(), 1u);
    EXPECT_EQ(std::get<1>(strategy->updates[0]), types::OrderState::kCancelled);
    runner->End();
}

TEST_F(RunnerTest, DoesNotAskAboutOrdersWhenThereAreNone) {
    Build();
    ASSERT_OK(runner->Begin());
    EXPECT_TRUE(runner->Drain());
    EXPECT_EQ(venue->get_order_calls, 0);
    runner->End();
}

TEST_F(RunnerTest, DoesNotSubscribeToQuotesNobodyWants) {
    Start();
    Finish();
    EXPECT_FALSE(data->quote_callback);
}

TEST_F(RunnerTest, SurvivesAHookThatThrows) {
    strategy->throw_on_bar = true;
    Start();
    data->Close(5);
    data->Close(6);
    Finish();
    ASSERT_OK(result);
    EXPECT_EQ(strategy->bars,
              (std::vector<types::Timestamp>{Hour(5), Hour(6)}));
}

TEST_F(RunnerTest, StopEndsTheStreamsAndCallsOnStop) {
    strategy->requirements.quotes = true;
    Start();
    Finish();
    ASSERT_OK(result);
    EXPECT_EQ(data->stops.load(), 2);  // bars and quotes
    EXPECT_TRUE(strategy->stopped);
}

types::Fill FillAt(const char* id, int hour) {
    types::Fill fill;
    fill.fill_id = id;
    fill.symbol = kSymbol;
    fill.time = Hour(hour);
    return fill;
}

TEST_F(RunnerTest, PollsForFillsAndDeliversEachOnce) {
    venue->fills = {FillAt("old", 3), FillAt("f1", 6), FillAt("f2", 6)};
    options.fill_poll_interval = std::chrono::milliseconds(2);
    Start();
    // Several polls, each of which sees f1 and f2 again.
    for (int i = 0; i < 1000 && venue->get_fills_calls < 5; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_GE(venue->get_fills_calls, 5);
    Finish();
    // "old" predates the start of the run.
    EXPECT_EQ(strategy->fills, (std::vector<std::string>{"f1", "f2"}));
}

TEST_F(RunnerTest, DoesNotPollWhenIntervalIsZero) {
    venue->fills = {FillAt("f1", 6)};
    options.fill_poll_interval = std::chrono::milliseconds(0);
    Start();
    data->Close(5);  // events still flow
    Finish();
    EXPECT_EQ(venue->get_fills_calls, 0);
    EXPECT_TRUE(strategy->fills.empty());
}

types::Fill FillAtMinute(const char* id, int minutes_after_start) {
    types::Fill fill = FillAt(id, 5);
    fill.time += std::chrono::minutes(minutes_after_start);
    return fill;
}

trading::ExecutionUpdate Pushed(const types::Fill& fill) {
    return trading::ExecutionUpdate{fill, std::nullopt};
}

TEST_F(RunnerTest, DeliversPushedFillsWithoutPolling) {
    venue->pushes = true;
    options.fill_poll_interval = std::chrono::milliseconds(1);
    options.fill_backstop_interval = std::chrono::milliseconds(0);
    Start();
    venue->Push(Pushed(FillAtMinute("f1", 1)));
    venue->Push(Pushed(FillAtMinute("f2", 2)));
    Finish();
    EXPECT_EQ(strategy->fills, (std::vector<std::string>{"f1", "f2"}));
    EXPECT_EQ(venue->get_fills_calls, 0);  // the 1 ms poll is not used
    EXPECT_EQ(venue->push_stops.load(), 1);
}

TEST_F(RunnerTest, AFillThatIsPushedAndPolledIsDeliveredOnce) {
    venue->pushes = true;
    Build();
    ASSERT_OK(runner->Begin());
    venue->fills = {FillAtMinute("f1", 1)};
    venue->Push(Pushed(FillAtMinute("f1", 1)));
    EXPECT_TRUE(runner->Drain());  // handles the push, then polls
    EXPECT_EQ(strategy->fills, std::vector<std::string>{"f1"});
    runner->End();
}

TEST_F(RunnerTest, TheBackstopPollFindsAnOlderFillThePushMissed) {
    venue->pushes = true;
    Build();
    ASSERT_OK(runner->Begin());
    venue->fills = {FillAt("before-the-run", 4), FillAtMinute("missed", 1),
                    FillAtMinute("pushed", 2)};
    venue->Push(Pushed(FillAtMinute("pushed", 2)));
    EXPECT_TRUE(runner->Drain());
    EXPECT_EQ(strategy->fills, (std::vector<std::string>{"pushed", "missed"}));
    runner->End();
}

TEST_F(RunnerTest, DropsAPushedFillOlderThanTheDuplicateCheckReaches) {
    venue->pushes = true;
    Build();
    ASSERT_OK(runner->Begin());
    venue->Push(Pushed(FillAtMinute("new", 30)));
    venue->Push(Pushed(FillAtMinute("stale", 19)));  // 11 minutes older
    venue->Push(Pushed(FillAtMinute("recent", 21)));
    EXPECT_TRUE(runner->Drain());
    EXPECT_EQ(strategy->fills, (std::vector<std::string>{"new", "recent"}));
    runner->End();
}

TEST_F(RunnerTest, PushedOrderChangesReachOnOrderUpdate) {
    venue->pushes = true;
    strategy->orders_on_start = 1;
    Build();
    ASSERT_OK(runner->Begin());
    types::OrderStatus status;
    status.order_id = "order-1";
    status.state = types::OrderState::kFilled;
    status.filled_quantity = types::Decimal::FromInt(1);
    types::Fill fill = FillAtMinute("f1", 1);
    fill.order_id = "order-1";
    venue->Push(trading::ExecutionUpdate{fill, status});
    venue->Push(trading::ExecutionUpdate{std::nullopt, status});  // again
    EXPECT_TRUE(runner->Drain());
    EXPECT_EQ(strategy->fills, std::vector<std::string>{"f1"});
    ASSERT_EQ(strategy->updates.size(), 1u);
    EXPECT_EQ(std::get<1>(strategy->updates[0]), types::OrderState::kFilled);
    // The order is closed, so the poll does not ask about it.
    EXPECT_EQ(venue->get_order_calls, 0);
    runner->End();
}

TEST_F(RunnerTest, PollsAsBeforeWhenTheVenueDoesNotPush) {
    venue->fills = {FillAtMinute("f1", 1)};
    options.fill_poll_interval = std::chrono::milliseconds(1);
    options.fill_backstop_interval = std::chrono::milliseconds(0);
    Start();
    for (int i = 0; i < 1000 && venue->get_fills_calls < 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Finish();
    EXPECT_GE(venue->get_fills_calls, 2);
    EXPECT_EQ(strategy->fills, std::vector<std::string>{"f1"});
}

TEST_F(RunnerTest, DrainProcessesEventsAndFillsOnTheCallingThread) {
    Build();
    ASSERT_OK(runner->Begin());
    data->Close(5);
    data->Close(6);
    venue->fills = {FillAt("f1", 6)};
    EXPECT_TRUE(runner->Drain());
    EXPECT_EQ(strategy->bars,
              (std::vector<types::Timestamp>{Hour(5), Hour(6)}));
    EXPECT_EQ(strategy->fills, std::vector<std::string>{"f1"});

    EXPECT_TRUE(runner->Drain());  // nothing new
    EXPECT_EQ(strategy->fills.size(), 1u);

    runner->End();
    EXPECT_TRUE(strategy->stopped);
    EXPECT_EQ(data->stops.load(), 1);
}

TEST_F(RunnerTest, RecordsFillsAndAFinalPnlSnapshotWhenAskedTo) {
    auto log = std::make_shared<kestrel::testing::FakeTradeLog>();
    options.trade_log = log;
    auto buy = FillAt("f1", 6);
    buy.quantity = types::Decimal::FromInt(2);
    buy.price = types::Decimal::FromInt(100);
    venue->fills = {buy};
    Build();
    ASSERT_OK(runner->Begin());
    data->Close(5);  // the mark: this bar closes at 5
    EXPECT_TRUE(runner->Drain());
    EXPECT_TRUE(strategy->fills == std::vector<std::string>{"f1"});
    runner->End();

    ASSERT_EQ(log->fills.size(), 1u);
    EXPECT_EQ(log->fills[0].fill_id, "f1");
    ASSERT_TRUE(log->finished.has_value());
    ASSERT_EQ(log->snapshots.size(), 1u);
    const auto& row = log->snapshots[0].second.at(0);
    EXPECT_EQ(row.symbol, kSymbol);
    EXPECT_EQ(row.quantity, types::Decimal::FromInt(2));
    EXPECT_EQ(row.average_price, types::Decimal::FromInt(100));
    EXPECT_EQ(row.mark_price, types::Decimal::FromInt(5));
    EXPECT_EQ(row.unrealized, types::Decimal::FromInt(-190));
}

TEST_F(RunnerTest, SnapshotsPnlWhileRunning) {
    auto log = std::make_shared<kestrel::testing::FakeTradeLog>();
    options.trade_log = log;
    options.pnl_interval = std::chrono::milliseconds(1);
    options.fill_poll_interval = std::chrono::milliseconds(2);
    // A clock that moves, so the interval elapses.
    std::atomic<int> ticks{0};
    runner = std::make_unique<strategy::Runner>(
        data, venue, std::move(strategy_owner), options,
        [&ticks] { return Hour(5) + std::chrono::milliseconds(10 * ticks++); });
    auto started = strategy->started.get_future();
    thread = std::thread([this] { result = runner->Run(); });
    started.wait();
    data->Close(5);
    for (int i = 0; i < 1000 && log->SnapshotCount() < 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    Finish();
    EXPECT_GE(log->snapshots.size(), 2u);
}

TEST_F(RunnerTest, RecordsNothingWithoutATradeLog) {
    venue->fills = {FillAt("f1", 6)};
    Build();
    ASSERT_OK(runner->Begin());
    EXPECT_TRUE(runner->Drain());
    runner->End();
    EXPECT_EQ(venue->get_fills_calls.load(), 2);  // Drain's polls only
}

TEST_F(RunnerTest, DrainReturnsFalseAfterStop) {
    Build();
    ASSERT_OK(runner->Begin());
    runner->Stop();
    EXPECT_FALSE(runner->Drain());
    runner->End();
}

TEST_F(RunnerTest, EndDoesNothingWhenBeginFailed) {
    data->fail_get_bars = true;
    Build();
    EXPECT_FALSE(runner->Begin());
    runner->End();
    EXPECT_FALSE(strategy->stopped);
}

TEST_F(RunnerTest, SurvivesAFillHookThatThrows) {
    strategy->throw_on_fill = true;
    venue->fills = {FillAt("f1", 6), FillAt("f2", 7)};
    Build();
    ASSERT_OK(runner->Begin());
    EXPECT_TRUE(runner->Drain());
    EXPECT_EQ(strategy->fills, (std::vector<std::string>{"f1", "f2"}));
    runner->End();
}

TEST_F(RunnerTest, TradesThroughTheRiskGateWhenConfigured) {
    options.risk = trading::RiskLimits{};
    options.risk->allowed_symbols = {"ETH/USD"};
    Build();
    ASSERT_OK(runner->Begin());
    ASSERT_NE(runner->RiskGate(), nullptr);

    types::OrderRequest order;
    order.symbol = kSymbol;
    order.quantity = types::Decimal::FromInt(1);
    auto r = runner->RiskGate()->PlaceOrder(order);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().category, ErrorCategory::kRejected);
    EXPECT_EQ(venue->attempts, 0);
    runner->End();
}

TEST_F(RunnerTest, FeedsTheGatePricesFromBars) {
    options.risk = trading::RiskLimits{};
    options.risk->max_order_notional = types::Decimal::FromInt(100);
    Build();
    ASSERT_OK(runner->Begin());  // warmup leaves hour 4's close (4) as price
    auto gate = runner->RiskGate();

    types::OrderRequest order;
    order.symbol = kSymbol;
    order.quantity = types::Decimal::FromInt(20);  // 80 at price 4: allowed
    EXPECT_TRUE(gate->PlaceOrder(order));

    data->Close(5);
    ASSERT_TRUE(runner->Drain());  // price is now 5: 100, still allowed
    EXPECT_TRUE(gate->PlaceOrder(order));
    data->Close(6);
    ASSERT_TRUE(runner->Drain());  // price is now 6: 120 exceeds the limit
    EXPECT_FALSE(gate->PlaceOrder(order));
    runner->End();
}

TEST_F(RunnerTest, FailsWhenTheGateCannotReadPositions) {
    options.risk = trading::RiskLimits{};
    venue->fail_reads = true;
    Build();
    auto r = runner->Begin();
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().category, ErrorCategory::kNetwork);
    EXPECT_EQ(data->stops.load(), 1);  // streams closed again
}

TEST_F(RunnerTest, CancelsOrdersAlreadyOpenBeforeOnStart) {
    venue->cancel_all_result = {"left-1", "left-2"};
    Build();
    ASSERT_OK(runner->Begin());
    EXPECT_EQ(venue->cancel_all_calls, 1);
    EXPECT_FALSE(strategy->start_window.empty());  // OnStart ran
    runner->End();
}

TEST_F(RunnerTest, DoesNotStartWhenOpenOrdersCannotBeCancelled) {
    venue->fail_cancel_all = true;
    Build();
    auto r = runner->Begin();
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().category, ErrorCategory::kNetwork);
    EXPECT_NE(r.error().message.find("left open"), std::string::npos);
    EXPECT_EQ(data->stops.load(), 0);  // nothing was subscribed
    EXPECT_FALSE(strategy->stopped);
    runner->End();  // does nothing: Begin failed
}

TEST_F(RunnerTest, FailsWithoutSymbols) {
    strategy->requirements.symbols.clear();
    Build();
    auto r = runner->Run();
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().category, ErrorCategory::kInvalidArgument);
}

TEST_F(RunnerTest, FailsWhenSubscribedToNothing) {
    strategy->requirements.bar_timeframes.clear();
    Build();
    auto r = runner->Run();
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().category, ErrorCategory::kInvalidArgument);
}

TEST_F(RunnerTest, FailsWhenTheStreamCannotBeOpened) {
    data->fail_stream_bars = true;
    Build();
    auto r = runner->Run();
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().category, ErrorCategory::kNotImplemented);
}

TEST_F(RunnerTest, FailsAndClosesStreamsWhenWarmupFails) {
    data->fail_get_bars = true;
    Build();
    auto r = runner->Run();
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().category, ErrorCategory::kNetwork);
    EXPECT_FALSE(strategy->stopped);  // never started
}

}  // namespace
