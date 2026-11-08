#pragma once

#include <chrono>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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

    // With loss_cap, drawdown_cap, spike_standdown or cooldown_after_stop
    // on, their state is kept across restarts where the run has a store
    // (`run` with a [database]; see Context::State): the halt, the P&L so
    // far and its peak, and when a stand-down or cooldown ends. A restart
    // then carries on from it, so the caps count every run since the state
    // was last reset, and a halted maker stays halted. reset_state starts
    // afresh (and forgets the saved state).
    bool reset_state = false;
};

struct MarketMakerParams {
    types::Symbol symbol;
    types::Decimal quantity;  // base-asset units per quote
    // Distance of each quote from the reservation price, in basis points of
    // the mid. It must cover the fees on both legs and leave room for adverse
    // selection.
    types::Decimal half_spread_bps;
    // Volatility scaling of the spread, off while vol_multiplier is zero. The
    // volatility is the mid's typical one-minute move in basis points, worked
    // out from its moves over the last vol_window (sampled at most once a
    // second, and divided by the time they cover, so a gap in the feed does
    // not distort it). The half-spread is then the larger of half_spread_bps
    // and vol_multiplier times that, held to max_half_spread_bps when that is
    // set (zero: no cap). Until vol_window of history has been seen,
    // half_spread_bps alone is used.
    types::Decimal vol_multiplier{0, 0};
    std::chrono::milliseconds vol_window{std::chrono::minutes(5)};
    types::Decimal max_half_spread_bps{0, 0};
    // Centre the quotes between the mid and the microprice, the mid weighted
    // by the size at the touch: (bid * ask_size + ask * bid_size) /
    // (bid_size + ask_size). It sits nearer the side with less size, the side
    // a few orders can clear, so the quotes lean towards where the price is
    // likelier to go next. The centre is mid + microprice_weight * (microprice
    // - mid): zero (the default) is the mid, one the full microprice, which
    // tends to overstate the next move. A quote without both sizes uses the
    // mid.
    types::Decimal microprice_weight{0, 0};
    // Pull the quote that recent trades are running against: no bid while the
    // trades of the last flow_window were mostly sellers hitting the bid, no
    // ask while they were mostly buyers lifting the ask, so a quote is not
    // left in the way of a sweep. The imbalance is (bought - sold) / (bought
    // + sold), by size, over the trades the quotes carried (a change in the
    // last trade's price, size, side or time is a new trade); the bid is
    // pulled at -flow_pull_imbalance or below, the ask at +flow_pull_imbalance
    // or above, and comes back once the imbalance is inside that. A trade
    // without a side or size is not counted. Zero (the default) is off; at
    // most 1, which pulls only when every trade went one way.
    types::Decimal flow_pull_imbalance{0, 0};
    std::chrono::milliseconds flow_window{std::chrono::seconds(5)};
    // How far the position may stray from where it started, in base units,
    // either way. Past it the side that would add to the position stops
    // quoting.
    types::Decimal max_inventory;
    // How far the reservation price moves, in basis points, when the position
    // is max_inventory away from where it started (in proportion for less):
    // long, both quotes shift down so a sale is likelier than a purchase.
    types::Decimal skew_bps{0, 0};
    // Stronger inventory control, both off by default. With skew_power above
    // 1 the lean is the position's share of max_inventory raised to it
    // (keeping its sign): a small position barely leans the quotes and a
    // large one leans them hard, as an inventory penalty that grows with the
    // square of the position (Avellaneda-Stoikov) does at 2. With skew_age
    // the skew also grows with how long the position has been held on its
    // side (since it last reached half a quote's quantity there; falling
    // under that, or crossing zero, starts it again), to double at skew_age
    // and no further, so a position the market will not take back is leaned
    // off harder as it ages.
    types::Decimal skew_power{1, 0};
    std::chrono::milliseconds skew_age{0};
    // A resting quote is replaced only when its target price has moved by at
    // least this many basis points (and at least one tick), since each
    // replacement goes to the back of the queue.
    types::Decimal requote_bps{1, 0};
    // The same, for a market that has moved towards the resting quote: a bid
    // whose target is now below it, an ask whose target is above it. That is
    // the quote the market is about to fill at a stale price, and with order
    // latency it can be filled while its cancel is on the way, so it is worth
    // replacing sooner than a quote the market has moved away from, which only
    // costs queue position to replace. Zero replaces it as soon as the target
    // is a tick past it. Unset: requote_bps.
    std::optional<types::Decimal> requote_toward_bps;
    // Replace a quote older than this even if its price is still right;
    // zero never does.
    std::chrono::milliseconds max_quote_age{0};
    // Queue position. Each replacement joins the back of the queue at its
    // price, and on a pair whose spread is a tick or two wide the place in
    // the queue is most of what a quote earns. With keep_at_touch, a resting
    // quote still at the best price on its side (the best bid for our bid)
    // is not replaced because its target moved away from it: it keeps its
    // place. A market moving towards it still replaces it (requote_toward_bps),
    // as does the age or room rule. A move away is a target better than the
    // quote (a higher bid); the skew or a widened spread moves the target
    // the other way, towards the quote, and still replaces it.
    bool keep_at_touch = false;
    // Pull both quotes once no quote has arrived for this long, so a quote is
    // not left resting at a price the strategy can no longer check (a
    // stalled feed, a dropped connection); the next quote puts them back.
    // Checked on a timer (OnTimer), a quarter of this, from 100 ms to 1 s.
    // Zero (the default) never pulls. A feed that sends only changes is
    // silent for seconds in a quiet market, so this wants tens of seconds.
    std::chrono::milliseconds quiet_pull{0};
    // Quote this many levels a side, each of quantity: the first where a
    // single quote would be, each further one level_step_bps (of the mid,
    // at least a tick) behind the one before. The levels share
    // max_inventory's room, the touch's level first, so a deeper level is
    // quoted only while there is room left for it. One (the default) is a
    // single quote a side. Zero level_step_bps: the half-spread quoted
    // (widened by volatility, with vol_multiplier).
    std::size_t levels = 1;
    types::Decimal level_step_bps{0, 0};
    // How far into the spread a computed price may go. kCompute quotes the
    // computed price; kJoin holds a quote to the best price on its side
    // (never better), so a bid computed inside the spread joins the best bid
    // instead; kImprove holds it to one tick better than that, while the
    // spread is at least three ticks (else as kJoin). A computed price behind
    // the touch stays where it is. Our own quote at the touch is the touch:
    // it is not improved on, nor is the last price quoted on that side (a
    // book delayed past a fill can still show our quote there).
    enum class Touch { kCompute, kJoin, kImprove };
    Touch touch = Touch::kCompute;
    // Quote with post-only orders: a quote that would cross the book on
    // arrival is refused by the venue instead of filling as a taker, and the
    // next quote tries again.
    bool post_only = false;
    MarketMakerProtection protection;
};

