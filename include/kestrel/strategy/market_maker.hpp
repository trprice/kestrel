#pragma once

#include <chrono>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <utility>

#include "kestrel/strategy/strategy.hpp"
#include "kestrel/types/decimal.hpp"

namespace kestrel::strategy {

// Drawdown protection. Every rule is off until its flag is set, and each has
// its own parameters, so a run chooses which to use. The rules came out of the
// live and replayed ZEC/USDT runs, where wins were a few basis points and a
// trending move cost tens of them.
//
// "The position" is the base held beyond what the strategy started with
// (inventory - start), with its average entry price worked out from the
// strategy's own fills; the starting holding is never sold by a rule. "P&L" is
// the cash from the strategy's fills (fees in the quote or base asset
// included) plus that position marked at the mid, so it is measured on every
// quote rather than when an order is attempted.
struct MarketMakerProtection {
    // Exit the position at market once it is this many basis points under its
    // average entry (the bid for a long, the ask for a short), then cancel the
    // quotes until the position is closed. The live runs used 40; the replays
    // found tighter stops only added stop-outs.
    bool stop_loss = false;
    types::Decimal stop_loss_bps{40, 0};

    // Halt for good, once the P&L since the start falls to -loss_cap_amount:
    // cancel the quotes, exit the position at market and quote no more.
    bool loss_cap = false;
    types::Decimal loss_cap_amount;  // quote asset; required when enabled

    // As loss_cap, but for the fall from the P&L's peak.
    bool drawdown_cap = false;
    types::Decimal drawdown_cap_amount;  // quote asset; required when enabled

    // Cancel both quotes and hold off quoting for spike_pause after the mid
    // moves (either way) by at least spike_3m_bps in 3 minutes, spike_5m_bps
    // in 5 or spike_15m_bps in 15. A move is measured against the newest mid
    // that is at least that old, and counts as nil until the strategy has seen
    // that much history.
    bool spike_standdown = false;
    types::Decimal spike_3m_bps{50, 0};
    types::Decimal spike_5m_bps{80, 0};
    types::Decimal spike_15m_bps{120, 0};
    std::chrono::milliseconds spike_pause{std::chrono::minutes(15)};

    // No bid while the 15 minute move is at or below -downtrend_bps.
    bool downtrend_filter = false;
    types::Decimal downtrend_bps{30, 0};

    // No bid while the 3 minute move is below -momentum_bps (zero: any fall).
    // The replays found this the one rule that helped on both tapes.
    bool momentum_filter = false;
    types::Decimal momentum_bps{0, 0};

    // After a stop_loss exit, quote neither side for cooldown. Does nothing
    // without stop_loss.
    bool cooldown_after_stop = false;
    std::chrono::milliseconds cooldown{std::chrono::minutes(30)};
};

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
    // Quote with post-only orders: a quote that would cross the book on
    // arrival is refused by the venue instead of filling as a taker, and the
    // next quote tries again.
    bool post_only = false;
    MarketMakerProtection protection;
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
// Protection rules (MarketMakerProtection) are opt-in. A rule that pauses a
// side cancels its resting quote; the exits are market orders for the whole
// position, and no second exit is sent until the first one's fills arrive.
//
// Not modelled yet: scaling the spread with volatility, pulling quotes when
// the feed goes quiet (the strategy only runs when a quote arrives), and fees
// charged in the base asset (the P&L counts them, the inventory does not).
class MarketMaker : public Strategy {
   public:
    // Throws std::invalid_argument unless quantity, half_spread_bps and
    // max_inventory are positive (max_inventory at least quantity) and
    // skew_bps, requote_bps and max_quote_age are not negative. Each enabled
    // protection rule's amounts must be positive (the filters' and the
    // durations may be zero).
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

    // Protection. All run on the quote's mid and the book.
    struct Pause {
        bool bid = false;
        bool ask = false;
    };
    // Applies the rules enabled in mParams.protection and says which sides
    // must not quote.
    Pause Protect(Context& context, const types::Quote& quote,
                  const types::Decimal& mid);
    // Basis-point move of the mid over `window`; zero without that history.
    types::Decimal Move(types::Timestamp now, std::chrono::milliseconds window,
                        const types::Decimal& mid) const;
    // Cancels both quotes and sells (or buys back) the position at market.
    // False if it could not (a quote cannot be pulled yet, the order was
    // refused) or nothing needs doing. Does nothing while an exit is pending.
    bool ExitPosition(Context& context, const char* why);
    void Halt(const char* why);
    types::Decimal Position() const { return mInventory - mStart; }

    // Fills the venue has reported in an order's state (an acknowledgement
    // that it filled at once, or its closing state) but whose OnFill has not
    // arrived yet. They count against the room on their side until it does,
    // so a quote that filled on arrival is not placed again and again before
    // the inventory catches up.
    struct Unconfirmed {
        types::Side side;
        types::Decimal expected;  // filled, per the venue
        types::Decimal seen;      // in OnFill so far
        types::Timestamp since;
        bool warned = false;
    };
    void ExpectFills(const std::string& order_id, types::Side side,
                     const types::Decimal& filled, const types::Decimal& seen,
                     types::Timestamp now);
    // Base units on `side` still to be confirmed by OnFill.
    types::Decimal UnconfirmedFills(types::Side side, types::Timestamp now);

    // The exit order in flight; empty id: none.
    struct Exit {
        std::string order_id;
        types::Decimal quantity;
        types::Decimal filled;
    };

    MarketMakerParams mParams;
    types::SymbolInfo mInfo;
    bool mReady = false;
    types::Decimal mInventory;  // base units held
    types::Decimal mStart;      // what was held at the start
    Resting mBid;
    Resting mAsk;

    types::Symbol mBaseAsset;
    types::Symbol mQuoteAsset;
    types::Decimal mCash;             // quote asset from fills, net of fees
    types::Decimal mEntry;            // average entry price of Position()
    types::Decimal mPeak;             // highest P&L seen
    types::Timestamp mStandUntil;     // spike stand-down ends
    types::Timestamp mCooldownUntil;  // stop-out cooldown ends
    std::optional<Exit> mExit;
    bool mHalted = false;
    std::size_t mWouldTake = 0;  // post-only quotes refused, for the log
    std::deque<std::pair<types::Timestamp, types::Decimal>> mMids;
    std::map<std::string, Unconfirmed> mUnconfirmed;  // by order ID
};

}  // namespace kestrel::strategy
