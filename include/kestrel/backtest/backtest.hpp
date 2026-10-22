#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "kestrel/backtest/replay_data_source.hpp"
#include "kestrel/strategy/strategy.hpp"
#include "kestrel/trading/paper_trading_venue.hpp"
#include "kestrel/trading/risk_gate.hpp"

namespace kestrel::backtest {

struct BacktestOptions {
    trading::PaperOptions paper;  // balances, fees and slippage
    std::optional<trading::RiskLimits> risk;
    // Bars of the strategy's series with start <= open_time < end are
    // replayed. Earlier bars in the data source are available as warmup.
    types::Timestamp start;
    types::Timestamp end;
};

struct BacktestResult {
    std::size_t bars_replayed = 0;
    std::vector<types::Fill> fills;
    std::vector<types::Position> positions;
    std::vector<types::Balance> balances;
};

// Runs a strategy over the bars in `data`, using the same Runner as live
// trading and a PaperTradingVenue for execution, on one thread and a simulated
// clock, so a run is repeatable. The bars of all of the strategy's series are
// replayed in the order they close.
//
// Each time a bar closes the paper venue is given a quote at the bar's close
// (bid = ask = last), then the strategy sees the bar. So:
//   - orders a strategy places on a bar fill on that bar's close, plus the
//     paper venue's slippage and fees;
//   - resting limit and stop orders can only trigger at closes, never on an
//     intrabar high or low;
//   - an order placed in OnStart is rejected, as no quote exists yet.
// Only strategies that work from bars are supported: one that asks for quotes,
// or a calendar-length timeframe (1M), fails with kUnsupported. `data` needs
// bars and SetSymbolInfo for every symbol the strategy trades.
types::Result<BacktestResult> RunBacktest(
    std::shared_ptr<ReplayDataSource> data,
    std::unique_ptr<strategy::Strategy> strategy, BacktestOptions options);

}  // namespace kestrel::backtest
