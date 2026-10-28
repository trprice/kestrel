#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include "kestrel/data/data_source.hpp"
#include "kestrel/strategy/strategy.hpp"
#include "kestrel/trading/risk_gate.hpp"
#include "kestrel/trading/trade_log.hpp"
#include "kestrel/trading/trading_venue.hpp"

namespace kestrel::strategy {

class OrderTracker;
class RunRecorder;

struct RunnerOptions {
    // When set, the strategy gets a RiskGatedVenue with these limits instead
    // of the venue itself, fed with prices from the bars and quotes.
    std::optional<trading::RiskLimits> risk;
    // How often Run() asks the venue for new fills and for changes to the
    // strategy's orders; zero turns polling off.
    std::chrono::milliseconds fill_poll_interval{1000};
    // The poll period instead, when the venue pushes executions
    // (TradingVenue::StreamExecutions): the poll is then only a backstop for
    // updates the push missed. Zero turns it off, which is safe only for a
    // venue that cannot miss an update (the in-process paper venue): with a
    // stream that drops messages while it reconnects, nothing would catch the
    // fills and order changes it missed until End(), and that final poll only
    // runs with a trade_log.
    std::chrono::milliseconds fill_backstop_interval{30'000};
    // When set, every fill and a P&L snapshot per symbol every pnl_interval
    // (zero: only at the end) go to this log; see RunRecorder. Run() only:
    // Drain-driven runs (backtests) get the fills but no periodic snapshots.
    std::shared_ptr<trading::TradeLog> trade_log;
    std::chrono::milliseconds pnl_interval{60'000};
    // With a trade_log, the venue's response times are stored in windows of
    // this length (zero: not at all); see RunRecorder::RecordLatency.
    std::chrono::milliseconds latency_interval{0};
    // Where the figures come from; null means the HTTP statistics.
    std::function<std::vector<types::EndpointLatency>()> latency_source;
};

// Drives one strategy: subscribes to what it asks for, then feeds the events
// to it one at a time on the thread that called Run().
//
// The data source delivers on its WebSocket thread, where a strategy must not
// block (it would stall the stream) and where Subscription::Stop() cannot be
// called. The callbacks therefore only enqueue; the strategy runs on the
// Run() thread, which is also free to call the venue over HTTP.
//
// Orders placed through the strategy's venue are remembered while they are
// active. The same poll checks them (GetOpenOrders, plus GetOrder for one that
// has left the open list) and delivers each change of state or filled quantity
// to OnOrderUpdate, ending with the closed state. No orders, no calls.
//
// Fills are delivered to OnFill once each. When the venue pushes executions
// (TradingVenue::StreamExecutions), its fills and order changes are queued as
// events like quotes and bars, so the strategy hears of a fill as soon as the
// venue reports it, and polling drops to fill_backstop_interval to catch what
// the push missed. Otherwise fills are found by polling TradingVenue::GetFills
// every fill_poll_interval. The poll starts from the time Begin() ran, and it
// assumes the venue's fill times and the runner's clock roughly agree; a fill
// is recognised as a duplicate by its ID for kSeenFillHorizon behind the
// newest fill seen.
//
// With options.trade_log the runner also records the fills it sees (as it
// polls them) and periodic P&L snapshots, valued at the last bar close or
// quote mid seen; the last poll before OnStop picks up late fills.
//
// Quotes are conflated per symbol: while one is queued for a symbol, newer
// quotes for it are merged into it (Quote::MergeFrom) instead of queueing
// behind it, so the strategy never sees a quote older than the last one that
// arrived, and a slow strategy cannot fall behind on quotes. Intermediate
// quotes are skipped. Bars are never dropped, so the queue still grows if bars
// outpace the strategy.
class Runner : private Context {
   public:
    // How far behind the newest fill seen the IDs of older ones are kept, to
    // drop a fill that arrives both pushed and polled. A pushed fill older
    // than that is dropped as stale (and logged).
    static constexpr std::chrono::minutes kSeenFillHorizon{10};

    Runner(std::shared_ptr<data::DataSource> data_source,
           std::shared_ptr<trading::TradingVenue> venue,
           std::unique_ptr<Strategy> strategy, RunnerOptions options = {},
           std::function<types::Timestamp()> clock = types::Now);
    ~Runner();

    // Begin(), then processes events until Stop(), then End(). Blocks.
    // Returns an error only if Begin() fails; once the loop is running,
    // problems are logged and the loop carries on. Call once.
    types::Result<void> Run();

    // Thread-safe, but not async-signal-safe (it takes a mutex): call it from
    // a watch thread, not a signal handler. Events already queued are
    // processed first. May be called before Run().
    void Stop();

    // Quotes merged into an already queued one instead of being delivered
    // separately. Thread-safe.
    std::size_t ConflatedQuotes() const;

    // The risk gate the strategy trades through, if options.risk was set; use
    // it to halt trading from outside the strategy.
    std::shared_ptr<trading::RiskGatedVenue> RiskGate() const { return mGate; }

    // Run() in pieces, for a driver that supplies events itself on one thread
    // and wants deterministic processing (see backtest::RunBacktest).
    //
    // Begin() subscribes, loads the warmup bars and calls OnStart; it fails
    // like Run() does. Drain() processes everything queued, then polls for
    // fills (always, whatever fill_poll_interval says) and repeats while that
    // finds new ones; it returns false if Stop() was called. End() closes the
    // streams and calls OnStop; it does nothing unless Begin() succeeded.
    types::Result<void> Begin();
    bool Drain();
    void End();

   private:
    struct BarEvent {
        types::Symbol symbol;
        types::Timeframe timeframe;
        types::Bar bar;
    };
    // Names the symbol whose pending quote (mPendingQuotes) is next; the
    // quote itself is filled in when the event is popped.
    struct QuoteEvent {
        types::Symbol symbol;
        types::Quote quote;
    };
    struct ExecutionEvent {
        trading::ExecutionUpdate update;
    };
    struct StopEvent {};
    using Event = std::variant<BarEvent, QuoteEvent, ExecutionEvent, StopEvent>;
    using Series = std::pair<types::Symbol, types::Timeframe>;
    using SteadyTime = std::chrono::steady_clock::time_point;

    void Push(Event event);
    void PushQuote(const types::Quote& quote);
    // Both require mMutex and a non-empty queue.
    Event TakeFront();
    // Waits for an event until `deadline` (forever if unset).
    std::optional<Event> Pop(std::optional<SteadyTime> deadline);
    std::optional<Event> TryPop();
    // Returns false for a StopEvent.
    bool Handle(const Event& event);

    types::Result<void> Warmup(const Requirements& requirements);
    void HandleBar(const BarEvent& event);
    // Records the bar in its window and hands it to the strategy; ignores
    // bars at or before the newest one already seen (reconnect duplicates).
    void DeliverBar(const types::Symbol& symbol, types::Timeframe timeframe,
                    const types::Bar& bar);
    // Hands the strategy the fills it has not seen; returns how many.
    std::size_t PollFills();
    // Hands one fill to the strategy (and the recorder) unless it was seen
    // before; returns whether it did.
    bool DeliverFill(const types::Fill& fill);
    // Hands a pushed fill and order change to the strategy.
    void HandleExecution(const trading::ExecutionUpdate& update);
    // Subscribes to the venue's executions, if it pushes them.
    void SubscribeExecutions();
    // Hands the strategy the order changes it has not seen; returns how many.
    std::size_t PollOrders();
    // PollFills, then PollOrders; returns how much they delivered.
    std::size_t Poll();
    // Runs a strategy hook, logging instead of propagating an exception.
    void Guarded(const char* hook, const std::function<void()>& call);

    // Context
    types::Timestamp Now() const override { return mClock(); }
    trading::TradingVenue& Venue() override { return *mVenue; }
    types::Result<types::SymbolInfo> SymbolInfo(
        const types::Symbol& symbol) override {
        return mData->GetSymbolInfo(symbol);
    }
    std::span<const types::Bar> Bars(const types::Symbol& symbol,
                                     types::Timeframe timeframe) const override;

    std::shared_ptr<data::DataSource> mData;
    std::shared_ptr<trading::RiskGatedVenue> mGate;  // null without risk
    std::shared_ptr<OrderTracker> mTracker;
    std::unique_ptr<RunRecorder> mRecorder;  // null without options.trade_log
    // What the strategy trades through: the tracker, over the gate if there is
    // one, over the venue.
    std::shared_ptr<trading::TradingVenue> mVenue;
    std::unique_ptr<Strategy> mStrategy;
    const RunnerOptions mOptions;
    std::function<types::Timestamp()> mClock;

    mutable std::mutex mMutex;  // guards the members below
    std::condition_variable mWake;
    std::deque<Event> mEvents;
    // Quotes waiting in mEvents, by symbol: at most one QuoteEvent per symbol
    // is queued, and it takes the quote from here when popped.
    std::map<types::Symbol, types::Quote> mPendingQuotes;
    std::size_t mConflated = 0;

    // Touched only on the thread driving the strategy.
    bool mBegun = false;
    bool mPushed = false;  // the venue pushes executions
    std::map<Series, std::vector<types::Bar>> mBars;
    std::size_t mWindowSize = 1;
    types::Timestamp mRunStart;    // when Begin() ran; no fill before it counts
    types::Timestamp mFillCursor;  // the newest fill time seen
    // Fill IDs seen, by fill time, kept for kSeenFillHorizon behind the cursor.
    std::map<std::string, types::Timestamp> mSeenFills;

    // Last: destroyed first, so no callback outlives the members above.
    std::vector<std::unique_ptr<data::Subscription>> mSubscriptions;
};

}  // namespace kestrel::strategy
