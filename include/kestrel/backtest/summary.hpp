#pragma once

#include <chrono>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "kestrel/types/decimal.hpp"
#include "kestrel/types/error.hpp"
#include "kestrel/types/market.hpp"
#include "kestrel/types/orders.hpp"
#include "kestrel/types/symbol.hpp"
#include "kestrel/types/time.hpp"

namespace kestrel::backtest {

// The account at one moment of a backtest, valued in the quote asset.
struct EquityPoint {
    types::Timestamp time;
    types::Decimal equity;       // every balance at the marks below
    types::Decimal hold_equity;  // the starting balances at the same marks
    // In the order of BacktestSummary::symbols: each symbol's mark and the
    // base asset held.
    std::vector<types::Decimal> marks;
    std::vector<types::Decimal> held;
};

struct SymbolSummary {
    types::Symbol symbol;
    std::string base;
    types::Decimal first_mark;  // also the cost basis of the starting holding
    types::Decimal last_mark;
    types::Decimal start_held;  // base asset
    types::Decimal min_held;
    types::Decimal max_held;
    types::Decimal end_held;
};

// What a backtest made or lost. Amounts are in `quote_asset` and kept to 8
// decimal places; fractions are of the starting equity (return) or of the peak
// (drawdown), so 0.01 is 1%.
struct BacktestSummary {
    std::string quote_asset;
    std::vector<SymbolSummary> symbols;

    // From the first moment every balance held had a price to the end.
    types::Timestamp start_time;
    types::Timestamp end_time;
    types::Decimal start_equity;  // the starting balances at the first marks
    types::Decimal end_equity;
    types::Decimal return_fraction;
    // The starting balances valued at the last marks: what holding them,
    // without trading, would have ended with. end_equity - hold_equity is what
    // trading added.
    types::Decimal hold_equity;

    // The largest fall from a peak, checked at every price replayed.
    types::Decimal max_drawdown;
    types::Decimal max_drawdown_fraction;
    types::Timestamp drawdown_peak_time;
    types::Timestamp drawdown_trough_time;

    // Average cost, as PnlTracker works it out, with the starting holding at
    // its symbol's first mark. Fees are included. unrealized is the rest of
    // the change in equity.
    types::Decimal realized;
    types::Decimal unrealized;

    std::size_t fills = 0;
    std::size_t buys = 0;
    std::size_t sells = 0;
    std::size_t maker_fills = 0;
    std::size_t taker_fills = 0;
    types::Decimal volume;  // sum of price * quantity
    types::Decimal fees;    // in the quote asset (a base-asset fee at its fill
                            // price)
    // Sells of something held, and those whose realized P&L (fee included)
    // was above zero: the rule RiskGatedVenue counts losing sells by.
    std::size_t closing_sells = 0;
    std::size_t winning_sells = 0;

    // Sampled: the first moment equity is known, then every
    // `sample_interval` from the backtest's start, and the end.
    std::vector<EquityPoint> curve;
};

// Builds a BacktestSummary while a backtest replays. Call Observe after every
// bar or quote with that event's price and the venue's balances, then Finish
// with the run's fills. Not thread-safe.
class SummaryBuilder {
   public:
    // `symbols` are the strategy's; an equity symbol takes
    // `default_quote_asset` as its quote. kInvalidArgument when the symbols do
    // not share a quote asset, or a starting balance is neither that asset nor
    // a symbol's base, so it could not be valued. A `sample_interval` of zero
    // keeps no curve but the first and last points.
    static types::Result<SummaryBuilder> Create(
        const std::vector<types::Symbol>& symbols,
        const std::string& default_quote_asset,
        const std::map<std::string, types::Decimal>& start_balances,
        types::Timestamp start, std::chrono::milliseconds sample_interval);

    // `mark` is the price `symbol` is valued at from now on (none: keep the
    // last one); `balances` are the venue's after the event. A curve point
    // due before `time` is taken from the state before this call.
    void Observe(types::Timestamp time, const types::Symbol& symbol,
                 std::optional<types::Decimal> mark,
                 const std::vector<types::Balance>& balances);

    // kNotFound if no moment had a price for every balance held.
    types::Result<BacktestSummary> Finish(
        types::Timestamp end, const std::vector<types::Fill>& fills);

    // Mid of the bid and ask, or the last trade when a side is missing.
    static std::optional<types::Decimal> MarkOf(const types::Quote& quote);

   private:
    SummaryBuilder() = default;

    std::optional<types::Decimal> Value(
        const std::map<std::string, types::Decimal>& balances) const;
    EquityPoint Point(types::Timestamp time) const;

    BacktestSummary mSummary;
    std::map<std::string, types::Decimal> mStartBalances;
    std::map<std::string, types::Decimal> mBalances;
    std::vector<std::optional<types::Decimal>> mMarks;  // by symbol index
    types::Timestamp mStart;
    std::chrono::milliseconds mInterval{0};
    types::Timestamp mNextSample;
    bool mValued = false;  // start_equity is known
    types::Decimal mEquity;
    types::Decimal mPeak;
    types::Timestamp mPeakTime;
};

}  // namespace kestrel::backtest
