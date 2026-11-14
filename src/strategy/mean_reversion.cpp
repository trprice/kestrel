#include "kestrel/strategy/mean_reversion.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "kestrel/strategy/patterns.hpp"

namespace kestrel::strategy {

using types::Decimal;
using types::Side;

MeanReversion::MeanReversion(MeanReversionParams params)
    : mParams(std::move(params)),
      mPosition(mParams.symbol, mParams.quantity) {
    if (mParams.quantity.Sign() <= 0) {
        throw std::invalid_argument("quantity must be positive");
    }
    if (mParams.rsi_period == 0) {
        throw std::invalid_argument("rsi_period must be positive");
    }
    const Decimal hundred = Decimal::FromInt(100);
    if (mParams.oversold.Sign() <= 0 ||
        mParams.oversold >= mParams.overbought ||
        mParams.overbought >= hundred) {
        throw std::invalid_argument("need 0 < oversold < overbought < 100");
    }
    if (mParams.bollinger_period == 1) {
        throw std::invalid_argument("bollinger_period must be 0 or at least 2");
    }
    if (mParams.bollinger_width.Sign() <= 0) {
        throw std::invalid_argument("bollinger_width must be positive");
    }
    if (mParams.atr_stop.IsNegative()) {
        throw std::invalid_argument("atr_stop cannot be negative");
    }
    if (mParams.atr_bars == 0) {
        throw std::invalid_argument("atr_bars must be positive");
    }
    // An average of one bar is the close itself: the exit would come on the
    // first bar, the trend filter never pass.
    if (mParams.exit_sma == 1 || mParams.trend_sma == 1) {
        throw std::invalid_argument(
            "exit_sma and trend_sma must be 0 or at least 2");
    }
}

LogContext MeanReversion::Log(Context& context) const {
    return context.Log().WithDefault("symbol", mParams.symbol);
}

Requirements MeanReversion::Needs() const {
    Requirements needs;
    needs.symbols = {mParams.symbol};
    needs.bar_timeframes = {mParams.timeframe};
    // Rsi and Atr use Wilder's smoothing (alpha = 1/n) over the whole window;
    // six periods leave the initial seed under 1%.
    needs.warmup_bars =
        std::max({6 * mParams.rsi_period + 1, mParams.bollinger_period,
                  mParams.exit_sma, mParams.trend_sma,
                  mParams.atr_stop.Sign() > 0 ? 6 * mParams.atr_bars + 1
                                              : std::size_t{0}});
    return needs;
}

void MeanReversion::OnBar(Context& context, const types::Symbol& symbol,
                          types::Timeframe timeframe, const types::Bar& bar) {
    if (mPosition.Busy() || symbol != mParams.symbol ||
        timeframe != mParams.timeframe) {
        return;
    }
    const auto bars = context.Bars(symbol, timeframe);
    const auto rsi = Rsi(bars, mParams.rsi_period);
    if (!rsi) {
        return;
    }

    if (!mPosition.Long()) {
        // An entry due at the next open stands only while its signal does,
        // and the ATR and stop are this bar's signal's or none.
        mDueSide.reset();
        mEntryAtr = Decimal();
        mStopPrice = Decimal();
        if (*rsi > mParams.oversold) {
            return;
        }
        if (mParams.bollinger_period > 0) {
            const auto bands = Bollinger(bars, mParams.bollinger_period,
                                         mParams.bollinger_width);
            if (!bands || bar.close > bands->lower) {
                return;
            }
        }
        if (mParams.trend_sma > 0) {
            const auto trend = Sma(bars, mParams.trend_sma);
            if (!trend || bar.close <= *trend) {
                return;
            }
        }
        if (mParams.atr_stop.Sign() > 0) {
            const auto atr = Atr(bars, mParams.atr_bars);
            if (!atr || atr->Sign() <= 0) {
                return;
            }
            const Decimal stop = bar.close - mParams.atr_stop * *atr;
            if (stop.Sign() <= 0) {
                Log(context).Warn(
                    "{} ATRs of {} under {} is no price for a stop; no entry",
                    mParams.atr_stop.ToString(), atr->ToString(),
                    bar.close.ToString());
                return;
            }
            mEntryAtr = *atr;
            mStopPrice = stop;
        }
        if (!context.MarketOpen()) {
            mDueSide = Side::kBuy;
            return;
        }
        Trade(context, Side::kBuy);
        return;
    }

    ++mBarsHeld;
    const bool overbought = *rsi >= mParams.overbought;
    bool sma_exit = false;
    if (mParams.exit_sma > 0) {
        const auto avg = Sma(bars, mParams.exit_sma);
        sma_exit = avg && bar.close >= *avg;
    }
    const bool stop_hit = mParams.atr_stop.Sign() > 0 &&
                          mStopPrice.Sign() > 0 && bar.close <= mStopPrice;
    const bool time_exit =
        mParams.max_hold_bars > 0 && mBarsHeld >= mParams.max_hold_bars;

    if (overbought || sma_exit || stop_hit || time_exit) {
        if (!context.MarketOpen()) {
            mDueSide = Side::kSell;
            return;
        }
        Trade(context, Side::kSell);
    }
}

void MeanReversion::OnFill(Context& context, const types::Fill& fill) {
    Apply(context, mPosition.OnFill(context, fill));
}

void MeanReversion::OnOrderUpdate(Context& context,
                                  const types::OrderStatus& status) {
    Apply(context, mPosition.OnOrderUpdate(context, status));
}

void MeanReversion::Apply(Context& context,
                          SingleOrderPosition::Change change) {
    if (change == SingleOrderPosition::Change::kEntered) {
        mBarsHeld = 0;
        // The stop is set from the price paid, not the signal's close.
        if (mParams.atr_stop.Sign() > 0 && mEntryAtr.Sign() > 0 &&
            mPosition.EntryPrice().Sign() > 0) {
            const Decimal stop =
                mPosition.EntryPrice() - mParams.atr_stop * mEntryAtr;
            if (stop.Sign() > 0) {
                mStopPrice = stop;
            } else {
                Log(context).Warn(
                    "paid {}, too little for a stop {} ATRs under; the "
                    "signal's stop {} stands",
                    mPosition.EntryPrice().ToString(),
                    mParams.atr_stop.ToString(), mStopPrice.ToString());
            }
        }
    } else if (change == SingleOrderPosition::Change::kExited) {
        mBarsHeld = 0;
        mEntryAtr = Decimal();
        mStopPrice = Decimal();
    }
}

void MeanReversion::OnSessionOpen(Context& context) {
    if (!mDueSide || mPosition.Busy()) {
        return;
    }
    const Side side = *std::exchange(mDueSide, std::nullopt);
    const bool enter = side == Side::kBuy && !mPosition.Long();
    const bool exit = side == Side::kSell && mPosition.Long();
    if (enter || exit) {
        Trade(context, side);
    }
}

void MeanReversion::OnSessionClosing(Context& context, types::Timestamp) {
    mDueSide.reset();
    if (mParams.flatten_at_close && mPosition.Long() && !mPosition.Busy()) {
        Trade(context, Side::kSell);
    }
}

void MeanReversion::Trade(Context& context, Side side) {
    if (side == Side::kBuy) {
        mPosition.Buy(context);
    } else if (!mPosition.Sell(context) && !mPosition.Long()) {
        Apply(context,
              SingleOrderPosition::Change::kExited);  // dust: flat at once
    }
}

}  // namespace kestrel::strategy
