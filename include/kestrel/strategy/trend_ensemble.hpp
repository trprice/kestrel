#pragma once

#include <chrono>
#include <cstddef>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "kestrel/strategy/spot_book.hpp"
#include "kestrel/strategy/strategy.hpp"
#include "kestrel/types/decimal.hpp"

namespace kestrel::strategy {

// The parameters are described in docs/strategies/trend-ensemble.md.
struct TrendEnsembleParams {
    std::vector<types::Symbol> symbols;  // sharing one quote asset, the cash
    // The cash of symbols without a quote asset (stocks, "SPY"): `[paper]
    // default_quote_asset` in paper mode, else USD.
    std::string default_quote_asset = "USD";
    types::Timeframe timeframe = types::Timeframe::kD1;
    // The Donchian channel lengths, one trend model each.
    std::vector<std::size_t> lookbacks = {5, 10, 20, 30, 60, 90, 150, 250, 360};
    std::size_t vol_bars = 90;         // bars of returns for the volatility
    types::Decimal target_vol{25, 2};  // annualized, of each symbol's share
    // Trading days a year, to annualize the volatility of daily bars: 365
    // for crypto, about 252 for US stocks. Other timeframes are taken as
    // trading around the clock, so they need 365.
    std::size_t days_per_year = 365;
    // The most of a symbol's share a model may hold (spot: no more than 1).
    types::Decimal max_weight{1, 0};
    // A holding off its target by more than this fraction of the target is
    // resized; a change of trend signal always trades.
    types::Decimal rebalance_threshold{20, 2};
    // Of equity, split equally over the symbols: what all of them may hold.
    types::Decimal invest_fraction{95, 2};
    // Count the coins of the universe the account holds at startup as held
    // (so a restarted run sells or keeps them); for an account of its own.
    bool adopt_holdings = false;
};

// An ensemble of Donchian channel trend models on each symbol, sized to a
// target volatility, long only; see docs/strategies/trend-ensemble.md.
//
// Each model (symbol, lookback n) goes long on a close at the highest close
// of the last n bars and exits on a close at or below a trailing stop: the
// channel's midpoint at entry, raised (never lowered) to each later
// midpoint. A symbol's weight is the share of its models that are long times
// min(target_vol / volatility, max_weight), of its equal share of the
// equity. The models replay the warm-up bars at startup (three times the
// longest lookback), so a restart carries on where the last run was.
//
// Trades happen once a period's bars are all in: sells first, buys once
// their fills are seen, as those pay for them.
class TrendEnsemble : public Strategy {
   public:
    // Throws std::invalid_argument for parameters out of range.
    explicit TrendEnsemble(TrendEnsembleParams params);

    std::string_view Name() const override { return "trend_ensemble"; }
    Requirements Needs() const override;
    void OnStart(Context& context) override;
    void OnBar(Context& context, const types::Symbol& symbol,
               types::Timeframe timeframe, const types::Bar& bar) override;
    void OnFill(Context& context, const types::Fill& fill) override;
    void OnOrderUpdate(Context& context,
                       const types::OrderStatus& status) override;
    void OnSessionOpen(Context& context) override;

    // The share of a symbol's models that are long, 0 to 1; for tests.
    types::Decimal Conviction(const types::Symbol& symbol) const;

   private:
    struct Model {
        bool long_ = false;
        types::Decimal stop;  // while long
    };
    struct SymbolState {
        std::vector<Model> models;             // one per lookback
        std::optional<types::Timestamp> last;  // the newest bar counted
        // Models long when an order last took it toward its target: a change
        // trades whatever the threshold. The count, not which models: one in
        // and another out leaves the weight as it was.
        std::optional<std::size_t> traded_long;
    };

    // Counts the bars of `symbol` after the last counted one into its models.
    void Advance(Context& context, const types::Symbol& symbol);
    // The longest space between bars that is not a halt.
    std::chrono::seconds MaxGap() const;
    // The index of the first bar after the last gap (a halt) at or before
    // `upto`, or 0 without one.
    std::size_t SegmentStart(std::span<const types::Bar> bars,
                             std::size_t upto) const;
    std::size_t LongModels(const types::Symbol& symbol) const;
    // The symbol's target, a fraction of equity; nullopt without a
    // volatility yet.
    std::optional<types::Decimal> Weight(Context& context,
                                         const types::Symbol& symbol) const;
    void Rebalance(Context& context, types::Timestamp period);
    // Enter() once no sell is out and every sell's fill has been seen.
    void TryEnter(Context& context);
    void Enter(Context& context);
    // The rebalance put off for orders still out, once none is.
    void RunPending(Context& context);

    TrendEnsembleParams mParams;
    SpotBook mBook;
    PeriodGate mGate;
    std::chrono::seconds mStep{};  // a bar's length
    std::size_t mPeriodsPerYear;   // bars of timeframe a trading year
    std::map<types::Symbol, SymbolState> mState;
    std::optional<types::Timestamp> mDue;      // waits for the market to open
    std::optional<types::Timestamp> mPending;  // waits for orders to close
    // Buys waiting for the sells: each symbol's target value, and its models
    // long then.
    struct Entry {
        types::Decimal target;
        std::size_t longs = 0;
        bool near = false;  // already within the threshold of the target
    };
    std::map<types::Symbol, Entry> mEntries;
    // Symbols being sold, and what is left held once the sell has filled.
    std::map<types::Symbol, types::Decimal> mSelling;
};

}  // namespace kestrel::strategy
