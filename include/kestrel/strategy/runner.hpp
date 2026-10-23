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
#include "kestrel/trading/trading_venue.hpp"

namespace kestrel::strategy {

struct RunnerOptions {
    // When set, the strategy gets a RiskGatedVenue with these limits instead
    // of the venue itself, fed with prices from the bars and quotes.
    std::optional<trading::RiskLimits> risk;
    // How often Run() asks the venue for new fills; zero turns polling off.
    std::chrono::milliseconds fill_poll_interval{1000};
};

// Drives one strategy: subscribes to what it asks for, then feeds the events
// to it one at a time on the thread that called Run().
//
// The data source delivers on its WebSocket thread, where a strategy must not
// block (it would stall the stream) and where Subscription::Stop() cannot be
// called. The callbacks therefore only enqueue; the strategy runs on the
// Run() thread, which is also free to call the venue over HTTP.
//
// Fills are found by polling TradingVenue::GetFills and delivered to OnFill
// once each. The poll starts from the time Begin() ran, and it assumes the
// venue's fill times and the runner's clock roughly agree.
//
// The queue is unbounded, so a strategy that is slower than the quote stream
// falls further and further behind.
class Runner : private Context {
   public:
    Runner(std::shared_ptr<data::DataSource> data_source,
           std::shared_ptr<trading::TradingVenue> venue,
           std::unique_ptr<Strategy> strategy, RunnerOptions options = {},
           std::function<types::Timestamp()> clock = types::Now);

    // Begin(), then processes events until Stop(), then End(). Blocks.
    // Returns an error only if Begin() fails; once the loop is running,
    // problems are logged and the loop carries on. Call once.
    types::Result<void> Run();

    // Thread-safe, but not async-signal-safe (it takes a mutex): call it from
    // a watch thread, not a signal handler. Events already queued are
    // processed first. May be called before Run().
    void Stop();

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
    struct QuoteEvent {
        types::Quote quote;
    };
    struct StopEvent {};
    using Event = std::variant<BarEvent, QuoteEvent, StopEvent>;
    using Series = std::pair<types::Symbol, types::Timeframe>;
    using SteadyTime = std::chrono::steady_clock::time_point;

    void Push(Event event);
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
    std::shared_ptr<trading::TradingVenue> mVenue;   // the gate, or the venue
    std::unique_ptr<Strategy> mStrategy;
    const RunnerOptions mOptions;
    std::function<types::Timestamp()> mClock;

    std::mutex mMutex;  // guards mEvents
    std::condition_variable mWake;
    std::deque<Event> mEvents;

    // Touched only on the thread driving the strategy.
    bool mBegun = false;
    std::map<Series, std::vector<types::Bar>> mBars;
    std::size_t mWindowSize = 1;
    types::Timestamp mFillCursor;
    std::map<std::string, types::Timestamp> mSeenFills;  // at or after cursor

    // Last: destroyed first, so no callback outlives the members above.
    std::vector<std::unique_ptr<data::Subscription>> mSubscriptions;
};

}  // namespace kestrel::strategy
