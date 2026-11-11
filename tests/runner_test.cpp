#include "kestrel/strategy/runner.hpp"

#include <spdlog/sinks/ringbuffer_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <functional>
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
        if (unknown_symbol) {
            return types::MakeError(ErrorCategory::kNotFound, "none");
        }
        return types::SymbolInfo{kSymbol,
                                 types::Decimal(1, 2),
                                 types::Decimal(1, 3),
                                 types::Decimal(1, 3),
                                 {}};
    }

    void Close(int hour) { bar_callback(kSymbol, Timeframe::kH1, BarAt(hour)); }
    std::shared_ptr<const types::MarketHours> Hours() const override {
        return hours;
    }

    bool fail_get_bars = false;
    bool fail_stream_bars = false;
    bool unknown_symbol = false;
    std::vector<types::Bar> history;
    std::shared_ptr<const types::MarketHours> hours =
        types::MarketHours::AlwaysOpen();
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
        if (on_quote) {
            on_quote();
        }
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
    void OnTimer(strategy::Context& context) override {
        timers.push_back(context.Now());
        if (on_timer) {
            on_timer();
        }
    }
    void OnStop(strategy::Context&) override { stopped = true; }

    Requirements requirements;
    std::vector<types::Timestamp> timers;
    std::function<void()> on_timer;
    bool throw_on_bar = false;
    bool throw_on_fill = false;
    int orders_on_start = 0;
    std::function<void()> on_quote;  // after a quote is recorded
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

// A market open from 00:00 to 06:00 UTC on 1970-01-01 only.
class MorningCalendar : public types::TradingCalendar {
   public:
    std::optional<types::TradingDay> On(
        std::chrono::year_month_day date) const override {
        if (date != std::chrono::year_month_day{std::chrono::sys_days{}}) {
            return std::nullopt;
        }
        return types::TradingDay{Hour(0), Hour(0), Hour(6), Hour(6)};
    }
    std::chrono::year_month_day DateAt(types::Timestamp t) const override {
        return std::chrono::year_month_day{
            std::chrono::floor<std::chrono::days>(t)};
    }
};

// Bars past max_queued_bars are dropped, and the backfill fetches them once
// the next bar shows the gap.
TEST_F(RunnerTest, ABarPastTheQueueLimitIsDroppedThenBackfilled) {
    options.max_queued_bars = 2;
    Build();
    ASSERT_OK(runner->Begin());
    for (int h = 5; h <= 8; ++h) {
        data->Close(h);  // 7 and 8 find the queue full
    }
    EXPECT_EQ(runner->DroppedBars(), 2u);
    EXPECT_TRUE(runner->Drain());
    EXPECT_EQ(strategy->bars,
              (std::vector<types::Timestamp>{Hour(5), Hour(6)}));
    data->Close(9);
    EXPECT_TRUE(runner->Drain());
    EXPECT_EQ(strategy->bars,
              (std::vector<types::Timestamp>{Hour(5), Hour(6), Hour(7), Hour(8),
                                             Hour(9)}));
    runner->End();
}

TEST_F(RunnerTest, ZeroQueuedBarsMeansNoLimit) {
    options.max_queued_bars = 0;
    Build();
    ASSERT_OK(runner->Begin());
    for (int h = 5; h <= 9; ++h) {
        data->Close(h);
    }
    EXPECT_EQ(runner->DroppedBars(), 0u);
    EXPECT_TRUE(runner->Drain());
    EXPECT_EQ(strategy->bars.size(), 5u);
    runner->End();
}

TEST_F(RunnerTest, DoesNotBackfillAGapThatIsAllClosedTime) {
    data->hours = std::make_shared<types::MarketHours>(
        std::make_shared<MorningCalendar>(), false);
    Start();
    data->Close(5);
    data->Close(8);  // hours 6 and 7: the market was closed
    Finish();
    EXPECT_EQ(strategy->bars,
              (std::vector<types::Timestamp>{Hour(5), Hour(8)}));
    EXPECT_EQ(data->get_bars_ranges.size(), 1u);  // the warmup only
}

