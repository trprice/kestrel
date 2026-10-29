#pragma once

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "kestrel/backtest/replay_data_source.hpp"
#include "kestrel/data/data_source.hpp"
#include "kestrel/db/bar_store.hpp"
#include "kestrel/db/market_trade_store.hpp"
#include "kestrel/db/quote_store.hpp"
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
    types::Timestamp end;  // must be after start
};

struct BacktestResult {
    std::size_t bars_replayed = 0;
    std::size_t quotes_replayed = 0;
    std::vector<types::Fill> fills;
    std::vector<types::Position> positions;
    std::vector<types::Balance> balances;
};

// Where LoadReplayData looks before going to the venue. Any may be null.
struct StoredData {
    db::BarStore* bars = nullptr;
    db::QuoteStore* quotes = nullptr;
    db::MarketTradeStore* trades = nullptr;
    // Build the quotes from `trades` (QuotesFromTrades) instead of reading
    // `quotes`, for ranges where no quotes were recorded.
    bool quotes_from_trades = false;
};

// Approximate quotes rebuilt from public trades, one per trade, for replaying
// a range where no quotes were recorded. A seller taking liquidity shows that
// the best bid was at the trade's price, a buyer that the best ask was, so
// each quote carries the trade as last/last_size (at the trade's time) and the
// latest bid and ask seen that way. When a trade puts one side through the
// other, that other side is moved to one tick away. What is lost: book
// changes without a trade (the quote stays stale until the next one, so a
// strategy requotes less often than it would live), the size at the touch
// (unknown, so under kConservative a resting order at the touch fills only
// when a trade goes through its price, not at it), and a quote is
// incomplete, without a bid or an ask, until each side has traded once.
std::vector<types::Quote> QuotesFromTrades(
    const types::Symbol& symbol, const std::string& venue,
    std::span<const types::MarketTrade> trades, const types::Decimal& tick);

// Fills a ReplayDataSource with what a backtest of `needs` over [from, to)
// uses: symbol info for every symbol (always from `source`), each symbol's bars
// for every timeframe, starting early enough before `from` to fill the warmup
// window, and, if the strategy asks for quotes, each symbol's quotes in
// [from, to).
//
// Bars come from `stored.bars` (keyed by source.Name()) when its first bar
// opens within one bar of the window's start and its last bar closes within one
// bar of the window's end. Only those two ends are checked, not gaps in the
// middle. A series it covers only partly, or whose lookup fails, is fetched
// from `source` instead, and the bars fetched are saved to `stored.bars` so the
// next run finds them (a failed save is logged, not an error). Quotes can only
// come from `stored.quotes`, as venues offer no history of them; kNotFound if
// it is null or holds none for a symbol in the range. With
// `stored.quotes_from_trades` they are built from `stored.trades` instead,
// kNotFound likewise.
//
// kInvalidArgument if `to` is not after `from`; kUnsupported for a calendar
// timeframe (1M); otherwise the source's own error.
types::Result<std::shared_ptr<ReplayDataSource>> LoadReplayData(
    data::DataSource& source, const strategy::Requirements& needs,
    types::Timestamp from, types::Timestamp to, StoredData stored = {});

// Runs a strategy over the bars in `data`, using the same Runner as live
// trading and a PaperTradingVenue for execution, on one thread and a simulated
// clock, so a run is repeatable. The bars of all of the strategy's series are
// replayed in the order they close.
//
// Each time a bar closes the paper venue is given a quote at the bar's close
// (bid = ask = last), then the strategy sees the bar. So:
//   - orders a strategy places on a bar fill on that bar's close, plus the
//     paper venue's slippage and fees;
//   - resting limit and stop orders can only trigger when the venue is given
//     a quote: at a close, or (for a strategy that asks for quotes) when a
//     stored quote arrives, which can be between two closes;
//   - an order placed in OnStart is rejected, as no quote exists yet.
// A strategy that asks for quotes is also replayed through the stored quotes
// in [start, end) (AddQuotes), in arrival order and ahead of any bar closing at
// the same instant. The paper venue is then fed those quotes instead of the
// bar-close ones, so fills happen at the recorded bid and ask.
// Resting limit orders fill according to paper.limit_fill_model. The default,
// kCross, fills one as soon as the other side's quote reaches its limit, which
// flatters a strategy that quotes (a market maker): use kConservative for
// those. It fills only when the quote goes strictly through the limit, a
// recorded trade prints through it, or trades at the limit clear the queue
// ahead of the order (see LimitFillModel). Trades are visible only in stored
// quotes that carry last/last_size (Binance.US does; Kraken sends no size).
// Neither model simulates order or cancel latency.
// A calendar-length timeframe (1M) fails with kUnsupported, and an end that is
// not after the start with kInvalidArgument. `data` needs bars (and quotes if
// asked for) and SetSymbolInfo for every symbol the strategy trades.
types::Result<BacktestResult> RunBacktest(
    std::shared_ptr<ReplayDataSource> data,
    std::unique_ptr<strategy::Strategy> strategy, BacktestOptions options);

}  // namespace kestrel::backtest
