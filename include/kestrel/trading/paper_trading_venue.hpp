#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "kestrel/data/data_source.hpp"
#include "kestrel/trading/pnl_tracker.hpp"
#include "kestrel/trading/trading_venue.hpp"

namespace kestrel::trading {

// How a resting limit order is decided to have filled.
enum class LimitFillModel {
    // Fills, at its limit price and in full, on the first quote whose touch on
    // the other side has reached the limit (ask <= a buy's limit, bid >= a
    // sell's). Ignores queue position and trades: optimistic.
    kCross,
    // Conservative, for quote-driven strategies such as market makers. An
    // order fills only on evidence that it would have been reached:
    //   - the opposite touch is strictly through the limit (ask < a buy's
    //     limit): filled, in full unless capped by cap_limit_fills_at_quote_size;
    //   - a trade prints through the limit (below a buy's, above a sell's):
    //     filled in full, since every order at a better price filled first;
    //   - trades print at the limit price: they first use up the orders queued
    //     ahead of ours, and only what is left fills us.
    // The queue ahead is the size shown at our price when we join it, and only
    // trades shrink it: a smaller displayed size may be cancels behind or in
    // front of us, so it is ignored. If that size is not known (the quote has
    // none, or our price is behind the best), trades at the limit price never
    // fill us until the price becomes the touch and shows a size. Trades are
    // read off the quote's last/last_size/exchange_time changing; with no
    // last_size a trade is known only by its price. A touch equal to the limit
    // does not fill (kCross would).
    kConservative,
};

struct PaperOptions {
    // Starting balances by asset. Defaults to 10,000 USD.
    std::map<std::string, types::Decimal> initial_balances{
        {"USD", types::Decimal(10000, 0)}};
    // Fractions of notional (0.004 = 0.40%). Kraken's entry tier: 0.25% /
    // 0.40%.
    types::Decimal maker_fee_rate{25, 4};
    types::Decimal taker_fee_rate{40, 4};
    // Market orders fill at ask*(1+slippage) / bid*(1-slippage).
    types::Decimal slippage{5, 4};
    // Quote asset for symbols that have none (equities like AAPL).
    std::string default_quote_asset = "USD";
    // When true, a limit fill is capped at the size shown at the touch, so
    // large orders fill in several partial fills as quotes arrive.
    bool cap_limit_fills_at_quote_size = false;
    LimitFillModel limit_fill_model = LimitFillModel::kCross;
};

// Simulated venue that works with any DataSource. It decides fills from live
// quotes:
//   market  fills at ask*(1+slippage) (buy) or bid*(1-slippage) (sell), taker
//           fee
//   limit   marketable on arrival -> fills at the touch as taker; otherwise
//           rests and fills at its limit price (maker fee) when
//           PaperOptions::limit_fill_model says it would have been reached
//   stop    a stop-market: once the touch reaches the stop price it becomes a
//           market order
// Spot only: no shorting and no leverage. Buys reserve quote asset (including
// worst-case fees) and sells reserve base asset, so Balance.locked reflects
// open orders. Fees are charged in the quote asset.
//
// Thread-safe. Quotes typically arrive on the data source's WebSocket thread
// while strategies call the TradingVenue API from other threads; one mutex
// guards all state, and no network I/O happens while it is held.
class PaperTradingVenue : public TradingVenue {
   public:
    PaperTradingVenue(std::shared_ptr<data::DataSource> data_source,
                      PaperOptions options,
                      std::function<types::Timestamp()> clock = types::Now);
    ~PaperTradingVenue() override;

    // Subscribes to the data source's quotes for `symbols`. Optional: quotes
    // can also be pushed in through OnQuote().
    types::Result<void> Start(const std::vector<std::string>& symbols);
    void Stop();

    // Feed a quote. Thread-safe; this is also the stream callback.
    void OnQuote(const types::Quote& quote);

    std::string_view Name() const override { return "paper"; }
    types::Result<types::OrderAcknowledgement> PlaceOrder(
        const types::OrderRequest& request) override;
    types::Result<CancelOutcome> CancelOrder(
        const std::string& order_id) override;
    types::Result<std::vector<std::string>> CancelAll() override;
    types::Result<std::vector<types::OrderStatus>> GetOpenOrders() override;
    types::Result<types::OrderStatus> GetOrder(
        const std::string& order_id) override;
    types::Result<std::vector<types::Fill>> GetFills(
        types::Timestamp since) override;
    types::Result<std::vector<types::Position>> GetPositions() override;
    types::Result<std::vector<types::Balance>> GetBalances() override;

   private:
    struct Order;
    struct Candidate;
    // A trade seen in a quote stream.
    struct Trade {
        types::Decimal price;
        std::optional<types::Decimal> size;
    };

    // All of these require mMutex to be held.
    std::optional<Candidate> Evaluate(const Order& order,
                                      const types::Quote& quote,
                                      bool placing) const;
    bool Execute(Order& order, const Candidate& candidate,
                 types::Liquidity liquidity);
    void Close(Order& order, types::OrderState state, std::string reason);
    // For a resting limit order under LimitFillModel::kConservative: what, if
    // anything, `trade` (new in `quote`) and `quote` fill, and the queue
    // position afterwards.
    std::optional<Candidate> EvaluateQueued(
        Order& order, const types::Quote& quote,
        const std::optional<Trade>& trade) const;
    // Learns the size queued ahead of `order` from the book in `quote`.
    void UpdateQueue(Order& order, const types::Quote& quote) const;
    void TryMatch(Order& order, const types::Quote& quote, bool placing,
                  const std::optional<Trade>& trade = std::nullopt);
    void ExpireDayOrders();
    types::Decimal Reserve(const Order& order,
                           const types::Decimal& remaining) const;
    void Adjust(std::map<std::string, types::Decimal>& book,
                const std::string& asset, const types::Decimal& delta);

    std::shared_ptr<data::DataSource> mDataSource;
    const PaperOptions mOptions;
    const std::function<types::Timestamp()> mClock;

    mutable std::mutex mMutex;
    std::map<std::string, Order>
        mOrders;  // by order id; ids sort in creation order
    // The orders in mOrders that may still be active, in creation order
    // (map nodes are stable). Closed orders are dropped by ExpireDayOrders(),
    // so scans do not grow with the number of orders ever placed.
    std::vector<Order*> mActive;
    std::vector<types::Fill> mFills;
    std::map<std::string, types::Decimal> mTotal;
    std::map<std::string, types::Decimal> mLocked;
    PnlTracker mPnl;  // positions and realized P&L, from the fills
    std::map<types::Symbol, types::Quote> mQuotes;
    std::uint64_t mNextOrder = 1;
    std::uint64_t mNextFill = 1;

    std::unique_ptr<data::Subscription> mSubscription;  // last: destroyed first
};

}  // namespace kestrel::trading
