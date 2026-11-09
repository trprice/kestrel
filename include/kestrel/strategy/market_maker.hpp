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

// The drawdown protection rules, each off until its flag is set; described
// in docs/strategies/market-maker.md#protection-rules.
struct MarketMakerProtection {
    bool stop_loss = false;
    types::Decimal stop_loss_bps{40, 0};

    bool loss_cap = false;
    types::Decimal loss_cap_amount;  // quote asset; required when enabled

    bool drawdown_cap = false;
    types::Decimal drawdown_cap_amount;  // quote asset; required when enabled

    bool spike_standdown = false;
    types::Decimal spike_3m_bps{50, 0};
    types::Decimal spike_5m_bps{80, 0};
    types::Decimal spike_15m_bps{120, 0};
    std::chrono::milliseconds spike_pause{std::chrono::minutes(15)};

    bool downtrend_filter = false;
    types::Decimal downtrend_bps{30, 0};

    bool momentum_filter = false;
    types::Decimal momentum_bps{0, 0};

    bool cooldown_after_stop = false;  // needs stop_loss
    std::chrono::milliseconds cooldown{std::chrono::minutes(30)};

    // Forget the state kept across restarts (Context::State) and start
    // afresh.
    bool reset_state = false;
};

// The parameters are described in docs/strategies/market-maker.md.
struct MarketMakerParams {
    types::Symbol symbol;
    types::Decimal quantity;  // base-asset units per quote
    types::Decimal half_spread_bps;
    types::Decimal vol_multiplier{0, 0};  // zero: a fixed spread
    std::chrono::milliseconds vol_window{std::chrono::minutes(5)};
    types::Decimal max_half_spread_bps{0, 0};  // zero: no cap
    types::Decimal microprice_weight{0, 0};    // zero: centred on the mid
    types::Decimal flow_pull_imbalance{0, 0};  // zero: off
    std::chrono::milliseconds flow_window{std::chrono::seconds(5)};
    types::Decimal max_inventory;  // base units, either way from the start
    types::Decimal skew_bps{0, 0};
    types::Decimal skew_power{1, 0};
    std::chrono::milliseconds skew_age{0};  // zero: off
    types::Decimal requote_bps{1, 0};
    std::optional<types::Decimal> requote_toward_bps;  // unset: requote_bps
    std::chrono::milliseconds max_quote_age{0};        // zero: never
    bool keep_at_touch = false;
    std::chrono::milliseconds quiet_pull{0};  // zero: never
    std::size_t levels = 1;
    types::Decimal level_step_bps{0, 0};  // zero: the half-spread quoted
    enum class Touch { kCompute, kJoin, kImprove };
    Touch touch = Touch::kCompute;
    bool post_only = false;
    MarketMakerProtection protection;
};

// Quotes a bid and an ask around a reservation price; how it quotes and
// protects itself is described in docs/strategies/market-maker.md.
//
// The inventory is moved only by fills (OnFill), and a side whose cancel lost
// a race with a fill waits for the order's final state before quoting again,
// so a fill is never counted twice. Fills a venue has reported in an order's
// state but not yet delivered count against the room on their side
// (Unconfirmed), or a quote that filled on arrival would be placed again and
// again before the inventory caught up.
class MarketMaker : public Strategy {
   public:
    // Throws std::invalid_argument for parameters out of range.
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
        types::Decimal price;  // the order's, for a fill booked unseen
        std::optional<types::Timestamp> checked;  // last asked of the venue
        bool lookup_warned = false;
    };
    void ExpectFills(const std::string& order_id, types::Side side,
                     const types::Decimal& filled, const types::Decimal& seen,
                     types::Timestamp now, const types::Decimal& price);
    // Asks the venue about fills unconfirmed for a while: corrects what an
    // order filled to what the venue says, and books one that has not
    // arrived long after the runner would have delivered it (see
    // ReconcileUnconfirmed in the .cpp).
    void ReconcileUnconfirmed(Context& context);
    // The bookkeeping of a fill, for OnFill and for one booked unseen.
    void ApplyFill(Context& context, const types::Fill& fill);
    // Fills booked unseen, by order ID, and how much of them: a copy that
    // arrives later after all is not counted again.
    struct BookedUnseen {
        types::Decimal quantity;
        types::Timestamp at;
    };
    std::map<std::string, BookedUnseen> mBookedUnseen;
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
