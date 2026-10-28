#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "kestrel/data/data_source.hpp"
#include "kestrel/types/error.hpp"
#include "kestrel/types/orders.hpp"

namespace kestrel::trading {

enum class CancelOutcome {
    kCancelled,      // the order was active and is now cancelled
    kAlreadyFilled,  // too late: it filled first (a normal outcome, not an
                     // error)
    kAlreadyClosed,  // already cancelled, expired or rejected
};

// Something that happened to an order, as the venue pushes it: a new fill, the
// order's state, or both (the state after the fill).
struct ExecutionUpdate {
    std::optional<types::Fill> fill;
    std::optional<types::OrderStatus> order;
};

// THREADING: called from whichever thread the venue learns of the change on
// (a stream thread, or the thread that called PlaceOrder or CancelOrder), one
// update at a time and in the order they happened. It must be thread-safe,
// return quickly, not throw, and not call back into the venue.
using ExecutionCallback = std::function<void(const ExecutionUpdate&)>;

// Order execution, separated from market data. Live and paper venues
// implement the same interface so strategies never know which one they use.
// All methods are thread-safe.
class TradingVenue {
   public:
    virtual ~TradingVenue() = default;

    virtual std::string_view Name() const = 0;

    // Rounds the price to the symbol's tick size (buy limits down, sell limits
    // up) and the quantity down to its lot size, rejects orders under the
    // minimum size, then submits. Returns the venue's order ID and initial
    // state.
    virtual types::Result<types::OrderAcknowledgement> PlaceOrder(
        const types::OrderRequest& request) = 0;

    // An unknown ID is kNotFound; an order that already filled is a normal
    // outcome reported via CancelOutcome.
    virtual types::Result<CancelOutcome> CancelOrder(
        const std::string& order_id) = 0;

    // Cancels every active order; returns the IDs that were cancelled.
    virtual types::Result<std::vector<std::string>> CancelAll() = 0;

    virtual types::Result<std::vector<types::OrderStatus>> GetOpenOrders() = 0;

    // The current state of one order, open or closed (filled, cancelled,
    // rejected or expired). An unknown ID is kNotFound. Venues that forget
    // closed orders after a while report those as kNotFound too.
    virtual types::Result<types::OrderStatus> GetOrder(
        const std::string& order_id) = 0;

    // Every fill, partial fills included, with fill time >= since.
    virtual types::Result<std::vector<types::Fill>> GetFills(
        types::Timestamp since) = 0;

    virtual types::Result<std::vector<types::Position>> GetPositions() = 0;
    virtual types::Result<std::vector<types::Balance>> GetBalances() = 0;

    // Pushes fills and order changes to `callback` as they happen, until the
    // subscription is stopped or destroyed, so a caller need not wait for its
    // next GetFills poll. Optional: a venue without a push channel returns
    // kNotImplemented, and callers poll. A pushed update may also show up in
    // GetFills / GetOpenOrders, and an update can be missed (a stream that
    // reconnects), so callers keep polling now and then and drop duplicates
    // by fill ID.
    virtual types::Result<std::unique_ptr<data::Subscription>> StreamExecutions(
        ExecutionCallback callback) {
        (void)callback;
        return types::MakeError(
            types::ErrorCategory::kNotImplemented,
            std::string(Name()) + " does not push executions");
    }
};

}  // namespace kestrel::trading
