#pragma once

#include <functional>
#include <memory>
#include <optional>

#include "kestrel/trading/trading_venue.hpp"
#include "kestrel/types/market_hours.hpp"

namespace kestrel::trading {

// A TradingVenue that refuses new orders while the market is closed
// (kRejected, "market closed: ..."), saying when it next opens. Everything
// else passes straight through, cancels included, so a strategy can always
// take its orders down. The runner puts one in front of the venue whenever
// the data source's market closes (DataSource::Hours), in paper, live and
// backtest runs alike.
class SessionGatedVenue final : public TradingVenue {
   public:
    SessionGatedVenue(std::shared_ptr<TradingVenue> venue,
                      std::shared_ptr<const types::MarketHours> hours,
                      std::function<types::Timestamp()> clock = types::Now)
        : mVenue(std::move(venue)),
          // No hours: always open, rather than a null to check on every
          // order.
          mHours(hours ? std::move(hours) : types::MarketHours::AlwaysOpen()),
          mClock(std::move(clock)) {}

    std::string_view Name() const override { return mVenue->Name(); }
    types::Result<types::OrderAcknowledgement> PlaceOrder(
        const types::OrderRequest& request) override;
    types::Result<types::OcoAcknowledgement> PlaceOco(
        const types::OcoRequest& request) override;
    types::Result<CancelOutcome> CancelOrder(
        const std::string& order_id) override {
        return mVenue->CancelOrder(order_id);
    }
    types::Result<std::vector<std::string>> CancelAll() override {
        return mVenue->CancelAll();
    }
    types::Result<std::vector<types::OrderStatus>> GetOpenOrders() override {
        return mVenue->GetOpenOrders();
    }
    types::Result<types::OrderStatus> GetOrder(
        const std::string& order_id) override {
        return mVenue->GetOrder(order_id);
    }
    types::Result<std::vector<types::Fill>> GetFills(
        types::Timestamp since) override {
        return mVenue->GetFills(since);
    }
    types::Result<std::vector<types::Position>> GetPositions() override {
        return mVenue->GetPositions();
    }
    types::Result<std::vector<types::Balance>> GetBalances() override {
        return mVenue->GetBalances();
    }
    types::Result<std::unique_ptr<data::Subscription>> StreamExecutions(
        ExecutionCallback callback) override {
        return mVenue->StreamExecutions(std::move(callback));
    }

   private:
    // The refusal for an order on `symbol` now, if the market is closed.
    std::optional<types::Error> Closed(const types::Symbol& symbol) const;

    const std::shared_ptr<TradingVenue> mVenue;
    const std::shared_ptr<const types::MarketHours> mHours;
    const std::function<types::Timestamp()> mClock;
};

}  // namespace kestrel::trading
