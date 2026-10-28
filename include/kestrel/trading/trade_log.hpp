#pragma once

#include <optional>
#include <span>

#include "kestrel/types/decimal.hpp"
#include "kestrel/types/error.hpp"
#include "kestrel/types/latency.hpp"
#include "kestrel/types/orders.hpp"
#include "kestrel/types/symbol.hpp"
#include "kestrel/types/time.hpp"

namespace kestrel::trading {

// One symbol's position and profit at a moment, in the quote asset.
struct PnlRow {
    types::Symbol symbol;
    types::Decimal quantity;       // base units held
    types::Decimal average_price;  // of that quantity; zero when flat
    // The price the position was valued at; unset until a price has been seen.
    std::optional<types::Decimal> mark_price;
    types::Decimal realized;  // net of fees, since the run began
    // (mark - average) * quantity; unset without a mark price.
    std::optional<types::Decimal> unrealized;
};

// Where a run's fills and P&L snapshots go (see strategy::RunRecorder). Called
// from the runner's thread only.
class TradeLog {
   public:
    virtual ~TradeLog() = default;

    // Each fill once. A fill that is written twice must not be stored twice.
    virtual types::Result<void> WriteFills(
        std::span<const types::Fill> fills) = 0;
    // The P&L of every symbol the run has a position or a price for, at `time`.
    virtual types::Result<void> WritePnl(types::Timestamp time,
                                         std::span<const PnlRow> rows) = 0;
    // The venue's response times for the window [start, end): one row per
    // endpoint called in it. Optional: a log that does not keep them ignores
    // the call.
    virtual types::Result<void> WriteLatency(
        types::Timestamp start, types::Timestamp end,
        std::span<const types::EndpointLatency> rows) {
        (void)start;
        (void)end;
        (void)rows;
        return {};
    }
    // The run ended cleanly at `time`.
    virtual types::Result<void> Finish(types::Timestamp time) = 0;
};

}  // namespace kestrel::trading
