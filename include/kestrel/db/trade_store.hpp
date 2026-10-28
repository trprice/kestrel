#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "kestrel/db/database.hpp"
#include "kestrel/trading/trade_log.hpp"

namespace kestrel::db {

struct RunInfo {
    std::string strategy;
    std::string venue;
    std::string mode;  // "paper" | "live"
    types::Timestamp started;
};

// What a run leaves in the database, while it runs and afterwards:
//   runs           one row per run; ended_at stays NULL until a clean finish,
//                  so a NULL on an old run means the process died
//   fills          every fill of the run
//   pnl_snapshots  per symbol and snapshot time: position, mark, realized and
//                  unrealized P&L
//   venue_latency  per endpoint and window (normally a minute): calls, errors,
//                  rate-limited calls and min/p50/p95/p99/max response time
//                  in milliseconds
//   pnl_totals     a view summing the snapshots over symbols (a total only
//                  makes sense when the symbols share a quote asset)
// e.g.  SELECT * FROM pnl_totals WHERE run_id = 3 ORDER BY snapshot_time DESC;
//       SELECT window_start, calls, p50_ms, p99_ms FROM venue_latency
//        WHERE run_id = 3 AND endpoint LIKE 'GET %/account' ORDER BY 1;
//
// One TradeStore is one run: StartRun creates the row and the object writes
// under its id.
class TradeStore : public trading::TradeLog {
   public:
    struct StoredPnl {
        types::Timestamp time;
        trading::PnlRow row;
    };

    static types::Result<std::unique_ptr<TradeStore>> StartRun(
        std::shared_ptr<Database> database, const RunInfo& info);

    std::int64_t RunId() const { return mRunId; }

    types::Result<void> WriteFills(std::span<const types::Fill> fills) override;
    types::Result<void> WritePnl(
        types::Timestamp time, std::span<const trading::PnlRow> rows) override;
    types::Result<void> WriteLatency(
        types::Timestamp start, types::Timestamp end,
        std::span<const types::EndpointLatency> rows) override;
    types::Result<void> Finish(types::Timestamp time) override;

    struct StoredLatency {
        types::Timestamp start;
        types::Timestamp end;
        types::EndpointLatency row;
    };

    // This run's rows, oldest first.
    types::Result<std::vector<types::Fill>> LoadFills() const;
    types::Result<std::vector<StoredPnl>> LoadPnl() const;
    types::Result<std::vector<StoredLatency>> LoadLatency() const;

   private:
    TradeStore(std::shared_ptr<Database> database, std::int64_t run_id)
        : mDatabase(std::move(database)), mRunId(run_id) {}

    std::shared_ptr<Database> mDatabase;
    std::int64_t mRunId;
};

}  // namespace kestrel::db
