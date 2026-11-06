#include "kestrel/strategy/breakout.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <stdexcept>

#include "kestrel/strategy/patterns.hpp"

namespace kestrel::strategy {

using types::Decimal;
using types::OrderState;
using types::Rounding;
using types::Side;

namespace {

bool ClosedUnfilled(const types::OrderStatus& status) {
    return (status.state == OrderState::kCancelled ||
            status.state == OrderState::kRejected ||
            status.state == OrderState::kExpired) &&
           status.filled_quantity.Sign() == 0;
}

}  // namespace

Breakout::Breakout(BreakoutParams params) : mParams(std::move(params)) {
    if (mParams.quantity.Sign() <= 0) {
        throw std::invalid_argument("quantity must be positive");
    }
    if (mParams.nr_bars < 2) {
        throw std::invalid_argument("nr_bars must be at least 2");
    }
    if (mParams.range_bars == 0 || mParams.arm_bars == 0 ||
        mParams.confirm_closes == 0) {
        throw std::invalid_argument(
            "range_bars, arm_bars and confirm_closes must be positive");
    }
    if (mParams.stop_buffer_bps.IsNegative() ||
        mParams.confirm_percent.IsNegative() ||
        mParams.target_multiple.IsNegative()) {
        throw std::invalid_argument(
            "stop_buffer_bps, confirm_percent and target_multiple cannot be "
            "negative");
    }
}

Requirements Breakout::Needs() const {
    Requirements needs;
    needs.symbols = {mParams.symbol};
    needs.bar_timeframes = {mParams.timeframe};
    std::size_t setup_bars = 2;
    if (mParams.setup == BreakoutParams::Setup::kNarrowRange) {
        setup_bars = mParams.nr_bars;
    } else if (mParams.setup == BreakoutParams::Setup::kRange) {
        setup_bars = mParams.range_bars;
    }
    needs.warmup_bars = std::max(setup_bars, mParams.trend_sma);
    return needs;
}

std::optional<Breakout::Levels> Breakout::FindSetup(
    Context& context, std::span<const types::Bar> bars) const {
    std::optional<PriceRange> range;
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
    const Decimal one = Decimal::FromInt(1);
    Levels levels;
    levels.height = range->Height();
    levels.trigger = range->high + tick;
    if (mParams.confirm == BreakoutParams::Confirm::kPercent) {
        const Decimal raise =
            Decimal::Divide(mParams.confirm_percent, Decimal::FromInt(100), 8,
                            Rounding::kNearest);
        levels.trigger = std::max(levels.trigger,
                                  (range->high * (one + raise))
                                      .RoundToIncrement(tick, Rounding::kUp));
    }
    const Decimal buffer =
        Decimal::Divide(mParams.stop_buffer_bps, Decimal::FromInt(10000), 8,
                        Rounding::kNearest);
    levels.stop =
        (range->low * (one - buffer)).RoundToIncrement(tick, Rounding::kDown);
    return levels;
}

void Breakout::OnBar(Context& context, const types::Symbol& symbol,
                     types::Timeframe timeframe, const types::Bar& bar) {
    if (symbol != mParams.symbol || timeframe != mParams.timeframe) {
        return;
    }
    switch (mState) {
        case State::kArmed:
            if (--mBarsLeft > 0) {
                return;
            }
            Disarm(context);
            if (mState != State::kFlat) {
                return;  // it filled meanwhile
            }
            break;  // expired: this bar may be a new setup
        case State::kWatching:
            if (bar.close > mLevels->trigger) {
                if (++mCloses >= mParams.confirm_closes) {
                    EnterAtMarket(context);
                }
                if (mState != State::kWatching || --mBarsLeft > 0) {
                    return;
                }
            } else {
                mCloses = 0;
                if (--mBarsLeft > 0) {
                    return;
                }
            }
            mState = State::kFlat;
            mLevels.reset();
            break;
        case State::kLong:
            // Whole bars only: not the one the entry filled inside.
            if (bar.open_time >= mEntryTime) {
                ++mBarsHeld;
            }
            // Orders are refused while closed: the resting stop protects
            // until the next bar in the session.
            if (!context.MarketOpen()) {
                return;
            }
            if (mStopId.empty()) {
                PlaceStop(context);  // an earlier attempt failed
            }
            if (mTarget && bar.close >= *mTarget) {
                Exit(context, "target");
            } else if (mParams.max_hold_bars > 0 &&
                       mBarsHeld >= mParams.max_hold_bars) {
                Exit(context, "held long enough");
            }
            return;
        case State::kEntering:
        case State::kExiting:
            return;
        case State::kFlat:
            break;
    }

    // Flat: look for a setup. Not while the market is closed: an order then
    // is refused, and an entry must not rest into the next open.
    if (!context.MarketOpen()) {
        return;
    }
    const auto levels = FindSetup(context, context.Bars(symbol, timeframe));
    if (!levels) {
        return;
    }
    if (mParams.confirm == BreakoutParams::Confirm::kCloses) {
        mLevels = levels;
        mState = State::kWatching;
        mCloses = 0;
        // The closes must start within arm_bars bars.
        mBarsLeft = mParams.arm_bars + mParams.confirm_closes - 1;
        return;
    }
    Arm(context, *levels);
}

void Breakout::Arm(Context& context, const Levels& levels) {
    types::OrderRequest request;
    request.symbol = mParams.symbol;
    request.side = Side::kBuy;
    request.type = types::OrderType::kStop;
    request.quantity = mParams.quantity;
    request.stop_price = levels.trigger;
    if (auto id = Place(context, request, "entry stop")) {
        mEntryId = *id;
        mLevels = levels;
        mState = State::kArmed;
        mBarsLeft = mParams.arm_bars;
    }
}

void Breakout::EnterAtMarket(Context& context) {
    types::OrderRequest request;
    request.symbol = mParams.symbol;
    request.side = Side::kBuy;
    request.type = types::OrderType::kMarket;
    request.quantity = mParams.quantity;
    if (auto id = Place(context, request, "entry")) {
        mEntryId = *id;
        mState = State::kEntering;
    }
}

void Breakout::PlaceStop(Context& context) {
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
        mStopId.clear();
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
    const auto cancelled = context.Venue().CancelOrder(mEntryId);
    if (cancelled && *cancelled == trading::CancelOutcome::kAlreadyFilled) {
        return;  // stays armed until the fill arrives
    }
    if (!cancelled) {
        spdlog::warn("breakout: cannot cancel the entry stop {}: {}", mEntryId,
                     cancelled.error().ToString());
    }
    mEntryId.clear();
    mLevels.reset();
    mState = State::kFlat;
}

std::optional<std::string> Breakout::Place(Context& context,
                                           const types::OrderRequest& request,
                                           const char* what) {
    auto ack = context.Venue().PlaceOrder(request);
    if (!ack) {
        spdlog::warn("breakout: {} for {} failed: {}", what, mParams.symbol,
                     ack.error().ToString());
        return std::nullopt;
    }
    if (ack->state == OrderState::kCancelled ||
        ack->state == OrderState::kRejected ||
        ack->state == OrderState::kExpired) {
        spdlog::warn("breakout: {} for {} closed at once: {}", what,
                     mParams.symbol, types::ToString(ack->state));
        return std::nullopt;
    }
    return ack->order_id;
}

void Breakout::OnFill(Context& context, const types::Fill& fill) {
    if (fill.symbol != mParams.symbol) {
        return;
    }
    if (fill.side == Side::kBuy) {
        mHeld += fill.quantity;
        if ((mState == State::kArmed || mState == State::kEntering) &&
            mHeld >= mParams.quantity) {
            mState = State::kLong;
            mBarsHeld = 0;
            mEntryTime = fill.time;
            mEntryId.clear();
            mTarget.reset();
            if (mParams.target_multiple.Sign() > 0) {
                mTarget =
                    fill.price + mParams.target_multiple * mLevels->height;
            }
            spdlog::info("breakout: long {} {} at {}; stop {}, target {}",
                         mHeld.ToString(), mParams.symbol,
                         fill.price.ToString(), mLevels->stop.ToString(),
                         mTarget ? mTarget->ToString() : std::string("none"));
            PlaceStop(context);
        }
        return;
    }
    mHeld = std::max(mHeld - fill.quantity, Decimal());
    if (mHeld.Sign() == 0) {
        mState = State::kFlat;
        mLevels.reset();
        mTarget.reset();
        mStopId.clear();
        mExitId.clear();
    }
}

void Breakout::OnOrderUpdate(Context& context,
                             const types::OrderStatus& status) {
    if (!ClosedUnfilled(status)) {
        return;
    }
    // Orders cancelled on purpose have had their IDs cleared already.
    if (status.order_id == mEntryId &&
        (mState == State::kArmed || mState == State::kEntering)) {
        spdlog::warn("breakout: entry {} closed unfilled: {}", status.order_id,
                     types::ToString(status.state));
        mEntryId.clear();
        mLevels.reset();
        mState = State::kFlat;
    } else if (status.order_id == mStopId && mState == State::kLong) {
        spdlog::warn(
            "breakout: protective stop {} closed unfilled: {}; "
            "placing it again",
            status.order_id, types::ToString(status.state));
        mStopId.clear();
        PlaceStop(context);
    } else if (status.order_id == mExitId && mState == State::kExiting) {
        spdlog::warn("breakout: exit {} closed unfilled: {}", status.order_id,
                     types::ToString(status.state));
        mExitId.clear();
        mState = State::kLong;
        PlaceStop(context);
    }
}

void Breakout::OnSessionClosing(Context& context, types::Timestamp) {
    switch (mState) {
        case State::kArmed:
            Disarm(context);  // not into the next open
            break;
        case State::kWatching:
            mState = State::kFlat;
            mLevels.reset();
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

}  // namespace kestrel::strategy
