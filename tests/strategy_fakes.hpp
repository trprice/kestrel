#pragma once

#include <algorithm>
#include <atomic>
#include <map>
#include <vector>

#include "kestrel/trading/trade_log.hpp"
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
            ack_state, types::FromEpochSeconds(0)};
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
    types::Result<types::OrderStatus> GetOrder(
        const std::string& order_id) override {
        ++get_order_calls;
        if (auto it = order_states.find(order_id); it != order_states.end()) {
            return it->second;
        }
        return types::MakeError(types::ErrorCategory::kNotFound, "unknown");
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
        return balances;
    }

    // Set before the venue is shared with other threads.
    bool reject_orders = false;
    types::OrderState ack_state = types::OrderState::kNew;
    bool fail_reads = false;  // positions and open orders
    bool fail_fills = false;
    std::vector<types::Position> positions;
    std::vector<types::Balance> balances;
    std::vector<types::OrderStatus> open_orders;
    std::vector<types::Fill> fills;
    std::map<std::string, types::OrderStatus> order_states;  // GetOrder

    int attempts = 0;
    int cancel_all_calls = 0;
    int get_order_calls = 0;
    std::atomic<int> get_fills_calls{0};
    std::vector<types::OrderRequest> orders;
};

// Remembers what a run writes.
class FakeTradeLog : public trading::TradeLog {
   public:
    types::Result<void> WriteFills(
        std::span<const types::Fill> batch) override {
        ++fill_writes;
        if (fail_fills) {
            return types::MakeError(types::ErrorCategory::kNetwork, "down");
        }
        fills.insert(fills.end(), batch.begin(), batch.end());
        return {};
    }
    types::Result<void> WritePnl(
        types::Timestamp time, std::span<const trading::PnlRow> rows) override {
        if (fail_pnl) {
            return types::MakeError(types::ErrorCategory::kNetwork, "down");
        }
        snapshots.emplace_back(
            time, std::vector<trading::PnlRow>(rows.begin(), rows.end()));
        return {};
    }
    types::Result<void> Finish(types::Timestamp time) override {
        finished = time;
        return {};
    }

    bool fail_fills = false;
    bool fail_pnl = false;
    int fill_writes = 0;
    std::vector<types::Fill> fills;
    std::vector<std::pair<types::Timestamp, std::vector<trading::PnlRow>>>
        snapshots;
    std::optional<types::Timestamp> finished;
};

}  // namespace kestrel::testing
