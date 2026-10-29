#pragma once

#include <algorithm>
#include <atomic>
#include <functional>
#include <utility>
#include <map>
#include <mutex>
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
        if (fail_cancel_all) {
            return types::MakeError(types::ErrorCategory::kNetwork, "down");
        }
        return std::exchange(cancel_all_result, {});
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
        if (before_get_fills) {
            before_get_fills();
        }
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
    // Without `pushes`, the venue has no push channel (the default).
    types::Result<std::unique_ptr<data::Subscription>> StreamExecutions(
        trading::ExecutionCallback callback) override {
        if (!pushes) {
            return TradingVenue::StreamExecutions(std::move(callback));
        }
        pushed = std::move(callback);
        return std::unique_ptr<data::Subscription>(
            std::make_unique<PushSubscription>(*this));
    }
    // Pushes `update` as the venue would.
    void Push(const trading::ExecutionUpdate& update) { pushed(update); }

    // Set before the venue is shared with other threads.
    bool pushes = false;
    bool reject_orders = false;
    types::OrderState ack_state = types::OrderState::kNew;
    bool fail_reads = false;  // positions and open orders
    bool fail_fills = false;
    bool fail_cancel_all = false;
    std::function<void()> before_get_fills;  // e.g. to hold a poll
    std::vector<std::string> cancel_all_result;  // returned once
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
    trading::ExecutionCallback pushed;
    std::atomic<int> push_stops{0};

   private:
    class PushSubscription : public data::Subscription {
       public:
        explicit PushSubscription(RecordingVenue& venue) : mVenue(venue) {}
        void Stop() override {
            if (!mStopped.exchange(true)) {
                ++mVenue.push_stops;
            }
        }
        ~PushSubscription() override { Stop(); }

       private:
        RecordingVenue& mVenue;
        std::atomic<bool> mStopped{false};
    };
};

// Remembers what a run writes. The writes lock, so SnapshotCount can be polled
// from another thread while a Run() is going; read the vectors directly only
// once the run has ended.
class FakeTradeLog : public trading::TradeLog {
   public:
    types::Result<void> WriteFills(
        std::span<const types::Fill> batch) override {
        std::lock_guard lock(mMutex);
        ++fill_writes;
        if (fail_fills) {
            return types::MakeError(types::ErrorCategory::kNetwork, "down");
        }
        fills.insert(fills.end(), batch.begin(), batch.end());
        return {};
    }
    types::Result<void> WritePnl(
        types::Timestamp time, std::span<const trading::PnlRow> rows) override {
        std::lock_guard lock(mMutex);
        if (fail_pnl) {
            return types::MakeError(types::ErrorCategory::kNetwork, "down");
        }
        snapshots.emplace_back(
            time, std::vector<trading::PnlRow>(rows.begin(), rows.end()));
        return {};
    }
    types::Result<void> WriteLatency(
        types::Timestamp start, types::Timestamp end,
        std::span<const types::EndpointLatency> rows) override {
        std::lock_guard lock(mMutex);
        if (fail_latency) {
            return types::MakeError(types::ErrorCategory::kNetwork, "down");
        }
        latency.push_back(
            {start, end,
             std::vector<types::EndpointLatency>(rows.begin(), rows.end())});
        return {};
    }
    types::Result<void> Finish(types::Timestamp time) override {
        std::lock_guard lock(mMutex);
        finished = time;
        return {};
    }
    std::size_t SnapshotCount() const {
        std::lock_guard lock(mMutex);
        return snapshots.size();
    }

    bool fail_fills = false;
    bool fail_pnl = false;
    bool fail_latency = false;
    int fill_writes = 0;
    std::vector<types::Fill> fills;
    std::vector<std::pair<types::Timestamp, std::vector<trading::PnlRow>>>
        snapshots;
    struct LatencyWindow {
        types::Timestamp start;
        types::Timestamp end;
        std::vector<types::EndpointLatency> rows;
    };
    std::vector<LatencyWindow> latency;
    std::optional<types::Timestamp> finished;

   private:
    mutable std::mutex mMutex;
};

}  // namespace kestrel::testing
