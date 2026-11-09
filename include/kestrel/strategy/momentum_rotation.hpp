#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

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
};

// Cross-sectional momentum, long only; see
// docs/strategies/momentum-rotation.md.
//
// A period is ranked only once every symbol's bar for it is in (or the next
// period has begun), since the bars arrive one at a time and ranking on a
// partial set would compare returns over different times. Entries wait for
// the sells' fills, as those fills are what pays for them. Only what the
// strategy bought itself counts as held: the account's other balances may
// belong to someone else.
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
    // Drops orders the venue has closed without an update reaching us;
    // whether none is left.
    bool Reconcile(Context& context);
    // What of `symbol` can be sold: held, to the lot size; zero for dust
    // under the minimum order.
    types::Decimal Sellable(Context& context,
                            const types::Symbol& symbol) const;
    // The quote balance plus holdings at their last close.
    std::optional<types::Decimal> Equity(Context& context) const;
    std::optional<types::Decimal> LastClose(Context& context,
                                            const types::Symbol& symbol) const;
    // Places a market order; its ID if the venue took it and it is active.
    std::optional<std::string> Place(Context& context,
                                     const types::Symbol& symbol,
                                     types::Side side,
                                     const types::Decimal& quantity);

    MomentumRotationParams mParams;
    std::string mQuote;                          // the shared quote asset
    std::map<types::Symbol, std::string> mBase;  // each symbol's base asset
    // The period being collected: its bars' open time, and the symbols with
    // a bar for it so far.
    std::optional<types::Timestamp> mPeriod;
    std::set<types::Symbol> mSeen;
    bool mDone = false;        // the period has been counted
    std::size_t mPeriods = 0;  // periods counted, for rebalance_bars
    // The period of the last ranking: a symbol without a bar for it sits the
    // ranking out.
    types::Timestamp mRanked;
    bool mDue = false;  // a rebalance waits for the market to open
    std::map<types::Symbol, types::Decimal> mHeld;  // bought, not yet sold
    // Active orders by ID, and whether each is a sell.
    std::map<std::string, bool> mOrders;
    std::vector<types::Symbol> mEntries;  // to buy once the sells are done
    // Symbols being sold, and what is left held once the sell has filled.
    std::map<types::Symbol, types::Decimal> mSelling;
};

}  // namespace kestrel::strategy
