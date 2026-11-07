#pragma once

#include <cstddef>
#include <optional>
#include <string>

#include "kestrel/strategy/strategy.hpp"
#include "kestrel/types/decimal.hpp"

namespace kestrel::strategy {

struct BreakoutParams {
    enum class Setup {
        kNarrowRange,  // the newest bar narrower than the nr_bars - 1 before
        kInsideBar,    // the newest bar inside the one before
        kRange,        // the high and low of the last range_bars bars
        kRectangle,    // kRange, touched min_touches times at each bound
        kPipeBottom,   // a two-bar reversal after decline_bars bars
        kFailedBreakdown,  // a close back above a broken range_bars low
        kMultipleBottom,   // a double or triple bottom (bottoms) in
                           // range_bars bars
        kOpeningRange,     // the session's first opening_bars bars
    };
    enum class Confirm {
        kNone,     // a buy-stop just above the setup's high
        kCloses,   // a market buy after confirm_closes closes above it
        kPercent,  // a buy-stop confirm_percent above the setup's high
    };
    enum class Entry {
        kStop,       // as confirm says: a buy-stop, or a market buy
        kThrowback,  // after a close above the trigger, a limit at the high
    };

    types::Symbol symbol;
    types::Decimal quantity;  // base-asset units per entry
    types::Timeframe timeframe = types::Timeframe::kH1;
    Setup setup = Setup::kNarrowRange;
    std::size_t nr_bars = 4;      // kNarrowRange: 4 is NR4
    std::size_t range_bars = 20;  // kRange and kRectangle: the window
    // kRectangle: a bar touches a bound within this fraction of the height
    // of it; runs of touching bars count once.
    types::Decimal touch_zone{1, 1};  // 0.1
    std::size_t min_touches = 2;      // of each bound
    // kRectangle: also require the deck's shortfall (since the last top
    // touch, a fall below the middle that turned short of the bottom).
    bool shortfall = false;
    // kPipeBottom: the bars the first bar's low must undercut, and whose
    // average range both bars must exceed.
    std::size_t decline_bars = 10;
    // kFailedBreakdown: bars the close back above the low may come within,
    // after the first close below it.
    std::size_t reclaim_bars = 3;
    // kMultipleBottom: 2 (double) or 3 (triple) swing lows within touch_zone
    // of the height of each other; swing_bars is how many bars on each side
    // a swing low must beat.
    std::size_t bottoms = 2;
    std::size_t swing_bars = 2;
    // kOpeningRange: the bars of the session's opening range (6 five-minute
    // bars: the first half hour). Intraday bars only.
    std::size_t opening_bars = 6;
    Confirm confirm = Confirm::kNone;
    std::size_t confirm_closes = 2;
    types::Decimal confirm_percent{5, 1};  // 0.5 (%)
    Entry entry = Entry::kStop;
    // kThrowback: bars the limit rests at the setup's high after the
    // breakout's confirming close.
    std::size_t throwback_bars = 3;
    // The protective stop: this far below the setup's low.
    types::Decimal stop_buffer_bps{10, 0};
    // The target: the entry plus this many times the setup's height; zero
    // for none.
    types::Decimal target_multiple{1, 0};
    // Bars an untriggered entry stays armed (kCloses: the closes must start
    // within this many bars).
    std::size_t arm_bars = 1;
    // Exit at market after this many whole bars held (not counting the bar
    // the entry filled inside); zero for never.
    std::size_t max_hold_bars = 0;
    // Arm only while the close is above this many bars' average close; zero
    // turns the filter off.
    std::size_t trend_sma = 0;
    // On a market that closes: sell in OnSessionClosing.
    bool flatten_at_close = false;
};

// Long-only breakout of a bar setup, after Fidelity's "Identifying Chart
// Patterns" (Kirkpatrick): wait for a setup (an NR4, an inside bar, the last
// N bars' range, a rectangle: that range when the price has tested both its
// support and its resistance, see RectangleOf, a pipe bottom, see
// PipeBottomOf, a failed breakdown, see FailedBreakdownOf, a double or
// triple bottom, see MultipleBottomOf, or the opening range: the session's
// first opening_bars bars, see OpeningRangeOf), enter on a break
// above its high, protect with a stop below its low, and take profit at the
// setup's height above the entry.
//
// Flat: on each closed bar, a setup (and, with trend_sma, a close above the
// average) arms an entry: a buy-stop one tick above the setup's high (raised
// by confirm_percent with kPercent), or with kCloses a watch that buys at
// market once confirm_closes closes in a row are above it. An entry not
// triggered within arm_bars is cancelled, and the next setup can arm.
//
// With entry kThrowback, the breakout is not bought but waited for: a close
// above the trigger (confirm_closes of them with kCloses; above the raised
// trigger with kPercent) within arm_bars, and then a post-only limit buy at the
// setup's high, the broken level, resting for throwback_bars bars. It fills
// only if the price comes back to that level, and then as a maker (no
// slippage, and on Binance.US no fee), from where the stop and target are the
// same distance as before. A breakout that runs without looking back is
// missed: the deck's caveat that throwbacks do not always occur.
//
// Long: on the entry's fill, a sell-stop rests at the setup's low less
// stop_buffer_bps. The target is checked on closes: a bar closing at or above
// it sells at market (the stop is cancelled first; one order cannot hold the
// shares for both, and the venue has no one-cancels-other). So does the
// max_hold_bars-th bar held, and with flatten_at_close the session's closing
// hook. A downward break is only ever an exit: there is no shorting.
//
// On a market that closes, intraday bars: nothing is armed while it is
// closed, and an armed entry is cancelled in OnSessionClosing, so a setup does
// not carry into the next session's open; a target or time exit due on a bar
// that closes with the market (the 15:30 bar, at 16:00) waits for the next
// bar, the stop resting meanwhile. Daily (and longer) bars all close after
// the market, so there a setup is pending until the next open, where the
// entry is placed (a buy-stop the open gaps through fills there); one cancelled
// at a close with bars of arm_bars left is placed again at the next open; and
// a target or time exit is sold at the next open. Orders are tracked through
// OnFill and OnOrderUpdate: an entry that closes unfilled returns to flat, one
// that closes partly filled is held as a long of what it bought, and a
// protective stop that closes unfilled, or could not be placed, is placed again
// (at the next bar). An entry whose cancel fails stays armed, and the cancel is
// retried each bar. When the stop or the exit sells out the position, the other
// is cancelled.
//
// A pipe bottom triggers above its second bar's high (the deck's "breakout
// through second bar"); its stop is below the lower of its two lows, and its
// height spans both bars. A failed breakdown triggers above the bar that
// closed back inside the range, its stop is below the false low, and its height
// is the range's. A double or triple bottom triggers above its neckline, with
// the stop below its lowest low and its height from the one to the other.
//
// The opening range is a setup once a session, on the close of its last bar.
// The session is the one Context::CurrentOrNextSession reports (so with
// extended hours it starts before the regular open), and on a market that
// never closes it is the UTC day. The range is the first opening_bars bars to
// open at or after the open, counted rather than timed: a bar straddling the
// open (hourly bars on a 09:30 open) is skipped, so the range starts at 10:00,
// and a bar missing from the window delays the setup to the next one. A run
// started after a session's range has formed waits for the next session. Its
// entry lasts arm_bars bars like any
// other, so for the usual opening range breakout set arm_bars to the rest of
// the session (it is cancelled at the close anyway) and flatten_at_close.
//
// Two adaptations of the deck: every setup, the inside bar included, triggers
// one tick above its high (the deck buys an inside bar on an open above its
// close), and the target is measured from the entry's fill rather than the
// breakout price, so an open gapping through the trigger raises it.
class Breakout : public Strategy {
   public:
    // Throws std::invalid_argument for a quantity that is not positive, an
    // nr_bars under 2, a negative buffer, percent or multiple, zero
    // arm_bars, confirm_closes, throwback_bars, min_touches, decline_bars,
    // reclaim_bars, swing_bars or opening_bars, bottoms other than 2 or 3, a
    // touch_zone outside (0, 0.5), with kMultipleBottom a range_bars too few to
    // hold the pattern (MultipleBottomBars), or kOpeningRange on bars of a day
    // or longer.
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
        types::Decimal trigger;  // buy above this
        types::Decimal stop;     // protective stop
        types::Decimal height;   // the setup's high minus its low
        // The level broken: the setup's high (a pipe bottom's second bar's
        // high); the throwback's limit.
        types::Decimal high;
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
    // Closes above the trigger that confirm the breakout.
    std::size_t ClosesNeeded() const;
    void EnterAtMarket(Context& context);
    void PlaceStop(Context& context);
    void Exit(Context& context, const char* why);
    // Cancels the entry: flat, or long if it had partly filled; still armed
    // if the cancel failed or the venue says it filled.
    void Disarm(Context& context);
    // On the entry's (whole or partial) fill: long mHeld, the stop placed.
    void GoLong(Context& context);
    void GoFlat();
    // Best effort: cancels the order `id` and clears it.
    void CancelOther(Context& context, std::string& id, const char* what);
    const char* StateName() const;
    // Places an order; the ID if the venue took it and it is still active.
    std::optional<std::string> Place(Context& context,
                                     const types::OrderRequest& request,
                                     const char* what);

    BreakoutParams mParams;
    State mState = State::kFlat;
    std::optional<Levels> mLevels;  // the setup being traded
    std::size_t mBarsLeft = 0;      // kArmed / kWatching: bars still allowed
    std::size_t mCloses = 0;        // kWatching: closes above so far
    std::size_t mBarsHeld = 0;      // kLong: whole bars since the entry
    types::Timestamp mEntryTime;    // kLong: when the entry first filled
    types::Decimal mEntryPrice;     // the entry's first fill price
    types::Decimal mHeld;           // base asset bought and not yet sold
    std::optional<types::Decimal> mTarget;
    Due mDue = Due::kStop;
    // kLong, daily bars: an exit called for while closed, made at the open.
    const char* mExitDue = nullptr;
    std::string mEntryId;
    std::string mStopId;
    std::string mExitId;
};

}  // namespace kestrel::strategy
