#include "kestrel/strategy/runner.hpp"

#include <atomic>
#include <future>
#include <thread>

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
    }
    void OnFill(strategy::Context&, const types::Fill& fill) override {
        fills.push_back(fill.fill_id);
        if (throw_on_fill) {
            throw std::runtime_error("boom");
        }
    }
    void OnStop(strategy::Context&) override { stopped = true; }

    Requirements requirements;
    bool throw_on_bar = false;
    bool throw_on_fill = false;
    std::vector<std::string> fills;
    std::promise<void> started;
    std::vector<types::Timestamp> start_window;
    std::vector<types::Timestamp> bars;
    std::vector<std::size_t> window_sizes;
    std::vector<types::Symbol> quotes;
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
