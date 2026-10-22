#include "kestrel/strategy/sma_crossover.hpp"

#include <spdlog/spdlog.h>

#include <stdexcept>

namespace kestrel::strategy {

using types::Decimal;
using types::Rounding;
using types::Side;

namespace {

// Average close of the `period` bars ending `back` bars before the newest.
Decimal Sma(std::span<const types::Bar> bars, std::size_t back,
            std::size_t period) {
    Decimal sum;
    for (std::size_t i = bars.size() - back - period; i < bars.size() - back;
         ++i) {
        sum += bars[i].close;
    }
    return Decimal::Divide(sum,
                           Decimal::FromInt(static_cast<std::int64_t>(period)),
                           8, Rounding::kNearest);
}

}  // namespace

SmaCrossover::SmaCrossover(SmaCrossoverParams params)
    : mParams(std::move(params)) {
    if (mParams.fast_period == 0 ||
        mParams.fast_period >= mParams.slow_period) {
        throw std::invalid_argument("need 0 < fast_period < slow_period");
    }
    if (mParams.quantity.Sign() <= 0) {
        throw std::invalid_argument("quantity must be positive");
    }
}

Requirements SmaCrossover::Needs() const {
    Requirements needs;
    needs.symbols = {mParams.symbol};
    needs.bar_timeframes = {mParams.timeframe};
    // The slow average over the previous bar, plus the newest bar.
    needs.warmup_bars = mParams.slow_period + 1;
    return needs;
}

void SmaCrossover::OnBar(Context& context, const types::Symbol& symbol,
                         types::Timeframe timeframe, const types::Bar&) {
    if (mPending || symbol != mParams.symbol ||
        timeframe != mParams.timeframe) {
        return;
    }
    const auto bars = context.Bars(symbol, timeframe);
    if (bars.size() < mParams.slow_period + 1) {
        return;
    }

    const Decimal fast = Sma(bars, 0, mParams.fast_period);
    const Decimal slow = Sma(bars, 0, mParams.slow_period);
    const Decimal prev_fast = Sma(bars, 1, mParams.fast_period);
    const Decimal prev_slow = Sma(bars, 1, mParams.slow_period);

    if (!mLong && prev_fast <= prev_slow && fast > slow) {
        Trade(context, Side::kBuy);
    } else if (mLong && prev_fast >= prev_slow && fast < slow) {
        Trade(context, Side::kSell);
    }
}

void SmaCrossover::OnFill(Context&, const types::Fill& fill) {
    if (fill.symbol != mParams.symbol) {
        return;
    }
    mPending = false;
    mLong = fill.side == Side::kBuy;
}

void SmaCrossover::Trade(Context& context, Side side) {
    types::OrderRequest request;
    request.symbol = mParams.symbol;
    request.side = side;
    request.type = types::OrderType::kMarket;
    request.quantity = mParams.quantity;

    auto ack = context.Venue().PlaceOrder(request);
    if (!ack) {
        spdlog::warn("sma_crossover: {} {} failed: {}", types::ToString(side),
                     mParams.symbol, ack.error().ToString());
        return;
    }
    // A venue can acknowledge an order it has already closed without a fill
    // (e.g. an unfillable fill-or-kill order); no fill will arrive for it.
    if (ack->state == types::OrderState::kCancelled ||
        ack->state == types::OrderState::kRejected ||
        ack->state == types::OrderState::kExpired) {
        spdlog::warn("sma_crossover: {} {} closed without a fill: {}",
                     types::ToString(side), mParams.symbol,
                     types::ToString(ack->state));
        return;
    }
    mPending = true;
    spdlog::info("sma_crossover: {} {} {} (order {})", types::ToString(side),
                 mParams.quantity.ToString(), mParams.symbol, ack->order_id);
}

}  // namespace kestrel::strategy
