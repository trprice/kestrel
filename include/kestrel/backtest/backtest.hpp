#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "kestrel/backtest/replay_data_source.hpp"
#include "kestrel/backtest/summary.hpp"
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
    // How often the summary's equity curve is sampled; zero keeps only its
    // first and last points.
    std::chrono::milliseconds equity_interval = std::chrono::minutes(1);
    // An order placed on a bar's close trades at the next price replayed,
    // normally the next bar's open (PaperTradingVenue::SetDeferMatching), so
    // a signal computed from a close is not filled at it (immediate-or-cancel
    // and fill-or-kill orders excepted). True fills it at that close instead,
    // the optimistic model before this option.
    bool fill_at_close = false;
    // Bars of this (finer) timeframe drive the paper venue instead of the
    // strategy's own, so the path within a strategy bar is the recorded one
    // rather than the open, nearer extreme, other extreme, close guess. The
    // strategy does not see them; `data` must hold them (LoadReplayData with
    // the timeframe added to the Requirements' bar_timeframes). A strategy
    // bar with none of these bars inside it (beyond the data's reach, or a
    // gap) is walked itself, as without this option. Not for a strategy
    // that takes quotes, and no coarser than the strategy's finest
    // timeframe.
    std::optional<types::Timeframe> fill_timeframe;
};

struct BacktestResult {
    std::size_t bars_replayed = 0;
    std::size_t quotes_replayed = 0;
    // With BacktestOptions::fill_timeframe: those bars walked, and the
    // strategy bars walked themselves for want of them.
    std::size_t fill_bars_replayed = 0;
    std::size_t bars_walked_without_fill_bars = 0;
    std::vector<types::Fill> fills;
    std::vector<types::Position> positions;
    std::vector<types::Balance> balances;
    // Equity, return against holding the starting balances, drawdown, fills
    // and fees (see BacktestSummary). An error, and the rest of the result
    // still valid, when the balances could not be valued: symbols without a
    // shared quote asset, a starting balance with no price, or no price
    // replayed at all.
    types::Result<BacktestSummary> summary;
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

// Whether `fill_timeframe` (BacktestOptions::fill_timeframe) suits a strategy
// with `needs`: kUnsupported for a calendar timeframe; kInvalidArgument for a
// strategy that takes quotes, or one coarser than the strategy's finest
// timeframe. RunBacktest checks it too; call it first to fail before loading
// data.
types::Result<void> CheckFillTimeframe(
    const strategy::Requirements& needs,
    std::optional<types::Timeframe> fill_timeframe);

// Runs a strategy over the bars in `data`, using the same Runner as live
// trading and a PaperTradingVenue for execution, on one thread and a simulated
// clock, so a run is repeatable. The bars of all of the strategy's series are
// replayed in the order they close.
//
// For a strategy without quotes, the paper venue walks each bar of the
// symbol's finest series (of those with bars), or of options.fill_timeframe,
// before the strategy sees it, as quotes (bid = ask = last): the open, the
// extreme nearer the open, the other extreme and the close, a third of the
// bar apart, and on each leg every resting order's price it passes. The
// strategy runs at each point. So:
//   - orders a strategy places on a bar's close fill at the next price walked
//     (unless options.fill_at_close), plus the paper venue's slippage and
//     fees;
//   - a resting stop or limit inside a later bar fills at its own price (at
//     the start of its leg), and one the bar opens beyond, at the open; an
//     order placed on that fill (a protective stop) can fill later in the
//     same bar. Which extreme came first is a guess (the nearer one), so a bar
//     that reaches both an entry and its stop is decided by it, unless finer
//     bars (options.fill_timeframe) say which;
//   - on a market that closes, a bar is walked only through the part of it
//     in its session (a daily bar from the open to the close), and session
//     hooks due at the open run right after the open's quote, so an order
//     placed in OnSessionOpen trades against the open;
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
// Order and cancel latency are paper.order_latency / report_latency.
// The summary values the balances after every bar or quote replayed, at the
// quote's mid (the last trade when a side is missing) or, for a strategy
// without quotes, the bar's close.
// A calendar-length timeframe (1M) fails with kUnsupported; an end that is
// not after the start, or a fill_timeframe with a strategy that takes quotes
// or coarser than the strategy's finest timeframe, with kInvalidArgument.
// `data` needs bars (and quotes if asked for) and SetSymbolInfo for every
// symbol the strategy trades.
types::Result<BacktestResult> RunBacktest(
    const std::shared_ptr<ReplayDataSource>& data,
    std::unique_ptr<strategy::Strategy> strategy,
    const BacktestOptions& options);

}  // namespace kestrel::backtest
