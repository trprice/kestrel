#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "kestrel/types/error.hpp"
#include "kestrel/types/orders.hpp"

namespace kestrel::trading {

enum class CancelOutcome {
  kCancelled,      // the order was active and is now cancelled
  kAlreadyFilled,  // too late: it filled first (a normal outcome, not an error)
  kAlreadyClosed,  // already cancelled, expired or rejected
};

// Order execution, separated from market data. Live and paper venues
// implement the same interface so strategies never know which one they use.
// All methods are thread-safe.
class TradingVenue {
 public:
  virtual ~TradingVenue() = default;

  virtual std::string_view Name() const = 0;

  // Rounds the price to the symbol's tick size (buy limits down, sell limits
  // up) and the quantity down to its lot size, rejects orders under the
  // minimum size, then submits. Returns the venue's order ID and initial state.
  virtual types::Result<types::OrderAcknowledgement> PlaceOrder(const types::OrderRequest& request) = 0;

  // An unknown ID is kNotFound; an order that already filled is a normal
  // outcome reported via CancelOutcome.
  virtual types::Result<CancelOutcome> CancelOrder(const std::string& order_id) = 0;

  // Cancels every active order; returns the IDs that were cancelled.
  virtual types::Result<std::vector<std::string>> CancelAll() = 0;

  virtual types::Result<std::vector<types::OrderStatus>> GetOpenOrders() = 0;

  // Every fill, partial fills included, with fill time >= since.
  virtual types::Result<std::vector<types::Fill>> GetFills(types::Timestamp since) = 0;

  virtual types::Result<std::vector<types::Position>> GetPositions() = 0;
  virtual types::Result<std::vector<types::Balance>> GetBalances() = 0;
};

}  // namespace kestrel::trading
