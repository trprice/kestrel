#include "kestrel/strategy/breakout.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "kestrel/strategy/orders.hpp"
#include "kestrel/strategy/patterns.hpp"
#include "kestrel/types/timeframe.hpp"

namespace kestrel::strategy {

using types::Decimal;
using types::OrderState;
using types::Rounding;
using types::Side;

Breakout::Breakout(BreakoutParams params)
    : mParams(std::move(params)),
      mShort(mParams.direction == BreakoutParams::Direction::kShort),
      mEntryQuantity(mParams.quantity) {
    mBaseAsset = BaseAsset(mParams.symbol);
    if (mParams.quantity.Sign() <= 0) {
        throw std::invalid_argument("quantity must be positive");
    }
    if (mParams.nr_bars < 2) {
        throw std::invalid_argument("nr_bars must be at least 2");
    }
    if (mParams.range_bars == 0 || mParams.arm_bars == 0 ||
        mParams.confirm_closes == 0 || mParams.throwback_bars == 0) {
        throw std::invalid_argument(
            "range_bars, arm_bars, confirm_closes and throwback_bars must be "
            "positive");
    }
    if (mParams.min_touches == 0 || mParams.decline_bars == 0 ||
        mParams.reclaim_bars == 0 || mParams.swing_bars == 0 ||
        mParams.opening_bars == 0) {
        throw std::invalid_argument(
            "min_touches, decline_bars, reclaim_bars, swing_bars and "
            "opening_bars must be positive");
    }
    if (mParams.setup == BreakoutParams::Setup::kOpeningRange &&
        CarriesOver()) {
        throw std::invalid_argument(
            "the opening_range setup needs intraday bars");
    }
    if (mParams.setup == BreakoutParams::Setup::kTriangle &&
        mParams.min_touches < 2) {
        throw std::invalid_argument(
            "a triangle needs min_touches of 2 or more");
    }
    if (mParams.bottoms != 2 && mParams.bottoms != 3) {
        throw std::invalid_argument("bottoms must be 2 or 3");
    }
    if ((mParams.setup == BreakoutParams::Setup::kMultipleBottom &&
         mParams.range_bars <
             MultipleBottomBars(mParams.bottoms, mParams.swing_bars)) ||
        (mParams.setup == BreakoutParams::Setup::kInverseHeadShoulders &&
         mParams.range_bars < MultipleBottomBars(3, mParams.swing_bars)) ||
        (mParams.setup == BreakoutParams::Setup::kCupHandle &&
         mParams.range_bars < CupHandleBars(mParams.swing_bars)) ||
        (mParams.setup == BreakoutParams::Setup::kTriangle &&
         mParams.range_bars <
             TriangleBars(mParams.min_touches, mParams.swing_bars))) {
        throw std::invalid_argument(
            "range_bars is too few to hold the pattern at this swing_bars");
    }
    const Decimal half(5, 1);  // 0.5
    if (mParams.touch_zone.Sign() <= 0 || mParams.touch_zone >= half) {
        throw std::invalid_argument("touch_zone must be above 0 and below 0.5");
    }
    if (mParams.stop_buffer_bps.IsNegative() ||
        mParams.confirm_percent.IsNegative() ||
        mParams.target_multiple.IsNegative() ||
        mParams.volume_ratio.IsNegative() || mParams.breakeven_r.IsNegative() ||
        mParams.atr_stop.IsNegative() || mParams.risk_per_trade.IsNegative()) {
        throw std::invalid_argument(
            "stop_buffer_bps, confirm_percent, target_multiple, volume_ratio, "
            "breakeven_r, atr_stop and risk_per_trade cannot be negative");
    }
    if (mParams.volume_bars == 0 || mParams.atr_bars == 0) {
        throw std::invalid_argument(
            "volume_bars and atr_bars must be positive");
    }
    if (mParams.volume_ratio.Sign() > 0 &&
        mParams.confirm != BreakoutParams::Confirm::kCloses &&
        mParams.entry != BreakoutParams::Entry::kThrowback) {
        throw std::invalid_argument(
            "volume_ratio needs confirm = closes or entry = throwback: a "
            "buy-stop fills before its bar's volume is known");
    }
}

Requirements Breakout::Needs() const {
    Requirements needs;
    needs.symbols = {mParams.symbol};
    needs.bar_timeframes = {mParams.timeframe};
    std::size_t setup_bars = 2;
    if (mParams.setup == BreakoutParams::Setup::kNarrowRange) {
        setup_bars = mParams.nr_bars;
    } else if (mParams.setup == BreakoutParams::Setup::kRange ||
               mParams.setup == BreakoutParams::Setup::kRectangle ||
               mParams.setup == BreakoutParams::Setup::kMultipleBottom ||
               mParams.setup == BreakoutParams::Setup::kInverseHeadShoulders ||
               mParams.setup == BreakoutParams::Setup::kTriangle ||
               mParams.setup == BreakoutParams::Setup::kCupHandle) {
        setup_bars = mParams.range_bars;
    } else if (mParams.setup == BreakoutParams::Setup::kPipeBottom) {
        setup_bars = mParams.decline_bars + 2;
    } else if (mParams.setup == BreakoutParams::Setup::kFailedBreakdown) {
        setup_bars = mParams.range_bars + mParams.reclaim_bars + 1;
    } else if (mParams.setup == BreakoutParams::Setup::kOpeningRange) {
        setup_bars = mParams.opening_bars + 1;  // and the bar before the open
    }
    needs.warmup_bars =
        std::max({setup_bars, mParams.trend_sma, mParams.trail_bars,
                  mParams.volume_ratio.Sign() > 0 ? mParams.volume_bars + 1
                                                  : std::size_t{0},
                  // Atr is smoothed over the whole window; Wilder's alpha of
                  // 1/n leaves the seed under 1% after six periods.
                  mParams.atr_stop.Sign() > 0 ? 6 * mParams.atr_bars + 1
                                              : std::size_t{0}});
    return needs;
}

std::optional<Breakout::Levels> Breakout::FindSetup(
    Context& context, std::span<const types::Bar> bars) const {
    std::optional<PriceRange> range;
    std::optional<Decimal> breakout;  // the level to break; else range->high
    std::optional<Decimal> floor;     // the stop's base; else range->low
    switch (mParams.setup) {
        case BreakoutParams::Setup::kNarrowRange:
            if (IsNarrowRange(bars, mParams.nr_bars)) {
                range = PriceRange{bars.back().high, bars.back().low};
            }
            break;
        case BreakoutParams::Setup::kInsideBar:
            if (IsInsideBar(bars)) {
                range = PriceRange{bars.back().high, bars.back().low};
            }
            break;
        case BreakoutParams::Setup::kRange:
            range = RangeOf(bars, mParams.range_bars);
            break;
        case BreakoutParams::Setup::kRectangle:
            if (const auto rect =
                    RectangleOf(bars, mParams.range_bars, mParams.touch_zone);
                rect && rect->top_touches >= mParams.min_touches &&
                rect->bottom_touches >= mParams.min_touches &&
                (!mParams.shortfall || rect->shortfall_up)) {
                range = rect->range;
            }
            break;
        case BreakoutParams::Setup::kPipeBottom:
            if (const auto pipe = PipeBottomOf(bars, mParams.decline_bars)) {
                range = pipe->range;
                breakout = pipe->breakout;
            }
            break;
        case BreakoutParams::Setup::kFailedBreakdown:
            if (const auto failed = FailedBreakdownOf(bars, mParams.range_bars,
                                                      mParams.reclaim_bars)) {
                range = failed->range;
                breakout = failed->reclaim_high;
                floor = failed->false_low;
            }
            break;
        case BreakoutParams::Setup::kMultipleBottom:
            if (const auto bottom =
                    MultipleBottomOf(bars, mParams.range_bars, mParams.bottoms,
                                     mParams.swing_bars, mParams.touch_zone)) {
                range = bottom->range;
            }
            break;
        case BreakoutParams::Setup::kInverseHeadShoulders:
            if (const auto hs = InverseHeadShouldersOf(bars, mParams.range_bars,
                                                       mParams.swing_bars,
                                                       mParams.touch_zone)) {
                range = hs->range;
                breakout = hs->breakout;
            }
            break;
        case BreakoutParams::Setup::kTriangle:
            if (const auto triangle =
                    TriangleOf(bars, mParams.range_bars, mParams.triangle,
                               mParams.min_touches, mParams.swing_bars,
                               mParams.touch_zone)) {
                range = triangle->range;
                breakout = triangle->breakout;
                floor = triangle->last_low;
            }
            break;
        case BreakoutParams::Setup::kCupHandle:
            if (const auto cup =
                    CupHandleOf(bars, mParams.range_bars, mParams.swing_bars,
                                mParams.touch_zone)) {
                range = cup->range;
                floor = cup->handle_low;
            }
            break;
        case BreakoutParams::Setup::kOpeningRange: {
            // Called only while the market is open (intraday bars), so a
            // session here is the current one.
            const auto session = context.CurrentOrNextSession();
            const types::Timestamp start =
                session ? session->start
                        : std::chrono::floor<std::chrono::days>(
                              bars.back().open_time);
            range = OpeningRangeOf(bars, mParams.opening_bars, start);
            break;
        }
    }
    if (!range || range->Height().Sign() <= 0) {
        return std::nullopt;  // no setup, or a flat one with no height
    }
    if (mParams.trend_sma > 0) {
        const auto average = Sma(bars, mParams.trend_sma);
        if (!average || bars.back().close <= *average) {
            return std::nullopt;
        }
    }
    auto info = context.SymbolInfo(mParams.symbol);
    if (!info) {
        spdlog::warn("breakout: no symbol info for {}: {}", mParams.symbol,
                     info.error().ToString());
        return std::nullopt;
    }
    const Decimal& tick = info->tick_size;
    Levels levels;
    levels.height = range->Height();
    // On the tick already, except a sloping neckline.
    const Decimal level =
        breakout.value_or(range->high).RoundToIncrement(tick, Rounding::kUp);
    levels.high = level;
    levels.trigger = level + tick;
    if (mParams.confirm == BreakoutParams::Confirm::kPercent) {
        const Decimal raise =
            Decimal::Divide(mParams.confirm_percent, Decimal::FromInt(100), 8,
                            Rounding::kNearest);
        levels.trigger = std::max(levels.trigger,
                                  (level + level.Abs() * raise)
                                      .RoundToIncrement(tick, Rounding::kUp));
    }
    const Decimal buffer = types::FromBps(mParams.stop_buffer_bps);
    const Decimal base = floor.value_or(range->low);
    levels.stop =
        (base - base.Abs() * buffer).RoundToIncrement(tick, Rounding::kDown);
    if (mParams.atr_stop.Sign() > 0) {
        const auto atr = Atr(bars, mParams.atr_bars);
        if (!atr || atr->Sign() <= 0) {
            spdlog::debug(
                "breakout: {} setup not armed: no average true range over {} "
                "bars ({} loaded)",
                mParams.symbol, mParams.atr_bars, bars.size());
            return std::nullopt;
        }
        levels.stop = (level - mParams.atr_stop * *atr)
                          .RoundToIncrement(tick, Rounding::kDown);
        if (Mirror(levels.stop).Sign() <= 0) {
            return std::nullopt;  // the ATR is wider than the price
        }
    }
    return levels;
}

void Breakout::OnBar(Context& context, const types::Symbol& symbol,
                     types::Timeframe timeframe, const types::Bar& raw) {
    if (symbol != mParams.symbol || timeframe != mParams.timeframe) {
        return;
    }
    RetryStrays(context);
    const types::Bar bar = Mirror(raw);
    // Each state's handler says whether this bar, having ended what that
    // state was waiting for, may also arm a new setup.
    bool look = false;
    switch (mState) {
        case State::kFlat:
            look = true;
            break;
        case State::kArmed:
            look = ArmedBar(context);
            break;
        case State::kWatching:
            look = WatchingBar(context, bar);
            break;
        case State::kPending:
            look = PendingBar();
            break;
        case State::kLong:
            LongBar(context, bar);
            break;
        case State::kEntering:
        case State::kExiting:
            // Waiting on a market order: its fill or final update moves on.
            // (An exit placed just before a close that has not filled by it
            // waits here unprotected, its stop cancelled, until then.)
            break;
    }
    if (look) {
        LookForSetup(context);
    }
}

bool Breakout::CountDown() {
    if (mBarsLeft > 0) {
        --mBarsLeft;
    }
    return mBarsLeft == 0;
}

bool Breakout::ArmedBar(Context& context) {
    // Zero already: an earlier cancel failed, or the venue said the entry
    // filled and its fill is still on the way. Try again.
    if (!CountDown()) {
        return false;
    }
    Disarm(context);
    // Still armed if it filled (perhaps partly) or could not be cancelled.
    return mState == State::kFlat;
}

bool Breakout::WatchingBar(Context& context, const types::Bar& bar) {
    if (bar.close > mLevels->trigger &&
        (mCloses > 0 || VolumeConfirms(context, bar))) {
        if (++mCloses >= ClosesNeeded()) {
            const bool throwback =
                mParams.entry == BreakoutParams::Entry::kThrowback;
            if (!context.MarketOpen() && CarriesOver()) {
                Pend(throwback ? Due::kThrowback : Due::kMarket,
                     throwback ? mParams.throwback_bars : 1);
                return false;
            }
            if (throwback) {
                ArmThrowback(context);
            } else {
                EnterAtMarket(context);
            }
            if (mState != State::kWatching) {
                // Entered, or the throwback was refused: not a new setup on
                // the bar that confirmed this one.
                return false;
            }
        }
    } else {
        mCloses = 0;
    }
    if (!CountDown()) {
        return false;
    }
    mState = State::kFlat;
    mLevels.reset();
    return true;
}

bool Breakout::PendingBar() {
    // Normally placed at the open before another bar closes.
    if (!CountDown()) {
        return false;
    }
    GoFlat();
    return true;
}

void Breakout::LongBar(Context& context, const types::Bar& bar) {
    // Whole bars only: not the one the entry filled inside.
    if (bar.open_time >= mEntryTime) {
        ++mBarsHeld;
    }
    const char* due = nullptr;
    if (mTarget && mTargetId.empty() && bar.close >= *mTarget) {
        due = "target";  // not resting at the venue
    } else if (mParams.max_hold_bars > 0 &&
               mBarsHeld >= mParams.max_hold_bars) {
        due = "held long enough";
    }
    if (!due) {
        RaiseStop(context, bar);
    }
    // Orders are refused while closed: the resting stop protects until the
    // next bar in the session, or with daily bars until the open, where a due
    // exit sells.
    if (!context.MarketOpen()) {
        if (due && CarriesOver()) {
            mExitDue = due;
        }
        return;
    }
    if (mStopId.empty()) {
        // An earlier attempt failed. A (raised) stop the price has already
        // fallen through would be refused, or sell at once: sell now.
        if (!due && bar.close <= mLevels->stop) {
            due = "closed through its stop";
        } else if (!due) {
            PlaceStop(context);
        }
    } else if (mStopMoved && !due) {
        MoveStop(context);
    }
    if (due) {
        Exit(context, due);
    }
}

void Breakout::LookForSetup(Context& context) {
    // Not on intraday bars while the market is closed: an order then is
    // refused, and an entry must not rest into the next open. Daily bars
    // always close after it: the entry waits for the open.
    const bool open = context.MarketOpen();
    if (!open && !CarriesOver()) {
        return;
    }
    const auto levels = FindSetup(context, Bars(context));
    if (!levels) {
        return;
    }
    if (mParams.confirm == BreakoutParams::Confirm::kCloses ||
        mParams.entry == BreakoutParams::Entry::kThrowback) {
        mLevels = levels;
        mState = State::kWatching;
        mCloses = 0;
        // The closes must start within arm_bars bars.
        mBarsLeft = mParams.arm_bars + ClosesNeeded() - 1;
        return;
    }
    if (open) {
        Arm(context, *levels);
    } else {
        mLevels = levels;
        Pend(Due::kStop, mParams.arm_bars);
    }
}

void Breakout::Pend(Due due, std::size_t bars) {
    mDue = due;
    mState = State::kPending;
    mBarsLeft = bars;
}

bool Breakout::CarriesOver() const {
    const auto bar = types::FixedDuration(mParams.timeframe);
    return !bar || *bar >= std::chrono::hours(24);
}

std::optional<Decimal> Breakout::EntrySize(Context& context,
                                           const Decimal& entry,
                                           const Decimal& stop) const {
    if (mParams.risk_per_trade.Sign() == 0) {
        return mParams.quantity;
    }
    const Decimal distance = entry - stop;  // in long terms, so above zero
    if (distance.Sign() <= 0) {
        spdlog::warn("breakout: {} stop {} is not below the entry {}; no entry",
                     mParams.symbol, Price(stop), Price(entry));
        return std::nullopt;
    }
    auto info = context.SymbolInfo(mParams.symbol);
    if (!info) {
        spdlog::warn("breakout: cannot size the entry in {}: {}",
                     mParams.symbol, info.error().ToString());
        return std::nullopt;
    }
    // quantity caps the size; below it, divide only to the lot's places, so
    // the quotient stays small enough to hold.
    Decimal size = mParams.quantity;
    if (mParams.risk_per_trade < mParams.quantity * distance) {
        const int places =
            info->lot_size.Sign() > 0 ? info->lot_size.Normalized().Scale() : 8;
        size = Decimal::Divide(mParams.risk_per_trade, distance, places,
                               Rounding::kDown);
        if (info->lot_size.Sign() > 0) {
            size = size.RoundToIncrement(info->lot_size, Rounding::kDown);
        }
    }
    size = size.Normalized();
    if (size.Sign() <= 0 || size < info->min_order_size ||
        (info->min_notional.Sign() > 0 &&
         size * entry.Abs() < info->min_notional)) {
        spdlog::info(
            "breakout: {} entry of {} to risk {} with the stop {} away is "
            "under the venue's minimum; no entry",
            mParams.symbol, size.ToString(), mParams.risk_per_trade.ToString(),
            distance.ToString());
        return std::nullopt;
    }
    return size;
}

void Breakout::Arm(Context& context, const Levels& levels) {
    const auto size = EntrySize(context, levels.trigger, levels.stop);
    if (!size) {
        return;
    }
    types::OrderRequest request;
    request.symbol = mParams.symbol;
    request.side = Side::kBuy;
    request.type = types::OrderType::kStop;
    request.quantity = *size;
    request.stop_price = levels.trigger;
    if (auto id = Place(context, request, "entry stop")) {
        mEntryQuantity = *size;
        mDue = Due::kStop;
        mEntryId = *id;
        mLevels = levels;
        mState = State::kArmed;
        mBarsLeft = mParams.arm_bars;
    }
}

std::size_t Breakout::ClosesNeeded() const {
    return mParams.confirm == BreakoutParams::Confirm::kCloses
               ? mParams.confirm_closes
               : 1;
}

void Breakout::ArmThrowback(Context& context) {
    const auto size = EntrySize(context, mLevels->high, mLevels->stop);
    if (!size) {
        mLevels.reset();
        mState = State::kFlat;
        return;
    }
    types::OrderRequest request;
    request.symbol = mParams.symbol;
    request.side = Side::kBuy;
    request.type = types::OrderType::kLimit;
    request.quantity = *size;
    request.limit_price = mLevels->high;
    // Only ever a maker: if the price is already back at the level, the
    // venue refuses it rather than buying at market, and the setup is
    // dropped (the throwback came and went; on daily bars, an open below
    // the level), where a buy-stop would be placed again.
    request.post_only = true;
    if (auto id = Place(context, request, "throwback limit")) {
        spdlog::info("breakout: {} throwback limit at {}", mParams.symbol,
                     Price(mLevels->high));
        mDue = Due::kThrowback;
        mEntryId = *id;
        mEntryQuantity = *size;
        mState = State::kArmed;
        mBarsLeft = mParams.throwback_bars;
    } else {
        mLevels.reset();
        mState = State::kFlat;
    }
}

void Breakout::EnterAtMarket(Context& context) {
    // Sized from the last close, the nearest price to the fill there is.
    const auto bars = Bars(context);
    const auto size =
        EntrySize(context, bars.empty() ? mLevels->trigger : bars.back().close,
                  mLevels->stop);
    if (!size) {
        return;
    }
    types::OrderRequest request;
    request.symbol = mParams.symbol;
    request.side = Side::kBuy;
    request.type = types::OrderType::kMarket;
    request.quantity = *size;
    if (auto id = Place(context, request, "entry")) {
        mEntryId = *id;
        mEntryQuantity = *size;
        mState = State::kEntering;
    }
}

void Breakout::GoLong(Context& context) {
    mState = State::kLong;
    mExitDue = nullptr;
    mBarsHeld = 0;
    mEntryId.clear();
    mTarget.reset();
    mRisk = mEntryPrice - mLevels->stop;
    mFirstStop = mLevels->stop;
    mStopMoved = false;
    mTargetHit = false;
    mTargetRefused = false;
    if (mParams.target_multiple.Sign() > 0) {
        mTarget = mEntryPrice + mParams.target_multiple * mLevels->height;
    }
    spdlog::info("breakout: {} {} {} at {}; stop {}, target {}",
                 mShort ? "short" : "long", mHeld.ToString(), mParams.symbol,
                 Price(mEntryPrice), Price(mLevels->stop),
                 mTarget ? Price(*mTarget) : std::string("none"));
    PlaceStop(context);
}

void Breakout::GoFlat() {
    ++mTrade;
    mState = State::kFlat;
    mShort = mParams.direction == BreakoutParams::Direction::kShort;
    mReversal = false;
    mStopMoved = false;
    mExitDue = nullptr;
    mLevels.reset();
    mTarget.reset();
    mEntryFilled = Decimal();
    mBaseFeePaid = false;
    mEntryId.clear();
    mStopId.clear();
    mExitId.clear();
    mTargetId.clear();
    mTargetHit = false;
    mTargetRefused = false;
}

void Breakout::CancelOther(Context& context, std::string& id,
                           const char* what) {
    if (id.empty()) {
        return;
    }
    const auto cancelled = context.Venue().CancelOrder(id);
    if (!cancelled) {
        // Still live, perhaps: kept, so its cancel is retried and its fill
        // recognised.
        spdlog::error(
            "breakout: cannot cancel the {} {} for {}: {}; trying again each "
            "bar",
            what, id, mParams.symbol, cancelled.error().ToString());
        AddStray(id);
    } else if (*cancelled == trading::CancelOutcome::kAlreadyFilled) {
        spdlog::error(
            "breakout: the {} {} for {} filled too: sold more "
            "than was held",
            what, id, mParams.symbol);
        AddStray(id);  // its fill is still on the way
    }
    id.clear();
}

void Breakout::AddStray(const std::string& id) {
    if (!id.empty() &&
        std::ranges::find(mStrays, id, &Stray::id) == mStrays.end()) {
        mStrays.push_back({id, mTrade});
    }
}

void Breakout::RetryStrays(Context& context) {
    // A day of hourly bars: an error that lasts longer will not clear.
    constexpr std::size_t kMaxTries = 24;
    std::erase_if(mStrays, [&](Stray& stray) {
        if (stray.answered) {
            return true;  // a bar has passed for its fill to arrive
        }
        const auto cancelled = context.Venue().CancelOrder(stray.id);
        if (cancelled ||
            cancelled.error().category == types::ErrorCategory::kNotFound) {
            stray.answered = true;
            return false;
        }
        if (++stray.tries >= kMaxTries) {
            spdlog::error(
                "breakout: giving up cancelling {} for {} after {} tries: {}",
                stray.id, mParams.symbol, stray.tries,
                cancelled.error().ToString());
            return true;
        }
        spdlog::warn("breakout: still cannot cancel {} for {}: {}", stray.id,
                     mParams.symbol, cancelled.error().ToString());
        return false;
    });
}

void Breakout::PlaceStop(Context& context) {
    mStopMoved = false;  // placed at mLevels->stop, or retried next bar
    if (mBaseFeePaid) {
        if (const Decimal rounded =
                RoundDownToLot(context, mParams.symbol, mHeld);
            rounded.Sign() > 0) {
            mHeld = rounded;
        }
    }
    if (mParams.resting_target && mTarget && !mNoOco && !mTargetRefused) {
        types::OcoRequest pair;
        pair.symbol = mParams.symbol;
        pair.side = mShort ? Side::kBuy : Side::kSell;
        pair.quantity = mHeld;
        pair.limit_price = Mirror(*mTarget);
        pair.stop_price = Mirror(mLevels->stop);
        auto ack = context.Venue().PlaceOco(pair);
        if (ack && types::IsActive(ack->limit.state) &&
            types::IsActive(ack->stop.state)) {
            mStopId = ack->stop.order_id;
            mTargetId = ack->limit.order_id;
            return;
        }
        if (ack) {
            // One leg closed at once: take the other down (cancelling either
            // cancels both) and try again on the next bar. A stop that sold
            // at once makes us flat by its fill.
            spdlog::error(
                "breakout: {}'s target and stop came back {} and {}; trying "
                "again on the next bar",
                mParams.symbol, types::ToString(ack->limit.state),
                types::ToString(ack->stop.state));
            for (const auto* leg : {&ack->limit, &ack->stop}) {
                if (types::IsActive(leg->state)) {
                    std::string id = leg->order_id;
                    CancelOther(context, id, "half-placed pair");
                }
            }
            return;
        }
        const auto category = ack.error().category;
        if (category == types::ErrorCategory::kNetwork ||
            category == types::ErrorCategory::kTimeout ||
            category == types::ErrorCategory::kServer ||
            category == types::ErrorCategory::kRateLimit) {
            // It may have been placed, or may be next time: not a second
            // sell beside it now.
            spdlog::error(
                "breakout: {}'s target and stop failed: {}; unprotected "
                "until the next bar",
                mParams.symbol, ack.error().ToString());
            return;
        }
        if (category == types::ErrorCategory::kNotImplemented) {
            spdlog::warn(
                "breakout: {} cannot rest the target with the stop ({}); "
                "selling on a close past it instead",
                context.Venue().Name(), ack.error().ToString());
            mNoOco = true;
        } else {
            // Refused (the price already past the target, a risk limit the
            // pair's two orders break): the stop alone, the close the target.
            spdlog::warn(
                "breakout: {}'s target {} cannot rest with the stop: {}; "
                "selling on a close past it instead",
                mParams.symbol, Price(*mTarget), ack.error().ToString());
            mTargetRefused = true;
        }
    }
    types::OrderRequest request;
    request.symbol = mParams.symbol;
    request.side = Side::kSell;
    request.type = types::OrderType::kStop;
    request.quantity = mHeld;
    request.stop_price = mLevels->stop;
    if (auto id = Place(context, request, "protective stop")) {
        mStopId = *id;
    } else {
        // Unprotected until the next bar, which tries again.
        spdlog::error("breakout: {} is held without a protective stop",
                      mParams.symbol);
    }
}

bool Breakout::VolumeConfirms(Context& context, const types::Bar& bar) const {
    if (mParams.volume_ratio.Sign() <= 0) {
        return true;
    }
    const auto bars = context.Bars(mParams.symbol, mParams.timeframe);
    const std::size_t n = mParams.volume_bars;
    if (bars.size() < n + 1) {
        return false;
    }
    Decimal total;
    for (std::size_t i = bars.size() - 1 - n; i < bars.size() - 1; ++i) {
        total += bars[i].volume;
    }
    const Decimal average =
        Decimal::Divide(total, Decimal::FromInt(static_cast<std::int64_t>(n)),
                        8, Rounding::kNearest);
    // No volume (a source without it, or a dead market) confirms nothing.
    const bool enough =
        average.Sign() > 0 && bar.volume >= mParams.volume_ratio * average;
    if (!enough) {
        spdlog::info(
            "breakout: {} closed above {} on {} volume, under {}x the average "
            "of {}; not counted",
            mParams.symbol, Price(mLevels->trigger),
            bar.volume.Normalized().ToString(),
            mParams.volume_ratio.Normalized().ToString(),
            average.Normalized().ToString());
    }
    return enough;
}

void Breakout::RaiseStop(Context& context, const types::Bar& bar) {
    if (mParams.breakeven_r.Sign() <= 0 && mParams.trail_bars == 0) {
        return;
    }
    auto info = context.SymbolInfo(mParams.symbol);
    if (!info) {
        return;
    }
    const Decimal& tick = info->tick_size;
    Decimal level = mLevels->stop;
    const char* why = nullptr;
    if (mParams.breakeven_r.Sign() > 0 && mRisk.Sign() > 0 &&
        bar.close >= mEntryPrice + mParams.breakeven_r * mRisk) {
        const Decimal even =
            mEntryPrice.RoundToIncrement(tick, Rounding::kDown);
        if (even > level) {
            level = even;
            why = "breakeven";
        }
    }
    if (mParams.trail_bars > 0) {
        if (const auto range = RangeOf(Bars(context), mParams.trail_bars)) {
            const Decimal& low = range->low;
            const Decimal buffer = types::FromBps(mParams.stop_buffer_bps);
            const Decimal trail = (low - low.Abs() * buffer)
                                      .RoundToIncrement(tick, Rounding::kDown);
            if (trail > level) {
                level = trail;
                why = "trailing";
            }
        }
    }
    // A sell-stop at or above the price would sell at once.
    if (why && level < bar.close) {
        spdlog::info("breakout: {} stop {} from {} to {} ({})", mParams.symbol,
                     mShort ? "lowered" : "raised", Price(mLevels->stop),
                     Price(level), why);
        mLevels->stop = level;
        mStopMoved = true;
    }
}

void Breakout::MoveStop(Context& context) {
    if (mStopId.empty()) {
        PlaceStop(context);
        return;
    }
    const auto cancelled = context.Venue().CancelOrder(mStopId);
    if (!cancelled) {
        // The old stop still protects; try again on the next bar.
        spdlog::warn(
            "breakout: cannot cancel the protective stop {} to move "
            "it: {}",
            mStopId, cancelled.error().ToString());
        return;
    }
    if (*cancelled == trading::CancelOutcome::kAlreadyFilled) {
        return;  // it sold first; its fill makes us flat
    }
    if (*cancelled == trading::CancelOutcome::kAlreadyClosed &&
        !mTargetId.empty()) {
        return;  // the target sold first; its updates settle it
    }
    mStopId.clear();
    mTargetId.clear();  // cancelled with its stop
    PlaceStop(context);
    if (mStopId.empty()) {
        // Refused: most likely the price is already through it (an open
        // gapping below a stop raised on the close), where a venue refuses a
        // sell-stop. The raised stop said to be out: sell at market.
        Exit(context, "the raised stop was refused");
    }
}

void Breakout::Exit(Context& context, const char* why) {
    if (!mStopId.empty()) {
        const auto cancelled = context.Venue().CancelOrder(mStopId);
        if (cancelled && *cancelled == trading::CancelOutcome::kAlreadyFilled) {
            return;  // the stop got there first; its fill makes us flat
        }
        if (!cancelled) {
            spdlog::warn("breakout: cannot cancel the protective stop {}: {}",
                         mStopId, cancelled.error().ToString());
            return;  // it still protects; try again on the next bar
        }
        if (*cancelled == trading::CancelOutcome::kAlreadyClosed &&
            !mTargetId.empty()) {
            return;  // the target sold first; its updates settle it
        }
        mStopId.clear();
        mTargetId.clear();  // cancelled with its stop
    }
    if (mBaseFeePaid) {
        if (const Decimal rounded =
                RoundDownToLot(context, mParams.symbol, mHeld);
            rounded.Sign() > 0) {
            mHeld = rounded;
        }
    }
    types::OrderRequest request;
    request.symbol = mParams.symbol;
    request.side = Side::kSell;
    request.type = types::OrderType::kMarket;
    request.quantity = mHeld;
    spdlog::info("breakout: exit {} ({})", mParams.symbol, why);
    if (auto id = Place(context, request, "exit")) {
        mExitId = *id;
        mState = State::kExiting;
    } else if (mState == State::kLong) {
        PlaceStop(context);  // still long: protect it again
    }
}

void Breakout::Disarm(Context& context) {
    mBarsLeft = 0;  // if this stays armed, the next bar tries again
    const auto cancelled = context.Venue().CancelOrder(mEntryId);
    if (!cancelled) {
        spdlog::warn("breakout: cannot cancel the entry {}: {}", mEntryId,
                     cancelled.error().ToString());
        return;  // it may still fill: stay armed and keep tracking it
    }
    if (*cancelled == trading::CancelOutcome::kAlreadyFilled) {
        return;  // stays armed until the fill arrives
    }
    if (mHeld.Sign() > 0) {
        // Partly filled: hold what was bought, protected like a full entry.
        spdlog::warn("breakout: entry {} closed after filling {} of {}",
                     mEntryId, mHeld.ToString(), mEntryQuantity.ToString());
        GoLong(context);
        return;
    }
    GoFlat();
}

std::optional<std::string> Breakout::Place(Context& context,
                                           types::OrderRequest request,
                                           const char* what) {
    if (mShort) {
        request.side = request.side == Side::kBuy ? Side::kSell : Side::kBuy;
        if (request.limit_price) {
            request.limit_price = -*request.limit_price;
        }
        if (request.stop_price) {
            request.stop_price = -*request.stop_price;
        }
    }
    if (auto ack = Submit(context, request, "breakout", what)) {
        return ack->order_id;
    }
    return std::nullopt;
}

void Breakout::OnFill(Context& context, const types::Fill& raw) {
    if (raw.symbol != mParams.symbol) {
        return;
    }
    if (const auto stray = std::ranges::find(mStrays, raw.order_id, &Stray::id);
        stray != mStrays.end() && stray->trade != mTrade) {
        spdlog::error(
            "breakout: late fill of {} {} ({}) from an earlier trade; not "
            "counted",
            raw.quantity.ToString(), mParams.symbol, raw.order_id);
        return;
    }
    types::Fill fill = raw;  // in the trade's terms
    if (mShort) {
        fill.side = fill.side == Side::kBuy ? Side::kSell : Side::kBuy;
        fill.price = -fill.price;
    }
    if (fill.side == Side::kBuy) {
        const Decimal received =
            mShort ? fill.quantity : ReceivedOnBuy(raw, mBaseAsset);
        if (received != fill.quantity) {
            mBaseFeePaid = true;  // a fee in the coin: less held than bought
        }
        if (mState == State::kLong) {
            // A late part of a partly filled entry (its cancel crossed this
            // fill): the stop must cover it too.
            spdlog::warn(
                "breakout: late entry fill of {} {}; resizing the "
                "stop",
                fill.quantity.ToString(), mParams.symbol);
            mHeld += received;
            mEntryFilled += fill.quantity;
            CancelOther(context, mStopId, "protective stop");
            mTargetId.clear();  // cancelled with its stop
            PlaceStop(context);
            return;
        }
        if (mState == State::kExiting) {
            // A late part of the entry, bought by this trade: counted, though
            // the exit already out was sized before it.
            spdlog::error(
                "breakout: late entry fill of {} {} ({}) while "
                "exiting",
                fill.quantity.ToString(), mParams.symbol, fill.order_id);
            mHeld += received;
            mEntryFilled += fill.quantity;
            return;
        }
        if (mState != State::kArmed && mState != State::kEntering) {
            // Not this strategy's trade: counting it would size the next
            // trade's stop and exit for more than that trade bought.
            spdlog::error(
                "breakout: unexpected buy of {} {} ({}) while {}; not counted",
                fill.quantity.ToString(), mParams.symbol, fill.order_id,
                StateName());
            return;
        }
        if (mHeld.Sign() == 0 && mEntryFilled.Sign() == 0) {
            mEntryPrice = fill.price;  // the target is measured from here
            mEntryTime = fill.time;
        }
        mHeld += received;
        mEntryFilled += fill.quantity;
        if (mEntryFilled >= mEntryQuantity) {
            GoLong(context);
        }
        return;
    }
    const bool selling = mState == State::kLong || mState == State::kExiting;
    if (!selling) {
        spdlog::error("breakout: unexpected sell of {} {} ({}) while {}",
                      fill.quantity.ToString(), mParams.symbol, fill.order_id,
                      StateName());
    }
    mHeld = std::max(mHeld - fill.quantity, Decimal());
    if (!mTargetId.empty() && fill.order_id == mTargetId) {
        mTargetHit = true;
        if (mState == State::kLong && mHeld.Sign() > 0) {
            // In part: the price got there, so sell the rest now rather
            // than leave it to a stop sized for the whole (cancelling the
            // target cancels its stop too, on a venue that has not already).
            const auto cancelled =
                context.Venue().CancelOrder(std::exchange(mTargetId, {}));
            if (cancelled &&
                *cancelled == trading::CancelOutcome::kAlreadyFilled) {
                return;  // it sold the rest too; that fill makes us flat
            }
            Exit(context, "target, the rest");
            return;
        }
    }
    if (selling && mHeld.Sign() == 0) {
        // A failed breakout: the stop, never moved, closed all of it.
        const bool reverse = mParams.reverse && !mReversal &&
                             fill.order_id == mStopId && !mTargetHit &&
                             mState == State::kLong && mLevels &&
                             mLevels->stop == mFirstStop;
        const std::optional<Levels> failed = mLevels;
        if (reverse) {
            // A cancel acknowledged may still be crossing a fill.
            for (const auto* id : {&mStopId, &mTargetId, &mExitId}) {
                if (*id != fill.order_id) {
                    AddStray(*id);
                }
            }
        }
        // Sold out, by the stop, the target or the exit: the others must not
        // sell too (the exit's cancel of the stop may not have landed yet).
        if (fill.order_id != mStopId) {
            CancelOther(context, mStopId, "protective stop");
        }
        if (fill.order_id != mTargetId) {
            CancelOther(context, mTargetId, "target");
        }
        if (fill.order_id != mExitId) {
            CancelOther(context, mExitId, "exit");
        }
        GoFlat();
        if (reverse) {
            Reverse(context, *failed);
        }
    }
}

void Breakout::Reverse(Context& context, const Levels& failed) {
    auto info = context.SymbolInfo(mParams.symbol);
    if (!info) {
        spdlog::warn("breakout: cannot reverse {}: no symbol info: {}",
                     mParams.symbol, info.error().ToString());
        return;
    }
    const Decimal buffer = types::FromBps(mParams.stop_buffer_bps);
    // In the failed trade's terms: beyond the level it broke.
    const Decimal beyond =
        (failed.high + failed.high.Abs() * buffer)
            .RoundToIncrement(info->tick_size, Rounding::kUp);
    // The other way round: the failed trade's prices negated.
    mShort = !mShort;
    mReversal = true;
    Levels levels;
    levels.trigger = levels.high = -failed.high;
    levels.stop = -beyond;
    levels.height = failed.height;
    mLevels = levels;
    spdlog::info("breakout: {} failed; reversing, {} at market, stop {}",
                 mParams.symbol, mShort ? "short" : "long", Price(levels.stop));
    EnterAtMarket(context);
    if (mState != State::kEntering) {
        GoFlat();  // refused: flat, looking for setups the usual way
    }
}

types::Bar Breakout::Mirror(const types::Bar& bar) const {
    if (!mShort) {
        return bar;
    }
    types::Bar out = bar;
    out.open = -bar.open;
    out.high = -bar.low;
    out.low = -bar.high;
    out.close = -bar.close;
    return out;
}

std::span<const types::Bar> Breakout::Bars(Context& context) const {
    const auto bars = context.Bars(mParams.symbol, mParams.timeframe);
    if (!mShort) {
        return bars;
    }
    mMirrored.clear();
    mMirrored.reserve(bars.size());
    for (const types::Bar& bar : bars) {
        mMirrored.push_back(Mirror(bar));
    }
    return mMirrored;
}

void Breakout::OnOrderUpdate(Context& context,
                             const types::OrderStatus& status) {
    if (!ClosedUnfilled(status.state)) {
        return;
    }
    // Orders cancelled on purpose have had their IDs cleared already. The
    // runner delivers an update's fills before its state, so mHeld counts
    // whatever part of the order filled.
    if (status.order_id == mEntryId &&
        (mState == State::kArmed || mState == State::kEntering)) {
        spdlog::warn("breakout: entry {} closed: {}, {} of {} filled",
                     status.order_id, types::ToString(status.state),
                     mHeld.ToString(), mEntryQuantity.ToString());
        if (mHeld.Sign() > 0) {
            GoLong(context);  // protect the part bought
        } else {
            GoFlat();
        }
    } else if (status.order_id == mTargetId && mState == State::kLong) {
        // The pair closes together: the stop's own update follows.
        mTargetId.clear();
        if (status.state == OrderState::kRejected) {
            mTargetRefused = true;  // the price is past it: the close checks
        }
    } else if (status.order_id == mStopId && mState == State::kLong) {
        mStopId.clear();
        if (!mTargetId.empty() && status.state == OrderState::kRejected) {
            mTargetRefused = true;  // the pair was refused: not again
        }
        if (!mTargetId.empty()) {
            // Closed with its target, which may have sold some: take the
            // target down too (if it is still working) before going on.
            const std::string target = std::exchange(mTargetId, {});
            const auto cancelled = context.Venue().CancelOrder(target);
            if (cancelled &&
                *cancelled == trading::CancelOutcome::kAlreadyFilled) {
                return;  // the target sold it all; its fill makes us flat
            }
        }
        if (mTargetHit) {
            // The target sold part: the price got there, so sell the rest.
            Exit(context, "target, the rest");
            return;
        }
        spdlog::warn(
            "breakout: protective stop {} closed: {}; placing it "
            "again for {}",
            status.order_id, types::ToString(status.state), mHeld.ToString());
        PlaceStop(context);
    } else if (status.order_id == mExitId && mState == State::kExiting) {
        spdlog::warn("breakout: exit {} closed: {}; {} still held",
                     status.order_id, types::ToString(status.state),
                     mHeld.ToString());
        mExitId.clear();
        mState = State::kLong;
        PlaceStop(context);
    }
}

void Breakout::OnSessionOpen(Context& context) {
    if (mState == State::kPending) {
        const std::size_t left = mBarsLeft;
        const Levels levels = *mLevels;
        switch (mDue) {
            case Due::kStop:
                Arm(context, levels);
                break;
            case Due::kMarket:
                EnterAtMarket(context);
                break;
            case Due::kThrowback:
                ArmThrowback(context);
                break;
        }
        if (mState == State::kArmed) {
            mBarsLeft = left;
        } else if (mState == State::kPending) {
            GoFlat();  // the order was refused
        }
    } else if (mState == State::kLong) {
        if (mStopId.empty()) {
            PlaceStop(context);
        } else if (mStopMoved && !mExitDue) {
            MoveStop(context);
        }
        if (mExitDue) {
            Exit(context, std::exchange(mExitDue, nullptr));
        }
    }
}

void Breakout::OnSessionClosing(Context& context, types::Timestamp) {
    switch (mState) {
        case State::kArmed: {
            const std::size_t left = mBarsLeft;
            const auto levels = mLevels;
            const Due due = mDue;
            Disarm(context);  // not into the next open
            // Daily bars: placed again at the next open while bars remain
            // (the bar closing after this close counts one).
            if (mState == State::kFlat && CarriesOver() && left > 1 && levels) {
                mLevels = levels;
                Pend(due, left);
            }
            if (mState == State::kLong && mParams.flatten_at_close) {
                Exit(context, "session closing");  // it had partly filled
            }
            break;
        }
        case State::kWatching:
            // Intraday, a watch does not span the night; daily bars count
            // their closes across sessions.
            if (!CarriesOver()) {
                mState = State::kFlat;
                mLevels.reset();
            }
            break;
        case State::kLong:
            if (mParams.flatten_at_close) {
                Exit(context, "session closing");
            }
            break;
        default:
            break;
    }
}

const char* Breakout::StateName() const {
    switch (mState) {
        case State::kFlat:
            return "flat";
        case State::kArmed:
            return "armed";
        case State::kWatching:
            return "watching";
        case State::kEntering:
            return "entering";
        case State::kLong:
            return "long";
        case State::kExiting:
            return "exiting";
        case State::kPending:
            return "pending";
    }
    return "?";
}

}  // namespace kestrel::strategy
