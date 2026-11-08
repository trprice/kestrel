#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
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
    // How long before a session closes Strategy::OnSessionClosing is called.
    // Must be positive: at the close itself orders are already refused, so a
    // lead under 1 ms is taken as 1 ms.
    std::chrono::milliseconds session_closing_lead{std::chrono::minutes(5)};
    // Run() only, for a strategy that takes quotes: a symbol with no quote
    // for this long while the market is open is logged as stale (once, and
    // again when quotes come back). Closed time does not count, so a quiet
    // night or weekend is not stale. Zero turns it off.
    std::chrono::milliseconds stale_quote_warning{std::chrono::minutes(2)};
    // End() cancels the orders open on the strategy's symbols that would open
    // or add to a position (entries), and leaves those that close it (a
    // protective stop), so a position stays protected while nothing runs. It
    // then polls fills before OnStop, so an entry that filled first reaches
    // OnFill (where a strategy places its stop). See EntriesToCancel. `run`
    // sets it; a backtest does not.
    bool cancel_entries_on_stop = false;
    // At most this many bars wait in the queue; zero for no limit. A bar
    // arriving while it is full is dropped (and logged), and the backfill
    // fetches it from the data source once the strategy catches up and the
    // next bar of its series shows the gap, so bars stay in order and are
    // not lost while the source can serve them. Until that next bar comes (a
    // quiet market, the next session) the newest bar dropped is not seen,
    // nor its price by the risk gate; one dropped just before a stop is
    // lost, as is one the backfill cannot fetch. A backtest sets zero.
    std::size_t max_queued_bars = 1000;
};

// Which of `open` (the orders open on the venue) are entries, to cancel when a
// run stops: those on `symbols` that do not reduce the position. Held is the
// symbol's entry in `positions`, or else the base asset's total in
// `balances` (a spot venue reports no positions); nullopt for either means it
// could not be read, and then every sell is taken to reduce and every buy is
// an entry (a spot account cannot be short). Orders that reduce it (sells
// when long, buys when short) are kept while their remaining quantities fit
// within it, stops first, then oldest first; the rest are entries too.
// `unprotected` lists the symbols held with no order kept to close them.
struct EntryCancels {
    std::vector<std::string> ids;
    std::vector<types::Symbol> unprotected;
};
EntryCancels EntriesToCancel(
    const std::vector<types::OrderStatus>& open,
    const std::vector<types::Symbol>& symbols,
    const std::optional<std::vector<types::Position>>& positions,
    const std::optional<std::vector<types::Balance>>& balances);