// Quotes a bid and an ask around a reservation price: the mid (or a point
// towards the microprice, with microprice_weight), shifted away from the side
// the position is heavy on. Spot only, so the ask is quoted only while there
// is base asset to sell, and the strategy starts from whatever the venue
// holds of the base asset. With levels above 1, each side quotes that many,
// a step apart (see MarketMakerParams).
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
// With vol_multiplier set, the spread widens with the mid's recent
// volatility (see MarketMakerParams).
//
// With flow_pull_imbalance set, the side recent trades are running against
// is pulled (see MarketMakerParams).
//
// With quiet_pull set, both quotes are pulled when the feed goes quiet (see
// MarketMakerParams); the protection rules still run only when a quote
// arrives.
class MarketMaker : public Strategy {
   public:
    // Throws std::invalid_argument unless quantity, half_spread_bps and
    // max_inventory are positive (max_inventory at least quantity) and
    // skew_bps, requote_bps, requote_toward_bps, max_quote_age and
    // vol_multiplier are not negative; with vol_multiplier set, vol_window
    // must be positive, and a max_half_spread_bps that is set at least
    // half_spread_bps; microprice_weight and flow_pull_imbalance are from 0 to
    // 1, and with flow_pull_imbalance set flow_window must be positive. Each
    // enabled protection rule's amounts must be positive (the filters' and
    // the durations may be zero).
    explicit MarketMaker(MarketMakerParams params);

