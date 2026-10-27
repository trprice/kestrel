#pragma once

#include <chrono>
#include <deque>
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

    RunRecorder(std::shared_ptr<trading::TradeLog> log,
                std::chrono::milliseconds interval);

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

    std::shared_ptr<trading::TradeLog> mLog;
    std::chrono::milliseconds mInterval;
    trading::PnlTracker mPnl;
    std::map<types::Symbol, types::Decimal> mMarks;
    std::deque<types::Fill> mQueue;
    types::Timestamp mLastSnapshot;
    bool mFillsFailing = false;
    bool mPnlFailing = false;
};

}  // namespace kestrel::strategy
