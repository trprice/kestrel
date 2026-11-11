#include "kestrel/strategy/ma_crossover.hpp"

#include <spdlog/spdlog.h>

#include <stdexcept>

#include "kestrel/strategy/patterns.hpp"

namespace kestrel::strategy {

using types::Decimal;
using types::Side;

MaCrossover::MaCrossover(MaCrossoverParams params)
    : mParams(std::move(params)) {
    if (mParams.fast_period == 0 ||
        mParams.fast_period >= mParams.slow_period) {
        throw std::invalid_argument("need 0 < fast_period < slow_period");
    }
    if (mParams.quantity.Sign() <= 0) {
        throw std::invalid_argument("quantity must be positive");
    }
    if (const auto parts = types::SplitSymbol(mParams.symbol)) {
        mBaseAsset = parts->base;
    }
}

Requirements MaCrossover::Needs() const {
    Requirements needs;
    needs.symbols = {mParams.symbol};
    needs.bar_timeframes = {mParams.timeframe};
    // The slow average over the previous bar, plus the newest bar; an EMA
    // is smoothed over the whole window, so give it a few periods to settle.
    needs.warmup_bars = mParams.average == MaCrossoverParams::Average::kEma
                            ? 4 * mParams.slow_period + 1
                            : mParams.slow_period + 1;
    return needs;
}

void MaCrossover::OnBar(Context& context, const types::Symbol& symbol,
                        types::Timeframe timeframe, const types::Bar&) {
    if (mPending || symbol != mParams.symbol ||
        timeframe != mParams.timeframe) {
        return;
    }
    const auto bars = context.Bars(symbol, timeframe);
    if (bars.size() < mParams.slow_period + 1) {
        return;
    }

    // The window holds at least slow_period + 1 bars (checked above); an EMA
    // is steadier with the full warmup, which a backtest or a live warmup
    // normally loads before the first bar.
    const auto average = [&](std::size_t period, std::size_t back) {
        return mParams.average == MaCrossoverParams::Average::kEma
                   ? *Ema(bars, period, back)
                   : *Sma(bars, period, back);
    };
    const Decimal fast = average(mParams.fast_period, 0);
    const Decimal slow = average(mParams.slow_period, 0);
    const Decimal prev_fast = average(mParams.fast_period, 1);
    const Decimal prev_slow = average(mParams.slow_period, 1);

    if (!mLong && prev_fast <= prev_slow && fast > slow) {
        Trade(context, Side::kBuy);
    } else if (mLong && prev_fast >= prev_slow && fast < slow) {
        Trade(context, Side::kSell);
    }
}

void MaCrossover::OnFill(Context&, const types::Fill& fill) {
    if (fill.symbol != mParams.symbol) {
        return;
    }
    mPending = false;
    if (fill.side == Side::kBuy) {
        mLong = true;
        if (fill.quantity.Sign() > 0) {
            Decimal received = fill.quantity;
            if (fill.fee.Sign() > 0 && !mBaseAsset.empty() &&
                fill.fee_asset == mBaseAsset) {
                received = std::max(received - fill.fee, Decimal());
            }
            mHeld += received;
        } else {
            mHeld = mParams.quantity;
        }
    } else {
        if (fill.quantity.Sign() > 0) {
            mHeld = std::max(mHeld - fill.quantity, Decimal());
        } else {
            mHeld = Decimal();
        }
        mLong = false;
    }
}

void MaCrossover::OnOrderUpdate(Context&, const types::OrderStatus& status) {
    if (!mPending || status.order_id != mOrderId) {
        return;
    }
    // A closed order with nothing filled will never produce a fill. (A filled
    // one is left to OnFill, which also learns the side.)
    const bool dead = status.state == types::OrderState::kCancelled ||
                      status.state == types::OrderState::kRejected ||
                      status.state == types::OrderState::kExpired;
    if (dead && status.filled_quantity.Sign() == 0) {
        spdlog::warn("ma_crossover: order {} closed without a fill: {}",
                     status.order_id, types::ToString(status.state));
        mPending = false;
    }
}

void MaCrossover::Trade(Context& context, Side side) {
    Decimal quantity =
        side == Side::kSell && mHeld.Sign() > 0 ? mHeld : mParams.quantity;
    if (side == Side::kSell && quantity != mParams.quantity) {
        if (auto info = context.SymbolInfo(mParams.symbol);
            info && info->lot_size.Sign() > 0) {
            const Decimal rounded = quantity.RoundToIncrement(
                info->lot_size, types::Rounding::kDown);
            if (rounded.Sign() > 0) {
                quantity = rounded;
            }
        }
    }
    types::OrderRequest request;
    request.symbol = mParams.symbol;
    request.side = side;
    request.type = types::OrderType::kMarket;
    request.quantity = quantity;

    auto ack = context.Venue().PlaceOrder(request);
    if (!ack) {
        spdlog::warn("ma_crossover: {} {} failed: {}", types::ToString(side),
                     mParams.symbol, ack.error().ToString());
        return;
    }
    // A venue can acknowledge an order it has already closed without a fill
    // (e.g. an unfillable fill-or-kill order); no fill will arrive for it.
    if (ack->state == types::OrderState::kCancelled ||
        ack->state == types::OrderState::kRejected ||
        ack->state == types::OrderState::kExpired) {
        spdlog::warn("ma_crossover: {} {} closed without a fill: {}",
                     types::ToString(side), mParams.symbol,
                     types::ToString(ack->state));
        return;
    }
    mPending = true;
    mOrderId = ack->order_id;
    spdlog::info("ma_crossover: {} {} {} (order {})", types::ToString(side),
                 quantity.ToString(), mParams.symbol, ack->order_id);
}

}  // namespace kestrel::strategy
