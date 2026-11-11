#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "kestrel/strategy/patterns.hpp"
#include "kestrel/strategy/strategy.hpp"
#include "kestrel/types/decimal.hpp"

namespace kestrel::strategy {

// The parameters are described in docs/strategies/breakout.md.
struct BreakoutParams {
    enum class Setup {
        kNarrowRange,
        kInsideBar,
        kRange,
        kRectangle,
        kPipeBottom,
        kFailedBreakdown,
        kMultipleBottom,  // a double or triple bottom: `bottoms`
        kOpeningRange,
        kInverseHeadShoulders,
        kTriangle,  // ascending or symmetrical: `triangle`
        kCupHandle,
    };
    enum class Confirm { kNone, kCloses, kPercent };
    enum class Entry { kStop, kThrowback };
    enum class Direction { kLong, kShort };

    types::Symbol symbol;
    types::Decimal quantity;  // base-asset units per entry
    types::Timeframe timeframe = types::Timeframe::kH1;
    Setup setup = Setup::kNarrowRange;
    Direction direction = Direction::kLong;
    bool reverse = false;
    std::size_t nr_bars = 4;
    std::size_t range_bars = 20;
    types::Decimal touch_zone{1, 1};  // 0.1 of the pattern's height
    std::size_t min_touches = 2;
    bool shortfall = false;
    std::size_t decline_bars = 10;
    std::size_t reclaim_bars = 3;
    std::size_t bottoms = 2;
    std::size_t swing_bars = 2;
    TriangleKind triangle = TriangleKind::kAscending;
    std::size_t opening_bars = 6;
    Confirm confirm = Confirm::kNone;
    std::size_t confirm_closes = 2;
    types::Decimal confirm_percent{5, 1};  // 0.5 (%)
    Entry entry = Entry::kStop;
    std::size_t throwback_bars = 3;
    types::Decimal stop_buffer_bps{10, 0};
    types::Decimal atr_stop;  // zero: the stop goes below the setup's low
    std::size_t atr_bars = 14;
    types::Decimal target_multiple{1, 0};  // zero: no target
    std::size_t arm_bars = 1;
    std::size_t max_hold_bars = 0;  // zero: never
    std::size_t trend_sma = 0;      // zero: no trend filter
    bool flatten_at_close = false;
    bool resting_target = false;
    types::Decimal volume_ratio;  // zero: no volume confirmation
    std::size_t volume_bars = 20;
    types::Decimal breakeven_r;  // zero: off
    std::size_t trail_bars = 0;  // zero: off
    // Zero: every entry buys `quantity`. Else at most this much to lose
    // between the entry and the stop, so `quantity` is a cap.
    types::Decimal risk_per_trade;
};

// Breakout of a bar setup, long or short; how it trades is described in
// docs/strategies/breakout.md.
//
// A short is the long side with every price negated: bars are mirrored on the
// way in (Bars, Mirror), orders on the way out (Place, PlaceStop) and fills
// on the way back (OnFill), so the setups and the state machine below are
// written once, in long terms. Offsets that scale with the price are written
// as p ± |p|·f rather than p·(1 ± f), so they move the same way either side
// of zero.
class Breakout : public Strategy {
   public:
    // Throws std::invalid_argument for parameters out of range.
    explicit Breakout(BreakoutParams params);

    std::string_view Name() const override { return "breakout"; }
    Requirements Needs() const override;
    void OnBar(Context& context, const types::Symbol& symbol,
               types::Timeframe timeframe, const types::Bar& bar) override;
    void OnFill(Context& context, const types::Fill& fill) override;
    void OnOrderUpdate(Context& context,
                       const types::OrderStatus& status) override;
    void OnSessionOpen(Context& context) override;
    void OnSessionClosing(Context& context, types::Timestamp close) override;

   private:
    // What a setup gives a trade.
    struct Levels {
        // Per setup: see docs/strategies/breakout.md#the-setups.
        types::Decimal trigger;  // one tick (or confirm_percent) above high
        types::Decimal stop;
        types::Decimal height;  // the target's distance from the entry
        types::Decimal high;    // the level broken; the throwback's limit
    };
    enum class State {
        kFlat,
        kArmed,     // an entry rests (mEntryId): a buy-stop, or the
                    // throwback's limit
        kWatching,  // kCloses or kThrowback: counting closes above
                    // mLevels->trigger
        kEntering,  // a market buy is out (mEntryId)
        kLong,      // holding; the protective stop rests (mStopId)
        kExiting,   // a market sell is out (mExitId)
        kPending,   // daily bars: an entry (mDue) waits for the next open
    };
    // The entry an armed or pending setup places.
    enum class Due {
        kStop,       // the buy-stop
        kMarket,     // a market buy after confirming closes
        kThrowback,  // the throwback's limit
    };

