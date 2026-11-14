#pragma once

#include <cstddef>
#include <optional>
#include <string>

#include "kestrel/strategy/single_order_position.hpp"
#include "kestrel/strategy/strategy.hpp"
#include "kestrel/types/decimal.hpp"

namespace kestrel::strategy {

// The parameters are described in docs/strategies/mean-reversion.md.
struct MeanReversionParams {
    types::Symbol symbol;
    types::Decimal quantity;  // base-asset units per order
    types::Timeframe timeframe = types::Timeframe::kH1;
    std::size_t rsi_period = 14;
    types::Decimal oversold{30, 0};
    types::Decimal overbought{70, 0};
    std::size_t bollinger_period = 0;  // zero: off
    types::Decimal bollinger_width{2, 0};
    std::size_t exit_sma = 0;   // zero: off
    std::size_t trend_sma = 0;  // zero: off
    types::Decimal atr_stop;    // zero: off
    std::size_t atr_bars = 14;
    std::size_t max_hold_bars = 0;  // zero: never
    bool flatten_at_close = false;
};

// RSI and Bollinger Band mean reversion, covering both classic RSI /
// Bollinger pullbacks and short-horizon Connors RSI(2) dips; see
// docs/strategies/mean-reversion.md.
//
// The orders go through a SingleOrderPosition: no order is placed while one
// is working, and the position changes on the fills, not on the order. An
// order due while the market is closed is sent at the next open, an entry
// only if the bars since have not cancelled its signal.
class MeanReversion : public Strategy {
   public:
    // Throws std::invalid_argument for parameters out of range.
    explicit MeanReversion(MeanReversionParams params);

    std::string_view Name() const override { return "mean_reversion"; }
    Requirements Needs() const override;
    void OnBar(Context& context, const types::Symbol& symbol,
               types::Timeframe timeframe, const types::Bar& bar) override;
    void OnFill(Context& context, const types::Fill& fill) override;
    void OnOrderUpdate(Context& context,
                       const types::OrderStatus& status) override;
    void OnSessionOpen(Context& context) override;
    void OnSessionClosing(Context& context, types::Timestamp close) override;

   private:
    // context.Log() with symbol=<symbol>, which the runner's has already;
    // the messages leave the symbol out.
    LogContext Log(Context& context) const;

    void Trade(Context& context, types::Side side);
    void Apply(Context& context, SingleOrderPosition::Change change);

    MeanReversionParams mParams;
    SingleOrderPosition mPosition;
    types::Decimal mEntryAtr;   // the ATR when the entry was signalled
    types::Decimal mStopPrice;  // zero: none
    std::size_t mBarsHeld = 0;
    std::optional<types::Side> mDueSide;  // to send at the next open
};

}  // namespace kestrel::strategy