// Drives one strategy: subscribes to what it asks for, then feeds the events
// to it one at a time on the thread that called Run().
//
// The data source delivers on its WebSocket thread, where a strategy must not
// block (it would stall the stream) and where Subscription::Stop() cannot be
// called. The callbacks therefore only enqueue; the strategy runs on the
// Run() thread, which is also free to call the venue over HTTP.
//
// Run() polls the venue on a poller thread of its own, so the strategy never
// waits for a poll's HTTP round trips (two of them, about 0.13 s each on
// Binance.US over an open connection): the poller
// queues what it finds as an event, and the fills and order changes are
// delivered on the Run() thread like everything else. A poll can therefore
// land between any two hooks, and a fill can be queued while the strategy is
// cancelling the order it belongs to. Drain() polls on the calling thread
// instead, so a backtest stays deterministic.
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
// quotes are skipped. Queued bars are bounded by options.max_queued_bars: one
// arriving past it is dropped, and recovered by the gap backfill.
//
// On a market that closes (the data source's Hours), the strategy trades
// through a trading::SessionGatedVenue, which refuses orders while it is
// closed; Context::MarketOpen and CurrentOrNextSession say when it trades;
// and OnSessionOpen and OnSessionClosing are called as sessions open and
// near their close (Run() wakes for them; a Drain-driven run gets them when
// the driver advances its clock to NextSessionEvent()). The gap backfill
// skips a gap that is all closed time.
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
    ~Runner() override;

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
    // Bars dropped because the queue was full (options.max_queued_bars).
    // Thread-safe.
    std::size_t DroppedBars() const;

    // The risk gate the strategy trades through, if options.risk was set; use
    // it to halt trading from outside the strategy.
    std::shared_ptr<trading::RiskGatedVenue> RiskGate() const { return mGate; }

    // Run() in pieces, for a driver that supplies events itself on one thread
    // and wants deterministic processing (see backtest::RunBacktest).
    //
    // Begin() cancels every order open on the venue's symbols, subscribes,
    // loads the warmup bars and calls OnStart; it fails like Run() does, and
    // fails without starting if the cancel fails. The runner assumes it is in
    // sole control of the account's orders while it runs: an order it did not
    // place holds funds and can fill without the strategy or the risk gate
    // counting it. Drain() processes everything queued, then polls for
    // fills (always, whatever fill_poll_interval says) and repeats while that
    // finds new ones; it returns false if Stop() was called. End() closes the
    // streams and calls OnStop; it does nothing unless Begin() succeeded.
    types::Result<void> Begin();
    bool Drain();
    void End();
    // When the next session hook is due (or the session ends), for a driver
    // that advances a clock: set the clock to it and Drain(). Nullopt on a
    // market that never closes, or before Begin().
    std::optional<types::Timestamp> NextSessionEvent() const;
    // When OnTimer is next due (Requirements::timer_interval), likewise for a
    // driver: Drain() calls it once the clock has reached it. Nullopt
    // without a timer, or before Begin(). The timer ignores market sessions:
    // it ticks while the market is closed too. For the driving thread only.
    std::optional<types::Timestamp> NextTimer() const { return mNextTimer; }

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
    // What one poll found: fills (oldest first) and changed order states.
    struct PolledEvent {
        std::vector<types::Fill> fills;
        std::vector<types::OrderStatus> orders;
    };
    struct StopEvent {};
    using Event = std::variant<BarEvent, QuoteEvent, ExecutionEvent,
                               PolledEvent, StopEvent>;
    using Series = std::pair<types::Symbol, types::Timeframe>;
    using SteadyTime = std::chrono::steady_clock::time_point;

    void Push(Event event);
    void PushQuote(const types::Quote& quote);
    // A bar, unless options.max_queued_bars are queued already.
    void PushBar(BarEvent event);
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
    // The poll's venue calls. Thread-safe, so the poller thread can make
    // them: they read the published fill cursor and the order tracker, and
    // touch nothing else of the runner's.
    std::vector<types::Fill> FetchFills();
    std::vector<types::OrderStatus> FetchOrders();
    // Hands the strategy what a poll found that it has not seen; returns how
    // much. Run() thread only.
    std::size_t DeliverPolled(const PolledEvent& polled);
    // Fetches and delivers the fills on the calling thread; returns how many.
    std::size_t PollFills();
    // Hands one fill to the strategy (and the recorder) unless it was seen
    // before; returns whether it did.
    bool DeliverFill(const types::Fill& fill);
    // Hands a pushed fill and order change to the strategy.
    void HandleExecution(const trading::ExecutionUpdate& update);
    // Subscribes to the venue's executions, if it pushes them.
    void SubscribeExecutions();
    // PollFills, then the order changes, on the calling thread; returns how
    // much they delivered.
    std::size_t Poll();
    // The poller thread: every `interval`, fetches and queues a PolledEvent
    // if it found anything. StopPoller() ends and joins it; it is safe to call
    // when no poller runs.
    void StartPoller(std::chrono::milliseconds interval);
    void StopPoller();
    // End(), with options.cancel_entries_on_stop.
    void CancelEntries();
    // Runs a strategy hook, logging instead of propagating an exception.
    void Guarded(const char* hook, const std::function<void()>& call);
    // Calls the session hooks that are due at `now`, moving to the next
    // session once one has ended. StartSessions does the first check.
    void StartSessions(types::Timestamp now);
    void CheckSession(types::Timestamp now);
    // Calls OnTimer if it is due.
    void CheckTimer(types::Timestamp now);
    // Adopts a restriction the calendar learned of since mSession was read.
    void RefreshSession(types::Timestamp now);
    // Logs the quote streams that have gone quiet while the market is open
    // (options.stale_quote_warning). Run() thread only.
    void CheckQuoteFeeds(types::Timestamp now);
    // A quote arrived for `symbol` at `now`.
    void NoteQuote(const types::Symbol& symbol, types::Timestamp now);
    // options.session_closing_lead, at least 1 ms.
    std::chrono::milliseconds Lead() const {
        return std::max(mOptions.session_closing_lead,
                        std::chrono::milliseconds(1));
    }

    // Context
    types::Timestamp Now() const override { return mClock(); }
    trading::TradingVenue& Venue() override { return *mVenue; }
    types::Result<types::SymbolInfo> SymbolInfo(
        const types::Symbol& symbol) override {
        return mData->GetSymbolInfo(symbol);
    }
    std::span<const types::Bar> Bars(const types::Symbol& symbol,
                                     types::Timeframe timeframe) const override;
    bool MarketOpen() const override { return mHours->IsOpen(mClock()); }
    std::optional<types::MarketHours::Session> CurrentOrNextSession()
        const override {
        return mHours->CurrentOrNext(mClock());
    }

    std::shared_ptr<data::DataSource> mData;
    std::shared_ptr<const types::MarketHours> mHours;
    // The session hooks (Run() thread only): the session open at the last
    // check, or the next one, and which of its hooks have been called. The
    // open counts as called for a session already open at Begin().
    std::optional<types::MarketHours::Session> mSession;
    bool mOpenCalled = false;
    bool mClosingCalled = false;
    // OnTimer: its interval, and when it is next due.
    std::chrono::milliseconds mTimerInterval{0};
    std::optional<types::Timestamp> mNextTimer;
    // The stale-quote check (Run() thread only): when each quoted symbol's
    // last quote came (or the run began), and which are reported stale.
    std::map<types::Symbol, types::Timestamp> mLastQuote;
    std::set<types::Symbol> mStaleQuotes;
    std::shared_ptr<trading::RiskGatedVenue> mGate;  // null without risk
    std::shared_ptr<OrderTracker> mTracker;
    std::unique_ptr<RunRecorder> mRecorder;  // null without options.trade_log
    // What the strategy trades through: the tracker, over the gate if there is
    // one, over the venue.
    std::shared_ptr<trading::TradingVenue> mVenue;
    std::unique_ptr<Strategy> mStrategy;
    const RunnerOptions mOptions;
    std::function<types::Timestamp()> mClock;
    // For the poller's log lines, which must not call into the strategy.
    const std::string mName;

    mutable std::mutex mMutex;  // guards the members below
    std::condition_variable mWake;
    std::deque<Event> mEvents;
    // Quotes waiting in mEvents, by symbol: at most one QuoteEvent per symbol
    // is queued, and it takes the quote from here when popped.
    std::map<types::Symbol, types::Quote> mPendingQuotes;
    std::size_t mConflated = 0;
    std::size_t mQueuedBars = 0;   // BarEvents in mEvents
    std::size_t mDroppedBars = 0;  // ever
    std::size_t mDropping = 0;     // dropped since the queue was last full

    // The poller thread, and what stops it.
    std::thread mPoller;
    std::mutex mPollerMutex;
    std::condition_variable mPollerWake;
    bool mPollerStop = false;  // guarded by mPollerMutex
    // mFillCursor in milliseconds since the epoch, for FetchFills to read on
    // the poller thread.
    std::atomic<std::int64_t> mPublishedCursor{0};

    // Touched only on the thread driving the strategy (mPushed and mRunStart
    // are set in Begin(), before the poller starts, and only read after).
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
