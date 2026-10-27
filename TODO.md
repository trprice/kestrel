# Kestrel TODO

## Known limitations

Things that work but have edges a user could trip over. Each is also noted in
the header of the code concerned.

### Strategy runner (`strategy/runner.hpp`)
- Quotes are conflated per symbol, but the event queue is otherwise unbounded:
  a strategy slower than the bar stream still falls behind (bars are never
  dropped).
- Fills are found by polling `GetFills`, not pushed, so `OnFill` can lag the
  fill by up to `fill_poll_interval` (default 1 s).
- The fill poll starts from the time `Begin()` ran and assumes the venue's fill
  times and the runner's clock roughly agree. A fill stamped earlier than the
  start (clock skew) is missed.
- Calendar timeframes (`1M`) get no gap backfill, and warmup sizing for them
  uses a 31-day estimate.
- One `Runner` drives one strategy; running several means one runner (and one
  set of data subscriptions) each.
- `OnOrderUpdate` is found by polling (`GetOpenOrders`, plus `GetOrder` for an
  order that left the list) at `fill_poll_interval`, so a quick state change
  can be seen late or have intermediate states skipped. An order that vanishes
  with `kNotFound` is dropped without an update. A strategy that needs to know
  about an order must have placed it through the runner's venue.
- `Stop()` is not async-signal-safe (it takes a mutex). `kestrel_app run`
  calls it from a watch thread that collects SIGINT/SIGTERM with
  `sigtimedwait`; any other driver must do the same.

### Risk gate (`trading/risk_gate.hpp`)
- Optional in `RunnerOptions`. Nothing forces it on for live trading.
- Daily loss and drawdown are summed across symbols, so they must share a
  quote asset.
- `max_drawdown` is sampled when an order is attempted: a peak or trough
  between orders is not seen. It and the other loss limits block entries only;
  they do not close a position that is already open.
- `max_consecutive_losses` counts losing sell fills, so a sell that fills in
  several pieces counts each losing piece.
- Daily loss counts only fills at or after the baseline call, relying on the
  venue's and the gate's clocks roughly agreeing. A fill landing between the
  positions snapshot and the baseline time is missed.
- A fee in an asset that is neither the quote nor the base (e.g. BNB) is
  treated as if it were quote currency.
- Market orders are valued at the last reference price (bar close or quote
  mid). `max_price_age` is unset by default, so a price never goes stale.
- Spot only: a sell larger than the holding is treated as not reducing a
  position, and shorting is not modelled.
- Checks and the venue call run under one lock, so concurrent orders are
  serialized (intended), and a slow venue call delays other orders.
- `Halt()` cancels at once, then waits for any order already inside
  `PlaceOrder` to land and cancels again. A slow venue call therefore delays
  `Halt` returning (not the first cancel). A position-reducing limit order
  placed after the halt is allowed and stays open.

### P&L (`trading/pnl_tracker.hpp`)
- Average-cost accounting only. Brokers and tax reports often use FIFO or
  specific-lot; figures will differ.
- Amounts are kept to 8 decimal places.
- A sell of more than is held realizes nothing on the excess (no known cost
  basis).
- `PaperTradingVenue` reports positions only while open, so a closed
  position's realized P&L is not visible through `GetPositions`.

### Paper venue (`trading/paper_trading_venue.hpp`)
- Closed orders are kept for `GetOrder` and never dropped, so memory grows
  with every order placed (a quoting strategy places thousands per hour).
  Prune closed orders once the runner has reported their final state or after
  an age.
- Fill history grows without bound and `GetFills` scans all of it on every
  call; both the runner's fill poll and the risk gate's daily-loss check hit
  that path, so a paper venue left running for weeks gets slower and leaks
  memory. Cap the history (or index it by time) rather than rescanning.
- Market orders fill the whole remaining quantity against a single quote with
  fixed-bps slippage: no book depth and no size-aware slippage, so paper
  results flatter anything above retail size. Limit fills can be capped at
  the touch (`cap_limit_fills_at_quote_size`); market fills should have the
  same option.

### Market maker (`strategy/market_maker.hpp`)
- No volatility scaling of the spread, and no pulling of quotes when the feed
  goes quiet: it only acts when a quote arrives.
- Inventory is the base-asset balance at start and the fills since; a fee
  charged in the base asset (Binance without BNB) is not deducted from it.
- Each requote goes to the back of the queue, so a tight `requote_bps` costs
  fills under `limit_fill_model = "conservative"`. The ZEC/USDT example
  replaces a quote on about every other update (29.5k placements for 51k
  quotes).
