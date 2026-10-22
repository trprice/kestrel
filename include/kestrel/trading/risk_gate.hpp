#pragma once

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "kestrel/trading/pnl_tracker.hpp"
#include "kestrel/trading/trading_venue.hpp"
#include "kestrel/types/decimal.hpp"
#include "kestrel/types/symbol.hpp"
#include "kestrel/types/time.hpp"

namespace kestrel::trading {

// Every limit is optional; an unset one is not checked. Amounts are in the
// quote asset and assume all traded symbols share one quote asset.
struct RiskLimits {
    // Price * quantity of a single order.
    std::optional<types::Decimal> max_order_notional;
    // Per symbol: what is held, plus open buy orders, plus the new order.
    std::optional<types::Decimal> max_position_notional;
    std::optional<std::size_t> max_open_orders;
    // Orders that reach the venue in any sliding 60 seconds.
    std::optional<std::size_t> max_orders_per_minute;
    // Realized loss (net of fees) since the start of the UTC day or since
    // ResetDailyBaseline, summed over all symbols. Once reached, only orders
    // that reduce a position are allowed until the next UTC day. The loss is
    // worked out by the gate from the venue's fills (see PnlTracker), not read
    // from Position::realized_pnl.
    std::optional<types::Decimal> max_daily_loss;
    // Empty means any symbol.
    std::set<types::Symbol> allowed_symbols;
    // Reject orders priced from a reference price older than this. Unset
    // means a reference price never goes stale.
    std::optional<std::chrono::milliseconds> max_price_age;
};

// A TradingVenue that checks every order against RiskLimits before passing it
// to the real venue. Strategies that only hold the gate cannot bypass it.
// Rejected orders fail with kRejected and a message starting "risk:" and never
// reach the venue. Cancels and all queries pass straight through.
//
// The gate fails closed: if a check needs data (positions, open orders, a
// price) that cannot be had, the order is rejected.
//
// Prices: a limit order is valued at its limit price and a stop at its stop
// price; a market order has no price of its own, so it is valued at the
// reference price last given to NoteReferencePrice (the Runner supplies
// these from bars and quotes). Without a usable reference price a market
// order is rejected whenever a notional limit is set.
//
// An order that reduces a position (a sell of no more than is held) is the
// only kind allowed while trading is halted or the daily loss limit is hit.
//
// Thread-safe. Orders are checked and placed one at a time, so two concurrent
// orders cannot together exceed a limit; that means the venue call is made
// while holding the gate's order lock.
class RiskGatedVenue : public TradingVenue {
   public:
    RiskGatedVenue(std::shared_ptr<TradingVenue> venue, RiskLimits limits,
                   std::function<types::Timestamp()> clock = types::Now);

    // Latest price for valuing market orders, stamped with the clock.
    void NoteReferencePrice(const types::Symbol& symbol,
                            const types::Decimal& price);

    // Starts the loss count: takes the venue's current positions as the cost
    // basis and counts fills from now on. The gate does this by itself at the
    // first order if it has not been done, but a loss realized before then
    // would be missed, so call it when trading starts. Needed only with
    // max_daily_loss. A fill is counted if its time is at or after this call,
    // so the venue's and the gate's clocks should roughly agree.
    types::Result<void> ResetDailyBaseline();

    // Kill switch: stops all orders except position-reducing ones and cancels
    // everything open. Does not take the order lock, so it is not delayed by
    // an order in flight. The halt holds even if the cancel fails.
    types::Result<std::vector<std::string>> Halt(const std::string& reason);
    void Resume() { mHalted = false; }
    bool IsHalted() const { return mHalted; }

    std::string_view Name() const override { return mVenue->Name(); }
    types::Result<types::OrderAcknowledgement> PlaceOrder(
        const types::OrderRequest& request) override;
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

   private:
    struct ReferencePrice {
        types::Decimal price;
        types::Timestamp time;
    };

    // All require mMutex.
    types::Result<void> StartLossCount();
    // Brings the P&L up to date with the venue's fills and returns why the
    // daily loss limit blocks trading, if it does.
    types::Result<std::optional<std::string>> CheckDailyLoss(
        types::Timestamp now);
    std::optional<types::Decimal> PriceFor(const types::OrderRequest& request,
                                           types::Timestamp now) const;

    const std::shared_ptr<TradingVenue> mVenue;
    const RiskLimits mLimits;
    const std::function<types::Timestamp()> mClock;

    std::atomic<bool> mHalted{false};

    mutable std::mutex mPriceMutex;
    std::map<types::Symbol, ReferencePrice> mPrices;

    std::mutex mMutex;  // serializes checking and placing orders
    std::deque<types::Timestamp> mRecentOrders;
    // The daily loss count. mFillCursor is the newest fill time seen; fills
    // at that time come back on every poll, so mSeenFills (pruned to the
    // cursor) filters them out.
    bool mCounting = false;
    PnlTracker mPnl;
    std::chrono::sys_days mDay{};
    types::Timestamp mFillCursor;
    std::map<std::string, types::Timestamp> mSeenFills;
};

}  // namespace kestrel::trading