    std::string_view Name() const override { return "market_maker"; }
    Requirements Needs() const override;
    void OnStart(Context& context) override;
    void OnQuote(Context& context, const types::Quote& quote) override;
    void OnFill(Context& context, const types::Fill& fill) override;
    void OnOrderUpdate(Context& context,
                       const types::OrderStatus& status) override;
    void OnTimer(Context& context) override;
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
    // A resting quote is replaced once its target is `toward` or more on the
    // side the market came from (below a bid, above an ask), or `away` or
    // more on the other.
    // With keep_at_touch, a quote at `best` (the book's best price on its
    // side) is not replaced for a move away.
    void Manage(Context& context, Resting& quote, types::Side side,
                const std::optional<types::Decimal>& price,
                const types::Decimal& size, const types::Decimal& room,
                const types::Decimal& away, const types::Decimal& toward,
                const types::Decimal& best, bool top);
    // Holds a computed price to the touch, as MarketMakerParams::touch says.
    // `top`: the quote is the side's first level (its price is the one
    // kImprove remembers).
    void HoldToTouch(const types::Quote& quote, types::Decimal& bid,
                     types::Decimal& ask) const;
    // Cancels a resting quote; false if it cannot be replaced yet.
    bool Pull(Context& context, Resting& quote);
    types::Decimal Size(const types::Decimal& room) const;
    // Samples the mid for the volatility, and returns the half-spread to
    // quote, in basis points.
    types::Decimal HalfSpreadBps(types::Timestamp now,
                                 const types::Decimal& mid);
    // The point the quotes are centred on, before the inventory skew.
    types::Decimal Centre(const types::Quote& quote, const types::Decimal& mid);
    // Notes a trade the quote carries, if it is new, and drops those older
    // than flow_window.
    void NoteTrade(const types::Quote& quote, types::Timestamp now);

    // Protection. All run on the quote's mid and the book.
    struct Pause {
        bool bid = false;
        bool ask = false;
    };
    // Applies the rules enabled in mParams.protection and says which sides
    // must not quote.
    Pause Protect(Context& context, const types::Quote& quote,
                  const types::Decimal& mid);
    // Adds the sides the recent trades are running against to `pause`.
    void FlowPause(types::Timestamp now, Pause& pause);
    // Basis-point move of the mid over `window`; zero without that history.
    types::Decimal Move(types::Timestamp now, std::chrono::milliseconds window,
                        const types::Decimal& mid) const;
    // Cancels both quotes and sells (or buys back) the position at market.
    // False if it could not (a quote cannot be pulled yet, the order was
    // refused) or nothing needs doing. Does nothing while an exit is pending.
    bool ExitPosition(Context& context, const char* why);
    void Halt(const char* why);
    // Kept state (MarketMakerProtection::reset_state). LoadState is false
    // when a saved state cannot be read, and the maker then does not quote.
    bool KeepsState() const;
    bool LoadState(Context& context);
    void SaveState(Context& context, bool now);
    std::string StateKey() const { return "market_maker/" + mParams.symbol; }
    types::Decimal Position() const { return mInventory - mStart; }
    // The reservation price's shift for the position, in basis points of
    // the mid: skew_bps times the lean (skew_power) times the age factor.
    types::Decimal SkewBps(types::Timestamp now) const;
    // Notes when the position reached its side (skew_age).
    void NoteHeld(types::Timestamp now);

    // Fills the venue has reported in an order's state (an acknowledgement
    // that it filled at once, or its closing state) but whose OnFill has not
    // arrived yet. They count against the room on their side until it does,
    // so a quote that filled on arrival is not placed again and again before
    // the inventory catches up.
    struct Unconfirmed {
        types::Side side{};
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
    // The quotes on each side, the touch's level first (levels of them).
    std::vector<Resting> mBids;
    std::vector<Resting> mAsks;
    // Cancels every quote on both sides; false if any cannot be pulled yet.
    bool PullAll(Context& context);
    bool AnyResting() const;
    // The price of the last quote placed on each side, kept after it fills
    // or closes, for MarketMakerParams::Touch::kImprove.
    std::optional<types::Decimal> mLastBidPrice;
    std::optional<types::Decimal> mLastAskPrice;

