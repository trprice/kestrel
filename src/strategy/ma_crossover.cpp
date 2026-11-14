#include "kestrel/strategy/ma_crossover.hpp"

#include <stdexcept>

#include "kestrel/strategy/patterns.hpp"

namespace kestrel::strategy {

using types::Decimal;

MaCrossover::MaCrossover(MaCrossoverParams params)
    : mParams(std::move(params)),
      mPosition(mParams.symbol, mParams.quantity) {
    if (mParams.fast_period == 0 ||
        mParams.fast_period >= mParams.slow_period) {
        throw std::invalid_argument("need 0 < fast_period < slow_period");
    }
    if (mParams.quantity.Sign() <= 0) {
        throw std::invalid_argument("quantity must be positive");
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
    if (mPosition.Busy() || symbol != mParams.symbol ||
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

    if (!mPosition.Long() && prev_fast <= prev_slow && fast > slow) {
        mPosition.Buy(context);
    } else if (mPosition.Long() && prev_fast >= prev_slow && fast < slow) {
        mPosition.Sell(context);
    }
}

void MaCrossover::OnFill(Context& context, const types::Fill& fill) {
    mPosition.OnFill(context, fill);
}

void MaCrossover::OnOrderUpdate(Context& context,
                                const types::OrderStatus& status) {
    mPosition.OnOrderUpdate(context, status);
}

}  // namespace kestrel::strategy
