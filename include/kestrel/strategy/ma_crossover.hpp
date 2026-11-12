#pragma once

#include <cstddef>

#include "kestrel/strategy/single_order_position.hpp"
#include "kestrel/strategy/strategy.hpp"
#include "kestrel/types/decimal.hpp"

namespace kestrel::strategy {

// The parameters are described in docs/strategies/ma-crossover.md.
struct MaCrossoverParams {
    enum class Average { kSma, kEma };
    types::Symbol symbol;
    types::Timeframe timeframe = types::Timeframe::kH1;
    std::size_t fast_period = 10;
    std::size_t slow_period = 30;
    Average average = Average::kSma;
    types::Decimal quantity;  // base-asset units per order
};

// A moving-average crossover, the worked example of the Strategy interface;
// see docs/strategies/ma-crossover.md.
//
// The orders go through a SingleOrderPosition: no order is placed while one
// is working, so a cross back and forth inside one order's fills cannot stack
// two orders, and the position changes on the fills, not on the order.
class MaCrossover : public Strategy {
   public:
    // Throws std::invalid_argument for parameters out of range.
    explicit MaCrossover(MaCrossoverParams params);

    std::string_view Name() const override { return "ma_crossover"; }
    Requirements Needs() const override;
    void OnBar(Context& context, const types::Symbol& symbol,
               types::Timeframe timeframe, const types::Bar& bar) override;
    void OnFill(Context& context, const types::Fill& fill) override;
    void OnOrderUpdate(Context& context,
                       const types::OrderStatus& status) override;

   private:
    MaCrossoverParams mParams;
    SingleOrderPosition mPosition;
};

}  // namespace kestrel::strategy
