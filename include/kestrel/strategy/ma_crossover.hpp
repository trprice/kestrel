#pragma once

#include <cstddef>

#include "kestrel/strategy/strategy.hpp"
#include "kestrel/types/decimal.hpp"

namespace kestrel::strategy {

struct MaCrossoverParams {
    // kSma: simple averages (Sma); kEma: exponential ones (Ema), which react
    // to a turn sooner.
    enum class Average { kSma, kEma };
    types::Symbol symbol;
    types::Timeframe timeframe = types::Timeframe::kH1;
    std::size_t fast_period = 10;
    std::size_t slow_period = 30;
    Average average = Average::kSma;
    types::Decimal quantity;  // base-asset units per order
};

// Long-only moving-average crossover on closed bars: market-buys when the
// fast average crosses above the slow one and market-sells when it crosses
// back below. The averages are simple, or with Average::kEma exponential,
// computed over a window of four slow periods so they settle. Meant as a worked
// example of the Strategy interface, not as a strategy to trade.
//
// It goes long or flat when the order's fill arrives (OnFill), and places no
// new order while one is outstanding. An order the venue accepted and later
// cancelled, rejected or expired without filling (OnOrderUpdate) stops the
// wait.
class MaCrossover : public Strategy {
   public:
    // Throws std::invalid_argument unless 0 < fast_period < slow_period and
    // quantity > 0.
    explicit MaCrossover(MaCrossoverParams params);

    std::string_view Name() const override { return "ma_crossover"; }
    Requirements Needs() const override;
    void OnBar(Context& context, const types::Symbol& symbol,
               types::Timeframe timeframe, const types::Bar& bar) override;
    void OnFill(Context& context, const types::Fill& fill) override;
    void OnOrderUpdate(Context& context,
                       const types::OrderStatus& status) override;

   private:
    void Trade(Context& context, types::Side side);

    MaCrossoverParams mParams;
    bool mLong = false;
    bool mPending = false;  // an order was accepted and has not filled
    std::string mOrderId;   // the pending order
};

}  // namespace kestrel::strategy