- Backtest ordering: a quote is matched against resting orders first, then
  the strategy sees it, and the runner delivers the fills afterwards. So
  on the quote that fills an order the strategy still believes it is resting
  (its cancel loses the race, see `cancel_raced`). This resembles fill-poll
  latency but is exactly one event, not a time.

### Backtesting (`backtest/backtest.hpp`)
- Monthly timeframes are rejected with `kUnsupported`. Strategies that need
  quotes replay stored quotes (`QuoteStore`); there are no stored quotes for
  a range that was never recorded.
- Resting limit orders fill per `[paper] limit_fill_model`. `cross` (the
  default) is optimistic for quoting strategies. `conservative` models queue
  position from trades, so it needs stored quotes carrying `last`/`last_size`
  (Binance.US does; Kraken's ticker has no trade size, so only price-through
  fills happen there). It assumes our order joins at the back of the displayed
  size, ignores order and cancel latency, takes no account of our own orders
  moving the market, and cannot see trades that print at our price while a
  better-priced level still stands. Identical back-to-back trades (same price,
  size and exchange time) look like one.
- Orders fill at the close of the bar that triggered them, plus slippage and
  fees. Resting limit and stop orders can only trigger at closes, never on an
  intrabar high or low.
- An order placed in `OnStart` is rejected because no quote exists yet.
- The result is raw fills, positions and balances. There is no equity curve,
  drawdown or other summary statistics.
- Symbol info (tick, lot, minimum size) must be supplied by hand for every
  symbol.

### Venues and data
- Kraken, Schwab and Binance.US trading venues are stubs that return
  `kNotImplemented`.
- The Schwab data source is a stub.
- `PaperTradingVenue` is spot only: no shorting, no leverage.
- `run` refuses live mode on a provider whose venue is a stub
  (`ValidateForRun`, with `HasLiveVenue` in `factory.cpp` to update as venues
  land), but `CreateComponents` still builds the stub, so `bars` in live mode
  runs until the first order-related call fails with `kNotImplemented`.
- Stream liveness is reconnect-only: `StreamQuotes`/`StreamBars` re-subscribe
  after disconnects, but nothing notices a half-open socket that quietly
  stops delivering, or a feed gone stale. The health-check item under
  Operations covers monitoring; a stale-feed watchdog that pauses (or halts)
  the strategy is a trading-safety feature and belongs with the risk work.

## In progress: strategy system and risk gate

Built and tested with fakes (206 tests). Not yet run against a live venue, and
reachable from `kestrel_app run` and `backtest`.

### Done
- [x] `Strategy` / `Context` / `Requirements` interface
- [x] `Runner`: queue, warmup, gap backfill, duplicate bars dropped, hook
      exceptions contained, fill polling, `Begin`/`Drain`/`End` for
      deterministic drivers
- [x] `SmaCrossover` example strategy
- [x] `OnOrderUpdate` hook and `TradingVenue::GetOrder`; the runner tracks the
      strategy's active orders and reports state changes; `SmaCrossover` stops
      waiting on an order that closed without a fill
- [x] Per-symbol quote conflation in the runner
- [x] `RiskGatedVenue::Halt` cancels again after an in-flight order lands
- [x] `RiskGatedVenue`: order and position notional, open orders, order rate,
      daily loss, symbol allowlist, price staleness, kill switch
- [x] `PnlTracker` as the single P&L implementation, used by both the paper
      venue and the risk gate
- [x] `ReplayDataSource` and `RunBacktest`
- [x] CLI subcommands `run <strategy>` and `backtest <strategy> --from --to`
      (one class per command in `app/commands/`); SIGINT/SIGTERM stop the
      runner through a watch thread
- [x] Per-strategy risk limits (`[risk.<strategy>]` over `[risk]`), and
      `max_orders_per_day`, `max_consecutive_losses` and `max_drawdown`
- [x] `[risk]` and `[strategy]` config sections (unknown keys are errors);
      strategy registry (`CreateStrategy`) in `factory.cpp`
- [x] Live runs need `[risk]` limits and a non-stub venue, checked at startup
      (`ValidateForRun`); the daily-loss baseline is started at startup and a
      failure is fatal
- [x] `CreateRunner` (factory) builds a ready-to-run runner and takes its
      provider connection as an argument, so the paper, no-limits and
      failed-baseline paths are tested with fakes; `LoadReplayData`
      (backtest) and `types::ParseTimestamp` hold the logic the `backtest`
      command used to carry; `cli_*` tests check the exit code and message
      for bad arguments

