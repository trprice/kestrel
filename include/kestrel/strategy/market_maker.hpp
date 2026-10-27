#pragma once

#include <chrono>
#include <optional>
#include <string>

#include "kestrel/strategy/strategy.hpp"
#include "kestrel/types/decimal.hpp"

namespace kestrel::strategy {

struct MarketMakerParams {
    types::Symbol symbol;
    types::Decimal quantity;  // base-asset units per quote
    // Distance of each quote from the reservation price, in basis points of
    // the mid. It must cover the fees on both legs and leave room for adverse
    // selection.
    types::Decimal half_spread_bps;
    // How far the position may stray from where it started, in base units,
    // either way. Past it the side that would add to the position stops
    // quoting.
    types::Decimal max_inventory;
    // How far the reservation price moves, in basis points, when the position
    // is max_inventory away from where it started (in proportion for less):
    // long, both quotes shift down so a sale is likelier than a purchase.
    types::Decimal skew_bps{0, 0};
    // A resting quote is replaced only when its target price has moved by at
    // least this many basis points (and at least one tick), since each
    // replacement goes to the back of the queue.
    types::Decimal requote_bps{1, 0};
    // Replace a quote older than this even if its price is still right;
    // zero never does.
    std::chrono::milliseconds max_quote_age{0};
};

// Quotes a bid and an ask around a reservation price: the mid, shifted away
// from the side the position is heavy on. Spot only, so the ask is quoted
// only while there is base asset to sell, and the strategy starts from
// whatever the venue holds of the base asset.
//
// Quotes never cross the book: a bid is held to one tick under the best ask,
// an ask to one tick over the best bid. Nothing is quoted while the book is
// crossed or locked.
//
// When a cancel loses a race with a fill, that side waits for the venue to
// report the order's final state before quoting again, so inventory is never
// counted twice. Fills move the inventory (OnFill); order state comes from
// OnOrderUpdate, so a rejected or expired quote is requoted.
//
// Not modelled yet: scaling the spread with volatility, pulling quotes when
// the feed goes quiet (the strategy only runs when a quote arrives), and fees
// charged in the base asset.
class MarketMaker : public Strategy {
   public:
    // Throws std::invalid_argument unless quantity, half_spread_bps and
    // max_inventory are positive (max_inventory at least quantity) and
    // skew_bps, requote_bps and max_quote_age are not negative.
    explicit MarketMaker(MarketMakerParams params);

    std::string_view Name() const override { return "market_maker"; }
    Requirements Needs() const override;
    void OnStart(Context& context) override;
    void OnQuote(Context& context, const types::Quote& quote) override;
    void OnFill(Context& context, const types::Fill& fill) override;
    void OnOrderUpdate(Context& context,
                       const types::OrderStatus& status) override;
    void OnStop(Context& context) override;

   private:
    // One resting quote.
    struct Resting {
        std::string order_id;  // empty: nothing resting
        types::Decimal price;
        types::Decimal quantity;
        types::Decimal filled;
        types::Timestamp placed;
        bool cancel_raced = false;  // a fill beat our cancel; await its state

        types::Decimal Remaining() const { return quantity - filled; }
    };

    // Brings `quote` to `price` / `size` (no price or size: nothing quoted).
    void Manage(Context& context, Resting& quote, types::Side side,
                const std::optional<types::Decimal>& price,
                const types::Decimal& size, const types::Decimal& room,
                const types::Decimal& threshold);
    // Cancels a resting quote; false if it cannot be replaced yet.
    bool Pull(Context& context, Resting& quote);
    types::Decimal Size(const types::Decimal& room) const;

    MarketMakerParams mParams;
    types::SymbolInfo mInfo;
    bool mReady = false;
    types::Decimal mInventory;  // base units held
    types::Decimal mStart;      // what was held at the start
    Resting mBid;
    Resting mAsk;
};

}  // namespace kestrel::strategy
