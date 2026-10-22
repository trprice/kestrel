#pragma once

#include <algorithm>
#include <atomic>
#include <vector>

#include "kestrel/trading/trading_venue.hpp"

namespace kestrel::testing {

// Records the orders a strategy places; every other call returns nothing.
class RecordingVenue : public trading::TradingVenue {
   public:
    std::string_view Name() const override { return "recording"; }

    types::Result<types::OrderAcknowledgement> PlaceOrder(
        const types::OrderRequest& request) override {
        ++attempts;
        if (reject_orders) {
            return types::MakeError(types::ErrorCategory::kRejected, "no");
        }
        orders.push_back(request);
        return types::OrderAcknowledgement{
            "order-" + std::to_string(orders.size()), request.client_order_id,
            types::OrderState::kNew, types::FromEpochSeconds(0)};
    }
    types::Result<trading::CancelOutcome> CancelOrder(
        const std::string&) override {
        return types::MakeError(types::ErrorCategory::kNotFound, "none");
    }
    types::Result<std::vector<std::string>> CancelAll() override {
        ++cancel_all_calls;
        return std::vector<std::string>{};
    }
    types::Result<std::vector<types::OrderStatus>> GetOpenOrders() override {
        if (fail_reads) {
            return types::MakeError(types::ErrorCategory::kNetwork, "down");
        }
        return open_orders;
    }
    types::Result<std::vector<types::Fill>> GetFills(
        types::Timestamp since) override {
        ++get_fills_calls;
        if (fail_fills) {
            return types::MakeError(types::ErrorCategory::kNetwork, "down");
        }
        std::vector<types::Fill> out;
        std::copy_if(fills.begin(), fills.end(), std::back_inserter(out),
                     [&](const types::Fill& f) { return f.time >= since; });
        return out;
    }
    types::Result<std::vector<types::Position>> GetPositions() override {
        if (fail_reads) {
            return types::MakeError(types::ErrorCategory::kNetwork, "down");
        }
        return positions;
    }
    types::Result<std::vector<types::Balance>> GetBalances() override {
        return std::vector<types::Balance>{};
    }

    // Set before the venue is shared with other threads.
    bool reject_orders = false;
    bool fail_reads = false;  // positions and open orders
    bool fail_fills = false;
    std::vector<types::Position> positions;
    std::vector<types::OrderStatus> open_orders;
    std::vector<types::Fill> fills;

    int attempts = 0;
    int cancel_all_calls = 0;
    std::atomic<int> get_fills_calls{0};
    std::vector<types::OrderRequest> orders;
};

}  // namespace kestrel::testing