### To finish
- [ ] Run the runner against Binance.US data end to end in paper mode (the only
      live data source that is complete) and fix what that shakes out. (It
      starts, streams and stops cleanly; a long run that actually trades has
      not been done.)
- [ ] `run` does not cancel open orders on shutdown; decide whether it should
- [ ] `backtest` fetches bars from the provider on every run; load them from
      `BarStore` instead (see Backtesting and research)
- [ ] Bound the runner's bar queue (quotes are conflated per symbol)
- [ ] Cap the paper venue's fill history so long paper runs stay bounded and
      fast, and stop rescanning all of it in `GetFills`
- [ ] Backtest summary: equity curve, total return, max drawdown, trade count,
      win rate, fees paid
- [ ] Backtest fill model: fill at the next bar's open by default (filling a
      signal computed on a close at that same close is a look-ahead), keeping
      fill-on-close as an option; use bar high/low for limit and stop order
      triggering
- [ ] Document the strategy system in the README (how to write a strategy,
      threading rules, how the gate works)

## Projects

### Venues
Each stub's header has a TODO comment listing the endpoints and auth it needs.
- [ ] **Kraken trading venue.** REST orders, balances, open orders and trade
      history, signed with the existing HMAC helper. Data source is already
      done.
- [ ] **Schwab data source and trading venue.** OAuth with the token store
      that already exists, bars, quotes and orders. Needs market-hours handling
      for equities.
- [ ] **Binance.US trading venue.** The data source is done; the venue is a
      stub.
- [ ] A user-data stream (WebSocket) for order and fill updates on venues that
      offer one, replacing fill polling.
- [ ] Re-verify the timeframe lists for Schwab, Kraken and Binance.US against
      each provider's docs before completing the stubs. The lists in the code
      and README came from memory.

### Strategies
- [ ] Agent access (listed as out of scope in the README). What it should look
      like isn't defined yet; decide that before building it.
- [ ] Indicators library (SMA/EMA, RSI, ATR, Bollinger, MACD) as small
      reusable pieces so strategies do not each reimplement them
- [ ] EMA crossover and RSI mean-reversion
- [ ] Breakout / Donchian channel with an ATR stop
- [ ] Grid trading and dollar-cost averaging
- [ ] Multi-symbol strategies (pairs trading, momentum rotation across a basket)
- [ ] Position sizing helpers: fixed fractional, volatility-targeted

### Backtesting and research
- [ ] Parameter sweeps and walk-forward testing
- [ ] Load backtest bars from the database (`BarStore`) instead of building
      `ReplayDataSource` by hand
- [ ] Order and cancel latency in the paper venue, so a quote can fill an
      order that the strategy has already asked to cancel
- [ ] Record the aggressor side of trades (Binance `m` flag) so trades can be
      attributed to the right side of the book
- [ ] Benchmark comparison against buy-and-hold

### Risk
- [ ] Portfolio-level limits (total exposure across symbols, concentration)
- [ ] Drawdown that is measured continuously (the `max_drawdown` limit is
      sampled at order attempts only), and a version that halts and closes
      the position instead of only blocking entries
- [ ] Stop-loss support: a per-position loss limit (`max_loss_per_position`)
      and a risk-per-trade percentage need orders that carry a stop, which
      `SmaCrossover` does not place
- [ ] FIFO and specific-lot P&L options in `PnlTracker`, for venues and tax
      reports that use them

### Operations
- [ ] Persist orders, fills and strategy decisions to the database
- [ ] Alerts (email, webhook or push) when the kill switch trips or a stream
      stays down
- [ ] Health checks: stale-data detection, stream reconnect counts, clock-skew
      warning
- [ ] Structured logging, with a per-strategy log context
- [ ] Run several strategies in one process, sharing data subscriptions
- [ ] Dry-run mode for live data: real quotes and the paper venue, with a switch
      to the live venue only when explicitly enabled

### Engineering
- [ ] Building components in the factory starts the paper venue's quote
      stream as a side effect, so `bars` and `run` open a WebSocket whether or
      not they need it (`stream`, `stream-bars` and `backtest` already avoid
      this by building only the data source); make starting the venue explicit
- [ ] Exercise the cpr FetchContent fallback in `cmake/KestrelDependencies.cmake`
      (build on a machine with no system cpr or libcurl). It is written but has
      only ever used the system cpr.
- [ ] CI: build and run tests on every push, with a clang-tidy and sanitizer
      (ASan/TSan) job. TSan is worth having, since the runner and venues are
      multithreaded.
- [ ] Integration tests against venue sandboxes or recorded responses
- [ ] Shorting and margin in the paper venue and the P&L tracker
