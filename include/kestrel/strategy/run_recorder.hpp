#pragma once

#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <vector>

#include "kestrel/trading/pnl_tracker.hpp"
#include "kestrel/trading/trade_log.hpp"

namespace kestrel::strategy {

// Keeps a run's fills and P&L in a TradeLog: every fill the runner sees, and
// a snapshot of each symbol's position and profit every `interval` and when
// the run ends. P&L comes from PnlTracker (average cost, fees realized when
// paid, the same figures the risk gate and the paper venue use), starting from
// the positions the venue reported at Start; the mark for the unrealized part
// is the last price the runner saw (a quote's mid, or a bar's close).
//
// With a latency source it also stores the venue's response times: every
// `latency_interval` (and at the end) the source is asked for the window's
// per-endpoint figures, which go to TradeLog::WriteLatency. The source starts
// a new window each time it is called; the default is the HTTP statistics
// (net::HttpLatency().TakeWindow()). A window that cannot be written is lost
// (logged once), since the next call reports only the next window.
//
// Writing must never hurt trading: a failed write is logged once and retried
// at the next Flush (fills are kept, up to kMaxQueued, after which the oldest
// are dropped); a failed snapshot is skipped, since the next one supersedes it.
// Writes happen on the caller's thread, so a slow database delays the
// strategy; the runner calls Flush from its fill poll, not per event.
//
// Not thread-safe: used from the runner's strategy thread only.
class RunRecorder {
   public:
    static constexpr std::size_t kMaxQueued = 100'000;

    using LatencySource = std::function<std::vector<types::EndpointLatency>()>;

    RunRecorder(std::shared_ptr<trading::TradeLog> log,
                std::chrono::milliseconds interval);

    // Turns on latency windows of `interval` (zero keeps them off). A null
    // `source` means the process-wide HTTP statistics.
    void RecordLatency(std::chrono::milliseconds interval,
                       LatencySource source = nullptr);

    // Takes `positions` as the cost basis and zeroes the realized P&L.
    void Start(const std::vector<types::Position>& positions,
               types::Timestamp now);
    void NotePrice(const types::Symbol& symbol, const types::Decimal& price);
    // Applies the fill to the P&L and queues it for writing.
    void NoteFill(const types::Fill& fill);
    // Writes the queued fills.
    void Flush();
    // Writes a snapshot if `interval` has passed since the last (an interval
    // of zero never does, apart from Finish's).
    void Tick(types::Timestamp now);
    // Flush, a final snapshot, and marks the run finished.
    void Finish(types::Timestamp now);

   private:
    void Snapshot(types::Timestamp now);
    void WriteLatencyWindow(types::Timestamp now);

    std::shared_ptr<trading::TradeLog> mLog;
    std::chrono::milliseconds mInterval;
    trading::PnlTracker mPnl;
    std::map<types::Symbol, types::Decimal> mMarks;
    std::deque<types::Fill> mQueue;
    types::Timestamp mLastSnapshot;
    bool mFillsFailing = false;
    bool mPnlFailing = false;
    std::chrono::milliseconds mLatencyInterval{0};
    LatencySource mLatencySource;
    types::Timestamp mLatencyStart;
    bool mLatencyFailing = false;
};

}  // namespace kestrel::strategy