TEST_F(RunnerTest, AnUnknownSymbolStopsTheRunBeforeItStarts) {
    data->unknown_symbol = true;
    Build();
    auto begun = runner->Begin();
    ASSERT_FALSE(begun.has_value());
    EXPECT_EQ(begun.error().category, ErrorCategory::kNotFound);
    EXPECT_NE(begun.error().message.find(kSymbol), std::string::npos);
    EXPECT_EQ(venue->cancel_all_calls, 0);  // the account was not touched
    EXPECT_TRUE(data->get_bars_ranges.empty());
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

TEST_F(RunnerTest, IgnoresAnOrderStateBehindTheOneAlreadySeen) {
    venue->pushes = true;
    strategy->orders_on_start = 1;  // "order-1", acknowledged as kNew
    Build();
    ASSERT_OK(runner->Begin());
    // Pushed: 2 filled. The poll then reads an older snapshot, 1 filled, as a
    // poll queued before the push would deliver it.
    venue->Push(trading::ExecutionUpdate{
        std::nullopt, StatusOf("order-1", types::OrderState::kPartiallyFilled, 2)});
    venue->open_orders = {
        StatusOf("order-1", types::OrderState::kPartiallyFilled, 1),
    };
    EXPECT_TRUE(runner->Drain());
    ASSERT_EQ(strategy->updates.size(), 1u);  // the push only
    EXPECT_EQ(std::get<2>(strategy->updates[0]), types::Decimal::FromInt(2));

    // A state that is ahead still gets through, and so does closing.
    venue->open_orders = {
        StatusOf("order-1", types::OrderState::kPartiallyFilled, 3),
    };
    EXPECT_TRUE(runner->Drain());
    venue->open_orders.clear();
    venue->order_states["order-1"] =
        StatusOf("order-1", types::OrderState::kCancelled, 3);
    EXPECT_TRUE(runner->Drain());
    ASSERT_EQ(strategy->updates.size(), 3u);
    EXPECT_EQ(std::get<2>(strategy->updates[1]), types::Decimal::FromInt(3));
    EXPECT_EQ(std::get<1>(strategy->updates[2]), types::OrderState::kCancelled);
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

TEST_F(RunnerTest, HandlesQuotesWhileAPollIsInFlight) {
    strategy->requirements.quotes = true;
    std::promise<void> poll_started;
    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();
    std::atomic<bool> first{true};
    venue->before_get_fills = [&] {
        if (first.exchange(false)) {
            poll_started.set_value();
            released.wait();  // a slow venue: the poll hangs here
        }
    };
    std::promise<void> quote_seen;
    strategy->on_quote = [&] { quote_seen.set_value(); };
    options.fill_poll_interval = std::chrono::milliseconds(1);
    Start();
    poll_started.get_future().wait();

    types::Quote quote;
    quote.symbol = kSymbol;
    quote.bid = types::Decimal::FromInt(100);
    data->quote_callback(quote);
    const auto seen = quote_seen.get_future().wait_for(std::chrono::seconds(5));
    release.set_value();
    Finish();
    EXPECT_EQ(seen, std::future_status::ready);  // not held behind the poll
    EXPECT_EQ(strategy->quotes.size(), 1u);
}

TEST_F(RunnerTest, StopWaitsForAPollInFlight) {
    std::promise<void> poll_started;
    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();
    std::atomic<bool> first{true};
    venue->before_get_fills = [&] {
        if (first.exchange(false)) {
            poll_started.set_value();
            released.wait();
        }
    };
    options.fill_poll_interval = std::chrono::milliseconds(1);
    Build();
    auto started = strategy->started.get_future();
    auto done = std::async(std::launch::async, [this] { return runner->Run(); });
    started.wait();
    poll_started.get_future().wait();
    runner->Stop();
    // End() joins the poller before OnStop, so Run() cannot return yet.
    EXPECT_EQ(done.wait_for(std::chrono::milliseconds(50)),
              std::future_status::timeout);
    release.set_value();
    ASSERT_OK(done.get());
    EXPECT_TRUE(strategy->stopped);
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

TEST_F(RunnerTest, DrainCallsOnTimerOnceItIsDue) {
    strategy->requirements.timer_interval = std::chrono::seconds(1);
    auto now = std::make_shared<types::Timestamp>(Hour(5));
    runner = std::make_unique<strategy::Runner>(
        data, venue, std::move(strategy_owner), options,
        [now] { return *now; });
    ASSERT_OK(runner->Begin());
    EXPECT_EQ(runner->NextTimer(), Hour(5) + std::chrono::seconds(1));
    *now += std::chrono::milliseconds(999);
    runner->Drain();
    EXPECT_TRUE(strategy->timers.empty());
    *now += std::chrono::milliseconds(1);
    runner->Drain();
    ASSERT_EQ(strategy->timers.size(), 1u);
    // A late tick is not made up: the next is an interval after it ran.
    *now += std::chrono::milliseconds(3500);
    runner->Drain();
    EXPECT_EQ(strategy->timers.size(), 2u);
    EXPECT_EQ(runner->NextTimer(), *now + std::chrono::seconds(1));
    runner->End();
}

TEST_F(RunnerTest, ATimerReArmsWhenTheClockStepsBack) {
    strategy->requirements.timer_interval = std::chrono::seconds(1);
    auto now = std::make_shared<types::Timestamp>(Hour(5));
    runner = std::make_unique<strategy::Runner>(
        data, venue, std::move(strategy_owner), options,
        [now] { return *now; });
    ASSERT_OK(runner->Begin());
    // An hour back (NTP, a resumed VM): due a second from the new time, not
    // when the clock catches up.
    *now -= std::chrono::hours(1);
    runner->Drain();
    EXPECT_EQ(runner->NextTimer(), *now + std::chrono::seconds(1));
    *now += std::chrono::seconds(1);
    runner->Drain();
    EXPECT_EQ(strategy->timers.size(), 1u);
    runner->End();
}

TEST_F(RunnerTest, NoTimerUnlessAsked) {
    Build();
    ASSERT_OK(runner->Begin());
    EXPECT_FALSE(runner->NextTimer());
    runner->End();
}

// Run() wakes for the timer with nothing else arriving, by a clock that runs.
TEST_F(RunnerTest, RunWakesForTheTimer) {
    strategy->requirements.timer_interval = std::chrono::milliseconds(20);
    options.fill_poll_interval = std::chrono::milliseconds(0);
    std::promise<void> ticked;
    strategy->on_timer = [this, &ticked] {
        if (strategy->timers.size() == 3) {
            ticked.set_value();
        }
    };
    const auto started = std::chrono::steady_clock::now();
    runner = std::make_unique<strategy::Runner>(
        data, venue, std::move(strategy_owner), options, [started] {
            return Hour(5) +
                   std::chrono::duration_cast<types::Timestamp::duration>(
                       std::chrono::steady_clock::now() - started);
        });
    thread = std::thread([this] { result = runner->Run(); });
    EXPECT_EQ(ticked.get_future().wait_for(std::chrono::seconds(5)),
              std::future_status::ready);
    Finish();
    ASSERT_GE(strategy->timers.size(), 3u);
    EXPECT_GE(strategy->timers[1] - strategy->timers[0],
              std::chrono::milliseconds(20));
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

types::OrderStatus OpenOrder(const char* id, types::Side side,
                             types::OrderType type, const char* quantity,
                             int created_hour = 0,
                             const char* symbol = kSymbol) {
    types::OrderStatus order;
    order.order_id = id;
    order.symbol = symbol;
    order.side = side;
    order.type = type;
    order.quantity = kestrel::testing::D(quantity);
    order.created_time = Hour(created_hour);
    return order;
}

types::Position PositionOf(const char* quantity) {
    types::Position position;
    position.symbol = kSymbol;
    position.quantity = kestrel::testing::D(quantity);
    return position;
}

using types::OrderType;
using types::Side;
using Ids = std::vector<std::string>;

// Long 1: the sell-stop protects it and stays; the buy-stop is an entry, and
// the target, a second full-size sell, would sell what the stop sells. Other
// symbols are not the strategy's.
TEST(EntriesToCancelTest, KeepsWhatClosesALongAndCancelsEntries) {
    const std::vector open = {
        OpenOrder("target", Side::kSell, OrderType::kLimit, "1", 0),
        OpenOrder("entry", Side::kBuy, OrderType::kStop, "1", 1),
        OpenOrder("stop", Side::kSell, OrderType::kStop, "1", 2),
        OpenOrder("other", Side::kBuy, OrderType::kLimit, "1", 0, "ETH/USD")};
    const auto cancel =
        strategy::EntriesToCancel(open, {kSymbol}, std::vector{PositionOf("1")},
                                  std::vector<types::Balance>{});
    EXPECT_EQ(cancel.ids, (Ids{"entry", "target"}));
}

TEST(EntriesToCancelTest, KeepsSellsThatFitAndCountsWhatIsLeftToFill) {
    auto partly = OpenOrder("partly", Side::kSell, OrderType::kLimit, "1", 0);
    partly.filled_quantity = kestrel::testing::D("0.6");
    const std::vector open = {
        partly, OpenOrder("half", Side::kSell, OrderType::kLimit, "0.5", 1),
        OpenOrder("late", Side::kSell, OrderType::kLimit, "0.2", 2)};
    // 0.4 left on the first and 0.5 fit within 1; 0.2 more does not.
    const auto cancel =
        strategy::EntriesToCancel(open, {kSymbol}, std::vector{PositionOf("1")},
                                  std::vector<types::Balance>{});
    EXPECT_EQ(cancel.ids, (Ids{"late"}));
}

// A spot venue reports no positions: the base asset held counts instead.
TEST(EntriesToCancelTest, UsesTheBaseAssetWithoutAPosition) {
    const std::vector open = {
        OpenOrder("stop", Side::kSell, OrderType::kStop, "0.5"),
        OpenOrder("entry", Side::kBuy, OrderType::kLimit, "0.5")};
    types::Balance btc;
    btc.asset = "BTC";
    btc.total = kestrel::testing::D("0.5");
    const auto cancel = strategy::EntriesToCancel(
        open, {kSymbol}, std::vector<types::Position>{}, std::vector{btc});
    EXPECT_EQ(cancel.ids, (Ids{"entry"}));
}

TEST(EntriesToCancelTest, CancelsEverythingWhenFlat) {
    const std::vector open = {
        OpenOrder("sell", Side::kSell, OrderType::kStop, "1"),
        OpenOrder("buy", Side::kBuy, OrderType::kStop, "1")};
    const auto cancel =
        strategy::EntriesToCancel(open, {kSymbol}, std::vector{PositionOf("0")},
                                  std::vector<types::Balance>{});
    EXPECT_EQ(cancel.ids, (Ids{"sell", "buy"}));
}

TEST(EntriesToCancelTest, KeepsTheBuyStopOfAShort) {
    const std::vector open = {
        OpenOrder("cover", Side::kBuy, OrderType::kStop, "1"),
        OpenOrder("more", Side::kSell, OrderType::kStop, "1")};
    const auto cancel = strategy::EntriesToCancel(
        open, {kSymbol}, std::vector{PositionOf("-1")},
        std::vector<types::Balance>{});
    EXPECT_EQ(cancel.ids, (Ids{"more"}));
}

// Holdings that could not be read: as on a spot account, sells close and
// buys open.
TEST(EntriesToCancelTest, WithoutHoldingsKeepsSellsAndCancelsBuys) {
    const std::vector open = {
        OpenOrder("sell", Side::kSell, OrderType::kStop, "5"),
        OpenOrder("buy", Side::kBuy, OrderType::kStop, "1")};
    const auto cancel =
        strategy::EntriesToCancel(open, {kSymbol}, std::nullopt, std::nullopt);
    EXPECT_EQ(cancel.ids, (Ids{"buy"}));
}

TEST_F(RunnerTest, CancelsEntriesButNotProtectiveStopsOnStop) {
    options.cancel_entries_on_stop = true;
    Build();
    ASSERT_OK(runner->Begin());
    venue->open_orders = {
        OpenOrder("stop", Side::kSell, OrderType::kStop, "1"),
        OpenOrder("entry", Side::kBuy, OrderType::kStop, "1")};
    venue->positions = {PositionOf("1")};
    runner->End();
    EXPECT_EQ(venue->cancels, (Ids{"entry"}));
    EXPECT_TRUE(strategy->stopped);
}

// An entry that fills as it is cancelled reaches OnFill before the strategy
// stops (where it would place its protective stop), even without a trade log.
TEST_F(RunnerTest, AnEntryThatFillsAsItIsCancelledStillReachesOnFill) {
    options.cancel_entries_on_stop = true;
    Build();
    ASSERT_OK(runner->Begin());
    venue->open_orders = {
        OpenOrder("entry", Side::kBuy, OrderType::kStop, "1")};
    venue->cancel_result = trading::CancelOutcome::kAlreadyFilled;
    venue->fills = {FillAt("f1", 6)};
    runner->End();
    EXPECT_EQ(venue->cancels, (Ids{"entry"}));
    EXPECT_EQ(strategy->fills, (Ids{"f1"}));
    EXPECT_TRUE(strategy->stopped);
}

TEST(EntriesToCancelTest, ReportsAHoldingLeftWithoutAClosingOrder) {
    const std::vector open = {
        OpenOrder("entry", Side::kBuy, OrderType::kStop, "1")};
    const auto cancel = strategy::EntriesToCancel(
        open, {kSymbol, kSymbol}, std::vector{PositionOf("1")},
        std::vector<types::Balance>{});
    EXPECT_EQ(cancel.ids, (Ids{"entry"}));  // once, though listed twice
    EXPECT_EQ(cancel.unprotected, std::vector<types::Symbol>{kSymbol});
}

TEST_F(RunnerTest, LeavesOrdersOpenOnStopUnlessAsked) {
    Build();
    ASSERT_OK(runner->Begin());
    venue->open_orders = {
        OpenOrder("entry", Side::kBuy, OrderType::kStop, "1")};
    runner->End();
    EXPECT_TRUE(venue->cancels.empty());
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

// A market open from 01:00 to 06:00 UTC every day.
class DailyCalendar : public types::TradingCalendar {
   public:
    std::optional<types::TradingDay> On(
        std::chrono::year_month_day date) const override {
        const types::Timestamp day{std::chrono::sys_days{date}};
        return types::TradingDay{
            day + std::chrono::hours(1), day + std::chrono::hours(1),
            day + std::chrono::hours(6), day + std::chrono::hours(6)};
    }
    std::chrono::year_month_day DateAt(types::Timestamp t) const override {
        return std::chrono::year_month_day{
            std::chrono::floor<std::chrono::days>(t)};
    }
};

// Records the session hooks and what the context said at each.
class SessionStrategy : public strategy::Strategy {
   public:
    std::string_view Name() const override { return "session"; }
    Requirements Needs() const override {
        Requirements r;
        r.symbols = {kSymbol};
        r.quotes = true;
        return r;
    }
    void OnStart(strategy::Context& context) override {
        if (on_start) {
            on_start();
        }
        hooks.push_back("start");
        open_at_start = context.MarketOpen();
        session_at_start = context.CurrentOrNextSession();
        order_at_start = Place(context);
    }
    void OnSessionOpen(strategy::Context& context) override {
        hooks.push_back("open");
        times.push_back(context.Now());
        order_at_open = Place(context);
        if (on_open) {
            on_open();
        }
    }
    void OnSessionClosing(strategy::Context& context,
                          types::Timestamp close) override {
        hooks.push_back("closing");
        times.push_back(context.Now());
        closes.push_back(close);
    }

    std::function<void()> on_open;   // after an open is recorded
    std::function<void()> on_start;  // first thing in OnStart
    std::vector<std::string> hooks;
    std::vector<types::Timestamp> times;
    std::vector<types::Timestamp> closes;
    bool open_at_start = false;
    std::optional<types::MarketHours::Session> session_at_start;
    types::Result<types::OrderAcknowledgement> order_at_start =
        types::MakeError(ErrorCategory::kInternal, "not placed");
    types::Result<types::OrderAcknowledgement> order_at_open =
        types::MakeError(ErrorCategory::kInternal, "not placed");

   private:
    static types::Result<types::OrderAcknowledgement> Place(
        strategy::Context& context) {
        types::OrderRequest request;
        request.symbol = kSymbol;
        request.type = types::OrderType::kLimit;
        request.quantity = types::Decimal::FromInt(1);
        request.limit_price = types::Decimal::FromInt(10);
        return context.Venue().PlaceOrder(request);
    }
};

class RunnerSessionTest : public ::testing::Test {
   protected:
    RunnerSessionTest() {
        data->hours = std::make_shared<types::MarketHours>(
            std::make_shared<DailyCalendar>(), false);
        auto owner = std::make_unique<SessionStrategy>();
        strategy = owner.get();
        strategy_owner = std::move(owner);
    }
    void Build() {
        options.fill_poll_interval = std::chrono::milliseconds(0);
        runner = std::make_unique<strategy::Runner>(
            data, venue, std::move(strategy_owner), options,
            [this] { return now; });
    }
    // Moves the clock to the next session event and drains.
    types::Timestamp Advance() {
        const auto due = runner->NextSessionEvent();
        EXPECT_TRUE(due.has_value());
        now = *due;
        runner->Drain();
        return now;
    }

    std::shared_ptr<FakeDataSource> data = std::make_shared<FakeDataSource>();
    std::shared_ptr<RecordingVenue> venue = std::make_shared<RecordingVenue>();
    std::unique_ptr<strategy::Strategy> strategy_owner;
    SessionStrategy* strategy = nullptr;
    strategy::RunnerOptions options;
    std::unique_ptr<strategy::Runner> runner;
    types::Timestamp now = Hour(0);  // before the day's 01:00 open
};

TEST_F(RunnerSessionTest,
       HooksAtTheOpenAndBeforeTheCloseAndOrdersOnlyWhenOpen) {
    Build();
    ASSERT_OK(runner->Begin());
    EXPECT_FALSE(strategy->open_at_start);
    ASSERT_TRUE(strategy->session_at_start);
    EXPECT_EQ(strategy->session_at_start->start, Hour(1));
    EXPECT_EQ(strategy->session_at_start->end, Hour(6));
    // Closed: refused before the venue.
    ASSERT_FALSE(strategy->order_at_start);
    EXPECT_EQ(strategy->order_at_start.error().category,
              ErrorCategory::kRejected);
    EXPECT_NE(strategy->order_at_start.error().message.find("market closed"),
              std::string::npos);
    EXPECT_EQ(venue->attempts, 0);

    EXPECT_EQ(Advance(), Hour(1));  // the open
    EXPECT_TRUE(strategy->order_at_open.has_value());
    EXPECT_EQ(venue->attempts, 1);
    EXPECT_EQ(Advance(), Hour(6) - std::chrono::minutes(5));  // closing
    EXPECT_EQ(Advance(), Hour(6));  // over: the next day's is found
    EXPECT_EQ(Advance(), Hour(25));
    EXPECT_EQ(strategy->hooks,
              (std::vector<std::string>{"start", "open", "closing", "open"}));
    EXPECT_EQ(strategy->closes, std::vector<types::Timestamp>{Hour(6)});
    // Cancels pass while closed.
    now = Hour(7);
    EXPECT_TRUE(venue->CancelAll().has_value());
    runner->End();
}

TEST_F(RunnerSessionTest, StartingInASessionCallsNoOpenButTheClosingIfDue) {
    now = Hour(3);  // mid-session: no OnSessionOpen for it
    Build();
    ASSERT_OK(runner->Begin());
    EXPECT_TRUE(strategy->open_at_start);
    EXPECT_TRUE(strategy->order_at_start.has_value());
    EXPECT_EQ(Advance(), Hour(6) - std::chrono::minutes(5));
    EXPECT_EQ(strategy->hooks, (std::vector<std::string>{"start", "closing"}));
    runner->End();

    // Started inside the closing window: OnSessionClosing right after
    // OnStart.
    auto owner = std::make_unique<SessionStrategy>();
    strategy = owner.get();
    strategy_owner = std::move(owner);
    now = Hour(6) - std::chrono::minutes(2);
    Build();
    ASSERT_OK(runner->Begin());
    EXPECT_EQ(strategy->hooks, (std::vector<std::string>{"start", "closing"}));
    runner->End();
}

TEST_F(RunnerSessionTest, AZeroLeadStillCallsTheClosingJustBeforeTheClose) {
    options.session_closing_lead = std::chrono::milliseconds(0);
    Build();
    ASSERT_OK(runner->Begin());
    EXPECT_EQ(Advance(), Hour(1));
    EXPECT_EQ(Advance(), Hour(6) - std::chrono::milliseconds(1));
    EXPECT_EQ(strategy->hooks,
              (std::vector<std::string>{"start", "open", "closing"}));
    runner->End();
}

// Run() wakes for a session event by itself, with no other event to wake it:
// a session opens 200 ms after the start, by a clock that runs.
TEST_F(RunnerSessionTest, RunWakesForTheNextSessionHook) {
    const auto started = std::chrono::steady_clock::now();
    const types::Timestamp base = Hour(0);
    class SoonCalendar : public types::TradingCalendar {
       public:
        explicit SoonCalendar(types::Timestamp open) : mOpen(open) {}
        std::optional<types::TradingDay> On(
            std::chrono::year_month_day date) const override {
            if (date != DateAt(mOpen)) {
                return std::nullopt;
            }
            return types::TradingDay{mOpen, mOpen,
                                     mOpen + std::chrono::hours(1),
                                     mOpen + std::chrono::hours(1)};
        }
        std::chrono::year_month_day DateAt(types::Timestamp t) const override {
            return std::chrono::year_month_day{
                std::chrono::floor<std::chrono::days>(t)};
        }

       private:
        types::Timestamp mOpen;
    };
    data->hours = std::make_shared<types::MarketHours>(
        std::make_shared<SoonCalendar>(base + std::chrono::milliseconds(200)),
        false);
    options.fill_poll_interval = std::chrono::milliseconds(0);
    std::promise<void> opened;
    auto owner = std::make_unique<SessionStrategy>();
    strategy = owner.get();
    strategy->on_open = [&opened] { opened.set_value(); };
    runner = std::make_unique<strategy::Runner>(
        data, venue, std::move(owner), options, [base, started] {
            return base +
                   std::chrono::duration_cast<types::Timestamp::duration>(
                       std::chrono::steady_clock::now() - started);
        });
    std::thread thread([this] { (void)runner->Run(); });
    const bool woke = opened.get_future().wait_for(std::chrono::seconds(5)) ==
                      std::future_status::ready;
    runner->Stop();
    thread.join();
    EXPECT_TRUE(woke);
    EXPECT_EQ(strategy->hooks, (std::vector<std::string>{"start", "open"}));
}

// Like DailyCalendar, but day 0's hours can change while the runner runs, as
// when Schwab's answer lands after the rules': open later, close earlier, or
// not trade at all.
class ChangingCalendar : public DailyCalendar {
   public:
    std::optional<types::TradingDay> On(
        std::chrono::year_month_day date) const override {
        auto day = DailyCalendar::On(date);
        if (date == std::chrono::year_month_day{std::chrono::sys_days{}}) {
            if (closed) {
                return std::nullopt;
            }
            day->open = day->pre_open = open;
            day->close = day->post_close = close;
        }
        return day;
    }
    types::Timestamp open = Hour(1);
    types::Timestamp close = Hour(6);
    bool closed = false;
};

TEST_F(RunnerSessionTest, ARestrictionLearnedAfterStartingMovesTheHooks) {
    auto calendar = std::make_shared<ChangingCalendar>();
    data->hours = std::make_shared<types::MarketHours>(calendar, false);
    Build();
    ASSERT_OK(runner->Begin());
    EXPECT_EQ(runner->NextSessionEvent(), Hour(1));
    // A delayed open, learned before it: the open hook moves to it.
    calendar->open = Hour(2);
    runner->Drain();
    EXPECT_EQ(Advance(), Hour(2));
    // An early close learned in the session: the closing hook comes before
    // it, not before the old close.
    calendar->close = Hour(3);
    now = Hour(2) + std::chrono::minutes(30);
    runner->Drain();
    EXPECT_EQ(Advance(), Hour(3) - std::chrono::minutes(5));
    EXPECT_EQ(strategy->closes, std::vector<types::Timestamp>{Hour(3)});
    runner->End();

    // A day that turns out closed: no hooks for it, the next day's instead.
    auto owner = std::make_unique<SessionStrategy>();
    strategy = owner.get();
    strategy_owner = std::move(owner);
    calendar->open = Hour(1);
    calendar->close = Hour(6);
    now = Hour(0);
    Build();
    ASSERT_OK(runner->Begin());
    calendar->closed = true;
    runner->Drain();
    EXPECT_EQ(runner->NextSessionEvent(), Hour(25));
    EXPECT_EQ(strategy->hooks, std::vector<std::string>{"start"});
    runner->End();
}

// Collects what is logged while it lives.
class LogCapture {
   public:
    LogCapture()
        : mSink(std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(256)) {
        spdlog::default_logger()->sinks().push_back(mSink);
    }
    ~LogCapture() {
        auto& sinks = spdlog::default_logger()->sinks();
        sinks.erase(std::remove(sinks.begin(), sinks.end(), mSink),
                    sinks.end());
    }
    bool Saw(const std::string& text) const {
        for (const auto& line : mSink->last_formatted()) {
            if (line.find(text) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

   private:
    std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> mSink;
};

// Run() logs a quote feed gone quiet while the market is open, counting
// only open time, and logs it again when quotes come back.
TEST_F(RunnerSessionTest, AQuietQuoteFeedIsStaleOnlyWhileTheMarketIsOpen) {
    LogCapture log;
    std::atomic<std::int64_t> clock_ms{0};  // 00:00, before the 01:00 open
    const auto set = [&](types::Timestamp t) {
        clock_ms = types::ToEpochMillis(t);
    };
    options.fill_poll_interval = std::chrono::milliseconds(10);
    options.stale_quote_warning = std::chrono::minutes(1);
    std::promise<void> started;
    strategy->on_start = [&started] { started.set_value(); };
    runner = std::make_unique<strategy::Runner>(
        data, venue, std::move(strategy_owner), options,
        [&clock_ms] { return types::FromEpochMillis(clock_ms.load()); });
    std::thread thread([this] { (void)runner->Run(); });
    started.get_future().wait();
    // The callback is set once OnStart has run.
    const auto settle = [] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    };

    set(Hour(0) + std::chrono::minutes(50));  // closed: not stale
    settle();
    set(Hour(1) + std::chrono::seconds(30));  // 30 s into the session
    settle();
    EXPECT_FALSE(log.Saw("no quote for"));
    set(Hour(1) + std::chrono::minutes(2));
    settle();
    EXPECT_TRUE(log.Saw("no quote for BTC/USD for 120 s of open market"));

    types::Quote quote;
    quote.symbol = kSymbol;
    data->quote_callback(quote);
    settle();
    EXPECT_TRUE(log.Saw("quotes for BTC/USD again, after 120 s of open"));
    runner->Stop();
    thread.join();
}

// With max_quote_age, the gate refuses entries in a symbol whose quotes
// have stopped, until they come back; the warning can be off.
TEST_F(RunnerSessionTest, StaleQuotesPauseEntriesThroughTheRiskGate) {
    LogCapture log;
    std::atomic<std::int64_t> clock_ms{types::ToEpochMillis(Hour(1))};
    const auto set = [&](types::Timestamp t) {
        clock_ms = types::ToEpochMillis(t);
    };
    options.fill_poll_interval = std::chrono::milliseconds(10);
    options.stale_quote_warning = std::chrono::milliseconds::zero();
    options.risk = trading::RiskLimits{};
    options.risk->max_quote_age = std::chrono::minutes(1);
    std::promise<void> started;
    strategy->on_start = [&started] { started.set_value(); };
    runner = std::make_unique<strategy::Runner>(
        data, venue, std::move(strategy_owner), options,
        [&clock_ms] { return types::FromEpochMillis(clock_ms.load()); });
    std::thread thread([this] { (void)runner->Run(); });
    started.get_future().wait();
    const auto settle = [] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    };
    types::OrderRequest buy;
    buy.symbol = kSymbol;
    buy.side = types::Side::kBuy;
    buy.type = types::OrderType::kMarket;
    buy.quantity = kestrel::testing::D("1");

    set(Hour(1) + std::chrono::seconds(70));
    settle();
    EXPECT_TRUE(log.Saw("risk gate refuses new positions in it"));
    EXPECT_FALSE(runner->RiskGate()->PlaceOrder(buy));

    types::Quote quote;
    quote.symbol = kSymbol;
    data->quote_callback(quote);
    settle();
    EXPECT_TRUE(log.Saw("quotes for BTC/USD again; entries allowed"));
    EXPECT_TRUE(runner->RiskGate()->PlaceOrder(buy));
    runner->Stop();
    thread.join();
}

TEST_F(RunnerSessionTest, ASessionSleptThroughGetsNoHooks) {
    Build();
    ASSERT_OK(runner->Begin());
    now = Hour(27);  // the next day's session, mid-way
    runner->Drain();
    // Day 0 is over without hooks; day 1's open is called on reaching it.
    EXPECT_EQ(strategy->hooks, (std::vector<std::string>{"start", "open"}));
    EXPECT_EQ(strategy->times, std::vector<types::Timestamp>{Hour(27)});
    runner->End();
}

TEST_F(RunnerSessionTest, AMarketThatNeverClosesHasNoSessions) {
    data->hours = types::MarketHours::AlwaysOpen();
    Build();
    ASSERT_OK(runner->Begin());
    EXPECT_TRUE(strategy->open_at_start);
    EXPECT_FALSE(strategy->session_at_start);
    EXPECT_TRUE(strategy->order_at_start.has_value());
    EXPECT_FALSE(runner->NextSessionEvent());
    runner->End();
}

}  // namespace
