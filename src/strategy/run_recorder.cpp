#include "kestrel/strategy/run_recorder.hpp"

#include <spdlog/spdlog.h>

#include <set>

#include "kestrel/net/timed_http_client.hpp"

namespace kestrel::strategy {

using types::Decimal;

RunRecorder::RunRecorder(std::shared_ptr<trading::TradeLog> log,
                         std::chrono::milliseconds interval)
    : mLog(std::move(log)), mInterval(interval) {}

void RunRecorder::RecordLatency(std::chrono::milliseconds interval,
                                LatencySource source) {
    mLatencyInterval = interval;
    mLatencySource = source ? std::move(source)
                            : [] { return net::HttpLatency().TakeWindow(); };
}

void RunRecorder::Start(const std::vector<types::Position>& positions,
                        types::Timestamp now) {
    mPnl.Reset(positions);
    mLastSnapshot = now;
    mLatencyStart = now;
    if (mLatencySource) {
        // What came before the run started belongs to no window.
        (void)mLatencySource();
    }
}

void RunRecorder::NotePrice(const types::Symbol& symbol, const Decimal& price) {
    mMarks[symbol] = price;
}

void RunRecorder::NoteFill(const types::Fill& fill) {
    mPnl.Apply(fill);
    if (mQueue.size() >= kMaxQueued) {
        spdlog::error("run recorder: dropping the oldest unwritten fill");
        mQueue.pop_front();
    }
    mQueue.push_back(fill);
}

void RunRecorder::Flush() {
    if (mQueue.empty()) {
        return;
    }
    const std::vector<types::Fill> batch(mQueue.begin(), mQueue.end());
    auto written = mLog->WriteFills(batch);
    if (!written) {
        if (!mFillsFailing) {
            spdlog::error("run recorder: cannot write fills, will retry: {}",
                          written.error().ToString());
        }
        mFillsFailing = true;
        return;
    }
    if (mFillsFailing) {
        spdlog::info("run recorder: fills are being written again");
    }
    mFillsFailing = false;
    mQueue.clear();
}

void RunRecorder::Tick(types::Timestamp now) {
    if (mInterval > std::chrono::milliseconds::zero() &&
        now - mLastSnapshot >= mInterval) {
        Snapshot(now);
    }
    if (mLatencySource &&
        mLatencyInterval > std::chrono::milliseconds::zero() &&
        now - mLatencyStart >= mLatencyInterval) {
        WriteLatencyWindow(now);
    }
}

void RunRecorder::WriteLatencyWindow(types::Timestamp now) {
    const auto start = mLatencyStart;
    mLatencyStart = now;
    const auto rows = mLatencySource();
    if (rows.empty()) {
        return;
    }
    auto written = mLog->WriteLatency(start, now, rows);
    if (!written) {
        if (!mLatencyFailing) {
            spdlog::error("run recorder: cannot write latency figures: {}",
                          written.error().ToString());
        }
        mLatencyFailing = true;
        return;
    }
    mLatencyFailing = false;
}

void RunRecorder::Snapshot(types::Timestamp now) {
    mLastSnapshot = now;
    std::vector<trading::PnlRow> rows;
    std::set<types::Symbol> seen;
    for (const types::Position& p : mPnl.Positions()) {
        trading::PnlRow row;
        row.symbol = p.symbol;
        row.quantity = p.quantity;
        row.average_price = p.average_price;
        row.realized = p.realized_pnl;
        if (auto mark = mMarks.find(p.symbol); mark != mMarks.end()) {
            row.mark_price = mark->second;
            row.unrealized = (mark->second - p.average_price) * p.quantity;
        }
        seen.insert(p.symbol);
        rows.push_back(std::move(row));
    }
    // A symbol with a price but no fills yet still gets a row: it shows the
    // run is alive and priced.
    for (const auto& [symbol, price] : mMarks) {
        if (!seen.contains(symbol)) {
            trading::PnlRow row;
            row.symbol = symbol;
            row.mark_price = price;
            row.unrealized = Decimal();
            rows.push_back(std::move(row));
        }
    }
    if (rows.empty()) {
        return;
    }
    auto written = mLog->WritePnl(now, rows);
    if (!written) {
        if (!mPnlFailing) {
            spdlog::error("run recorder: cannot write a P&L snapshot: {}",
                          written.error().ToString());
        }
        mPnlFailing = true;
        return;
    }
    mPnlFailing = false;
}

void RunRecorder::Finish(types::Timestamp now) {
    Flush();
    Snapshot(now);
    if (mLatencySource &&
        mLatencyInterval > std::chrono::milliseconds::zero()) {
        WriteLatencyWindow(now);
    }
    if (auto done = mLog->Finish(now); !done) {
        spdlog::error("run recorder: cannot mark the run finished: {}",
                      done.error().ToString());
    }
}

}  // namespace kestrel::strategy
