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

struct MomentumRotationParams {
    // The universe; all must share one quote asset (the cash).
    std::vector<types::Symbol> symbols;
    types::Timeframe timeframe = types::Timeframe::kD1;
    std::size_t lookback_bars = 28;  // momentum: the return over these bars
    std::size_t rebalance_bars = 7;  // rebalance every this many bars
    std::size_t top_n = 3;           // hold this many of the strongest
    // Hold only symbols whose momentum is above zero (absolute momentum), so
    // in a falling market the strategy sits in cash.
    bool positive_only = true;
    // The share of equity (cash plus holdings at the last close) invested,
    // split equally over top_n slots; the rest is room for fees and moves
    // between the sizing and the fill.
    types::Decimal invest_fraction{95, 2};  // 0.95
};

// Cross-sectional momentum, long only: every rebalance_bars bars, rank the
// symbols by their return over lookback_bars and hold the top_n (with
// positive_only, only those that rose), an equal share of equity in each.
// Momentum is the anomaly that holds up best in crypto research (Yang,
// "Behavioral Anomalies in Cryptocurrency Markets"; Quantpedia's crypto
// summaries).
//
// Bars of the symbols arrive one at a time: a period is ranked once every
// symbol's bar for it has closed, or, if one is missing, when a bar of the
// next period arrives. The lookback is measured in time: a symbol without a
// bar for the ranked period, or for lookback_bars before it, sits that
// ranking out, and one held then is kept (with its slot) rather than sold.
// With fewer than half the symbols ranked there is no rebalance.
// A rebalance sells what was ranked out of the top at market first, and buys
// what entered it at market into the slots left, once those sells have
// filled and their fills been seen (or the sells closed), sized from equity
// then and capped by the cash available. Symbols still in the top are kept
// as they are, not resized. Holdings are what the strategy bought itself
// (from its fills, less fees in the base asset), not the account's other
// balances, which OnStart warns about; dust under the minimum order counts
// as nothing held. Orders still out at a rebalance are checked with the
// venue; if one is still open, that rebalance is skipped. While the market
// is closed (daily stock bars) a rebalance waits for the next session's
// open.
class MomentumRotation : public Strategy {
   public:
    // Throws std::invalid_argument for fewer than two symbols, one listed
    // twice, symbols with
    // different quote assets (or none), zero lookback_bars, rebalance_bars or
    // top_n, a top_n above the number of symbols, a calendar timeframe, or
    // an invest_fraction outside (0, 1].
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