    types::Symbol mBaseAsset;
    types::Symbol mQuoteAsset;
    types::Decimal mCash;             // quote asset from fills, net of fees
    types::Decimal mEntry;            // average entry price of Position()
    types::Decimal mPeak;             // highest P&L seen
    types::Timestamp mStandUntil;     // spike stand-down ends
    types::Timestamp mCooldownUntil;  // stop-out cooldown ends
    std::optional<Exit> mExit;
    int mHeldSide = 0;  // -1 short, 0 flat (under half a quote), 1 long
    types::Timestamp mHeldSince;
    bool mHalted = false;
    // Kept across restarts: the P&L of the runs before this one, the P&L
    // last measured (with it), whether something worth saving at once
    // changed, and when the state was last saved.
    types::Decimal mCarried;
    types::Decimal mLastPnl;
    bool mStateDirty = false;
    std::optional<types::Timestamp> mStateSaved;
    types::Timestamp mStateRetryAt;  // no save attempt before this
    types::Decimal mLastMark;        // the mid mLastPnl was marked at
    bool mStateWarned = false;
    std::size_t mWouldTake = 0;  // post-only quotes refused, for the log
    std::deque<std::pair<types::Timestamp, types::Decimal>> mMids;
    std::map<std::string, Unconfirmed> mUnconfirmed;  // by order ID

    // Volatility. Each sample is a move of the mid: when it started, when
    // it ended, and its square in basis points squared.
    struct VolSample {
        types::Timestamp start;
        types::Timestamp end;
        double squared_bps = 0;
    };
    std::optional<std::pair<types::Timestamp, types::Decimal>> mVolLast;
    std::optional<types::Timestamp> mVolFirst;
    std::deque<VolSample> mVolSamples;
    double mVolBps = 0;  // one-minute volatility; zero until the window fills
    // For the log at the end: the half-spreads quoted.
    std::size_t mHalfCount = 0;
    std::size_t mHalfWidened = 0;
    double mHalfSum = 0;
    types::Decimal mHalfMax;
    // For the log at the end: how far the microprice moved the centre.
    std::size_t mCentreCount = 0;
    double mCentreShiftSum = 0;  // absolute, in bps

    // Order flow: the trades of the last flow_window, and the last trade
    // seen, to tell a new one from a book update that repeats it.
    struct FlowTrade {
        types::Timestamp time;
        types::Decimal size;
        bool bought = false;  // a buyer took
    };
    struct LastTrade {
        std::optional<types::Decimal> price;
        std::optional<types::Decimal> size;
        std::optional<types::Side> taker_side;
        std::optional<types::Timestamp> time;
        bool operator==(const LastTrade&) const = default;
    };
    std::optional<LastTrade> mLastTrade;
    std::deque<FlowTrade> mFlow;
    types::Decimal mFlowBought;
    types::Decimal mFlowSold;
    // For the log at the end: trades not counted, and for each side (bid,
    // ask) how often and for how long the flow pulled it.
    std::size_t mFlowUncounted = 0;
    struct FlowPulls {
        bool on = false;
        std::size_t count = 0;
        types::Timestamp since;
        std::chrono::milliseconds total{0};
    };
    FlowPulls mFlowBid;
    FlowPulls mFlowAsk;
    std::optional<types::Timestamp> mFirstQuote;
    types::Timestamp mLastQuote;
    // When the last quote for the symbol arrived, for quiet_pull, and
    // whether the quotes were pulled for its silence.
    std::optional<types::Timestamp> mLastArrival;
    bool mQuietPulled = false;
    bool mQuietLogged = false;  // the pull was logged: log the return too
};

}  // namespace kestrel::strategy
