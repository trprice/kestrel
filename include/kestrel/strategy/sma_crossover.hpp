#pragma once

#include <cstddef>

#include "kestrel/strategy/strategy.hpp"
#include "kestrel/types/decimal.hpp"

namespace kestrel::strategy {

struct SmaCrossoverParams {
    types::Symbol symbol;
    types::Timeframe timeframe = types::Timeframe::kH1;
    std::size_t fast_period = 10;
    std::size_t slow_period = 30;
    types::Decimal quantity;  // base-asset units per order
};

// Long-only moving-average crossover on closed bars: market-buys when the
// fast average crosses above the slow one and market-sells when it crosses
// back below. Meant as a worked example of the Strategy interface, not as a
// strategy to trade.
//
// It goes long or flat when the order's fill arrives (OnFill), and places no
// new order while one is outstanding. A market order that is accepted but never
// fills would leave it waiting forever.
class SmaCrossover : public Strategy {
   public:
    // Throws std::invalid_argument unless 0 < fast_period < slow_period and
    // quantity > 0.
    explicit SmaCrossover(SmaCrossoverParams params);

    std::string_view Name() const override { return "sma_crossover"; }
    Requirements Needs() const override;
    void OnBar(Context& context, const types::Symbol& symbol,
               types::Timeframe timeframe, const types::Bar& bar) override;
    void OnFill(Context& context, const types::Fill& fill) override;

   private:
    void Trade(Context& context, types::Side side);

    SmaCrossoverParams mParams;
    bool mLong = false;
    bool mPending = false;  // an order was accepted and has not filled
};

}  // namespace kestrel::strategy
