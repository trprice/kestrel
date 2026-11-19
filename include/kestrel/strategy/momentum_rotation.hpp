#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include "kestrel/strategy/spot_book.hpp"
#include "kestrel/strategy/strategy.hpp"
#include "kestrel/types/decimal.hpp"

namespace kestrel::strategy {

// The parameters are described in docs/strategies/momentum-rotation.md.
struct MomentumRotationParams {
    std::vector<types::Symbol> symbols;  // sharing one quote asset, the cash
    types::Timeframe timeframe = types::Timeframe::kD1;
    std::size_t lookback_bars = 28;
    std::size_t rebalance_bars = 7;
    std::size_t top_n = 3;
    bool positive_only = true;
    types::Decimal invest_fraction{95, 2};  // 0.95
    // Count the coins of the universe the account holds at startup as held
    // (so a restarted run sells or keeps them); for an account of its own.
    bool adopt_holdings = false;
};

// Cross-sectional momentum, long only; see
// docs/strategies/momentum-rotation.md.
//
// A period is ranked only once every symbol's bar for it is in (or the next
// period has begun), since the bars arrive one at a time and ranking on a
// partial set would compare returns over different times. Entries wait for
// the sells' fills, as those fills are what pays for them. Only what the
// strategy bought itself counts as held, as the account's other balances may
// belong to someone else, unless adopt_holdings says the account is its own.
class MomentumRotation : public Strategy {
   public:
    // Throws std::invalid_argument for parameters out of range.
    explicit MomentumRotation(MomentumRotationParams params);

    std::string_view Name() const override { return "momentum_rotation"; }
    Requirements Needs() const override;
    void OnBar(Context& context, const types::Symbol& symbol,
               types::Timeframe timeframe, const types::Bar& bar) override;
    void OnFill(Context& context, const types::Fill& fill) override;
    void OnOrderUpdate(Context& context,
                       const types::OrderStatus& status) override;
    void OnSessionOpen(Context& context) override;
    void OnStart(Context& context) override;

   private:
    // A period's bars are in: count it, and rebalance every rebalance_bars.
    void Period(Context& context, types::Timestamp open_time);
    void Rebalance(Context& context);
    // Enter() once no sell is out and every sell's fill has been seen.
    void TryEnter(Context& context);
    // Buys mEntries.
    void Enter(Context& context);

    MomentumRotationParams mParams;
    SpotBook mBook;
    PeriodGate mGate;
    std::size_t mPeriods = 0;  // periods counted, for rebalance_bars
    // The period of the last ranking: a symbol without a bar for it sits the
    // ranking out.
    types::Timestamp mRanked;
    bool mDue = false;  // a rebalance waits for the market to open
    std::vector<types::Symbol> mEntries;  // to buy once the sells are done
    // Symbols being sold, and what is left held once the sell has filled.
    std::map<types::Symbol, types::Decimal> mSelling;
};

}  // namespace kestrel::strategy