    std::optional<Levels> FindSetup(Context& context,
                                    std::span<const types::Bar> bars) const;
    void Arm(Context& context, const Levels& levels);
    // Daily bars: the entry `due` at the next open, for `bars` bars.
    void Pend(Due due, std::size_t bars);
    // Whether the timeframe's bars close after the market (a day or more),
    // so what they call for is done at the next open.
    bool CarriesOver() const;
    void ArmThrowback(Context& context);
    // The entry's size, buying at `entry` with the stop at `stop`: quantity,
    // or with risk_per_trade what loses that much at the stop, rounded down
    // to the lot. nullopt (logged) if that is under the venue's minimum.
    std::optional<types::Decimal> EntrySize(Context& context,
                                            const types::Decimal& entry,
                                            const types::Decimal& stop) const;
    // Closes above the trigger that confirm the breakout.
    std::size_t ClosesNeeded() const;
    void EnterAtMarket(Context& context);
    void PlaceStop(Context& context);
    // Long, on a close that is no exit: raises mLevels->stop as breakeven_r
    // and trail_bars say, and marks the stop to be moved.
    void RaiseStop(Context& context, const types::Bar& bar);
    // Moves the resting stop to mLevels->stop (cancel, then place).
    void MoveStop(Context& context);
    // volume_ratio: whether `bar`, the newest, traded enough.
    bool VolumeConfirms(Context& context, const types::Bar& bar) const;
    void Exit(Context& context, const char* why);
    // Cancels the entry: flat, or long if it had partly filled; still armed
    // if the cancel failed or the venue says it filled.
    void Disarm(Context& context);
    // On the entry's (whole or partial) fill: long mHeld, the stop placed.
    void GoLong(Context& context);
    // A bar in each state; whether the bar may also arm a new setup.
    bool ArmedBar(Context& context);
    bool WatchingBar(Context& context, const types::Bar& bar);
    bool PendingBar();
    void LongBar(Context& context, const types::Bar& bar);
    void LookForSetup(Context& context);
    // Counts a bar off mBarsLeft; whether none are left (zero already
    // counts: the last bar's work is being retried).
    bool CountDown();
    void GoFlat();
    // Cancels the order `id` and clears it; one not known to be closed is
    // kept as a stray.
    void CancelOther(Context& context, std::string& id, const char* what);
    void AddStray(const std::string& id);
    void RetryStrays(Context& context);
    const char* StateName() const;
    // Places an order; the ID if the venue took it and it is still active.
    // `request` is in the trade's terms: mirrored for a short.
    std::optional<std::string> Place(Context& context,
                                     types::OrderRequest request,
                                     const char* what);
    // A price between the trade's terms and the market's (negated for a
    // short; its own inverse).
    types::Decimal Mirror(const types::Decimal& price) const {
        return mShort ? -price : price;
    }
    std::string Price(const types::Decimal& price) const {
        return Mirror(price).ToString();
    }
    types::Bar Mirror(const types::Bar& bar) const;
    // The symbol's bars, in the trade's terms.
    std::span<const types::Bar> Bars(Context& context) const;
    // reverse: after the stop closed the trade with `failed` levels, the
    // other way at market.
    void Reverse(Context& context, const Levels& failed);

    BreakoutParams mParams;
    State mState = State::kFlat;
    // The trade, or while flat the setup looked for, is short: prices are
    // mirrored, and buys are sells.
    bool mShort = false;
    bool mReversal = false;     // this trade reverses a failed one
    types::Decimal mFirstStop;  // kLong: the stop as first placed
    // Orders given up on whose cancel is not known to have landed: a failed
    // cancel, one that crossed a fill, or the orders of a trade just
    // reversed. Their cancels are retried each bar until the venue answers,
    // and kept a bar longer for a fill still on its way. A stray of an
    // earlier trade is not this trade's: its fill is logged and not counted
    // (one of this trade's still sells what this trade holds).
    struct Stray {
        std::string id;
        std::size_t trade = 0;  // mTrade when it was given up
        std::size_t tries = 0;
        bool answered = false;
    };
    std::vector<Stray> mStrays;
    std::size_t mTrade = 0;  // counts trades: GoFlat starts the next
    // Bars(): a short's mirrored bars.
    mutable std::vector<types::Bar> mMirrored;
    std::optional<Levels> mLevels;  // the setup being traded
    std::size_t mBarsLeft = 0;      // kArmed / kWatching: bars still allowed
    std::size_t mCloses = 0;        // kWatching: closes above so far
    std::size_t mBarsHeld = 0;      // kLong: whole bars since the entry
    types::Timestamp mEntryTime;    // kLong: when the entry first filled
    types::Decimal mEntryPrice;     // the entry's first fill price
    types::Decimal mRisk;           // kLong: the entry less the first stop
    bool mStopMoved = false;  // kLong: mLevels->stop is above the resting stop
    types::Decimal mHeld;           // base asset bought and not yet sold
    types::Decimal mEntryFilled;    // gross entry quantity filled so far
    types::Decimal mEntryQuantity;  // what the entry out (or last) buys
    std::string mBaseAsset;         // base asset of mParams.symbol
    bool mBaseFeePaid = false;      // whether a buy paid a base-asset fee
    std::optional<types::Decimal> mTarget;
    Due mDue = Due::kStop;
    // kLong, daily bars: an exit called for while closed, made at the open.
    const char* mExitDue = nullptr;
    std::string mEntryId;
    std::string mStopId;
    std::string mExitId;
    // resting_target: the target's limit, paired with mStopId.
    std::string mTargetId;
    bool mTargetHit = false;      // the target has sold some of the position
    bool mTargetRefused = false;  // refused this trade: the close checks it
    bool mNoOco = false;          // the venue has no one-cancels-other
};

}  // namespace kestrel::strategy
