# Kestrel TODO

## Status

The strategy, risk and backtest code has not run against a live venue, and
`run` in live mode is refused until a trading venue is implemented. `run` has
been exercised in paper mode on Binance.US, including an 8 hour market-making
session.

### Done
- [x] Build system, `Decimal`, shared types, `Error` / `std::expected`
- [x] `DataSource` / `TradingVenue` interfaces, `Configuration`, factory
- [x] `PaperTradingVenue` (works with any `DataSource`)
- [x] `KrakenDataSource`: REST bars and symbol info, WebSocket quotes (tested
      live)
- [x] `BinanceUSDataSource`: klines, symbol info, WebSocket quotes, closed-bar
      stream
- [x] Database: PostgreSQL + TimescaleDB `bars` and `quotes` hypertables,
      `BarStore`, `QuoteStore` / `QuoteWriter`, Binance.US closed-bar stream
      (tested live)
- [x] `PnlTracker`: realized P&L from fills, used by the paper venue and the
      risk gate (tested with fakes)
- [x] Strategy interface, `Runner` (event queue, warmup, gap backfill, fill
      polling to `OnFill`), example `SmaCrossover` (tested with fakes)
- [x] `RiskGatedVenue`: order and position size, open orders, order rate,
      daily loss, symbol allowlist, kill switch (tested with fakes)
- [x] `ReplayDataSource` and `RunBacktest`: the same `Runner`, a paper venue
      and a simulated clock (tested with fakes)
- [x] `kestrel_app run` and `backtest`, `[risk]` and `[strategy]` config,
      strategy registry
- [x] `MarketMaker` strategy with opt-in drawdown protection
- [x] Run recording: fills and P&L snapshots in the database (`[recording]`)

### Stubs
- [ ] Schwab data source, and the Kraken, Schwab and Binance.US trading
      venues return `kNotImplemented`; each header has a TODO listing the
      endpoints and auth needed. The timeframe mappings, symbol mapping and
      the Schwab token store are real. See Venues under Projects.

### Not started
- [ ] Indicators, more strategies, agent access (see Strategies under
      Projects)

## Known limitations

Things that work but have edges a user could trip over. Each is also noted in
the header of the code concerned.

### Strategy runner (`strategy/runner.hpp`)
- Quotes are conflated per symbol, but the event queue is otherwise unbounded:
  a strategy slower than the bar stream still falls behind (bars are never
  dropped).
- Fills and order changes are pushed only by venues that implement
  `StreamExecutions` (today only the paper venue). Others are polled, so
  `OnFill` can lag the fill by up to `fill_poll_interval` (default 1 s).
- With pushes, the backstop poll looks back `Runner::kSeenFillHorizon`
  (10 minutes) behind the newest fill, and a pushed fill older than that is
  dropped as stale. The paper venue pushes with no delay, so live latency is
  not modelled in paper runs (see the paper-delay item under Projects).
- The fill poll starts from the time `Begin()` ran and assumes the venue's fill
  times and the runner's clock roughly agree. A fill stamped earlier than the
  start (clock skew) is missed.
- Calendar timeframes (`1M`) get no gap backfill, and warmup sizing for them
  uses a 31-day estimate.
- One `Runner` drives one strategy; running several means one runner (and one
  set of data subscriptions) each.
- Without pushes, `OnOrderUpdate` is found by polling (`GetOpenOrders`, plus
  `GetOrder` for an order that left the list) at `fill_poll_interval`, so a
  quick state change can be seen late or have intermediate states skipped. An order that vanishes
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
- Protection rules (stop loss, loss and drawdown caps, spike stand-down,
  downtrend and momentum filters, post-stop cooldown) are opt-in and off by
  default. They come from the Oct 2026 live and replay ZEC/USDT runs; those
  replays were one down-trending 4 h sample with an uncalibrated fill model,
  so the default thresholds are hints, not tuned values. Tighter stops than
  40 bps and the cooldown showed no reliable gain.
- Protection P&L covers only the position beyond the starting balance, so a
  fall in the starting holding itself trips nothing. It values the position
  at the mid, and exits are market orders (taker fee and slippage).
- A halt is permanent for the run; only restarting the process resumes it.
  Nothing is persisted, so a restart starts with fresh P&L and a fresh peak.
- Rules run only when a quote arrives, so a silent feed trips none of them.
- No volatility scaling of the spread, and no pulling of quotes when the feed
  goes quiet: it only acts when a quote arrives.
- Inventory is the base-asset balance at start and the fills since; a fee
  charged in the base asset (Binance without BNB) is not deducted from it.
- Each requote goes to the back of the queue, so a tight `requote_bps` costs
  fills under `limit_fill_model = "conservative"`. The ZEC/USDT example
  replaces a quote on about every other update (29.5k placements for 51k
  quotes).
- Fills the venue has reported but `OnFill` has not delivered
  (`mUnconfirmed`) are cleared only by that `OnFill`. If it never comes, the
  room on that side stays held for the rest of the run (logged once after
  30 s): safe, but the side can stop quoting for good. Ways it can never come:
  the runner drops a pushed fill older than `kSeenFillHorizon`; a fill stamped
  before the run started (clock skew) is never polled; and an order
  acknowledged as filled is assumed filled at the size asked for, so if a live
  venue fills a different (rounded) quantity the fills never add up to it.
  There is no reconciliation yet (see the project item under Strategies).
- Backtest ordering: a quote is matched against resting orders first, and
  the paper venue pushes the fill and the order's state, so the strategy hears
  of the fill before it sees the quote that caused it. That is better informed
  than live trading, where the fill report takes a round trip; there is no
  fill latency in a backtest at all (see the paper-delay item under
  Projects).

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

### Run recording (`strategy/run_recorder.hpp`)
- Fills are recorded when the runner polls them, so a fill the venue reports
  after the final poll at shutdown is missed. Fills and snapshots are written
  on the strategy thread, so a slow database delays the strategy.
- Unwritten fills are kept in memory (up to 100k) while the database is down;
  a snapshot that fails is skipped. A process that dies loses what was queued.
- P&L is only as good as the start: if the venue's positions cannot be read at
  startup the recorder warns and counts from zero, and a balance held at the
  start (not a position) has no cost basis, so selling it realizes nothing.
- Snapshot values in the market maker's own protection P&L and the recorder
  are computed separately and can differ (the strategy's covers only the
  position beyond its starting balance).
- `backtest` records nothing.

### Venues and data
- Kraken and Schwab trading venues are stubs that return `kNotImplemented`.
  The Binance.US venue reads (balances, open orders, order lookup, fills) but
  does not trade yet. Its `GetPositions` returns nothing, so the risk gate's
  `max_position_notional` does not see holdings the account already had.
- The latency rollups cover HTTP round trips only, and a window that fails to
  write is dropped, not retried.
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

Built and tested with fakes. Not yet run against a live venue, and reachable
from `kestrel_app run` and `backtest`.

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
- [x] Run the runner against Binance.US data end to end in paper mode: an 8 hour
      `market_maker` session traded, recorded and stopped cleanly
- [ ] `run` does not cancel open orders on shutdown; decide whether it should.
      (`MarketMaker` cancels its own quotes in `OnStop`; `SmaCrossover` and
      other strategies leave theirs resting.)
- [x] `backtest` loads bars from `BarStore` first and falls back to the
      provider (`LoadReplayData`), saving what it fetched
- [ ] Bound the runner's bar queue (quotes are conflated per symbol)
- [ ] Cap the paper venue's fill history so long paper runs stay bounded and
      fast, and stop rescanning all of it in `GetFills`
- [ ] Backtest summary: equity curve, total return, max drawdown, trade count,
      win rate, fees paid
- [ ] Backtest fill model: fill at the next bar's open by default (filling a
      signal computed on a close at that same close is a look-ahead), keeping
      fill-on-close as an option; use bar high/low for limit and stop order
      triggering
- [x] Document how to write a strategy in the README (hooks, threading rules,
      `Context`, the risk gate)

## Projects

### Venues
Each stub's header has a TODO comment listing the endpoints and auth it needs.
- [ ] **Kraken trading venue.** REST orders, balances, open orders and trade
      history, signed with the existing HMAC helper. Data source is already
      done.
- [ ] **Schwab data source and trading venue.** OAuth with the token store
      that already exists, bars, quotes and orders. Needs market-hours handling
      for equities.
- [ ] **Binance.US trading venue.** The data source is done. The venue is
      read-only so far (signed client, balances, open orders, order lookup,
      fills), checked against the live API on 2026-10-28 with `kestrel_app
      account` (signing, clock offset, 30 days of fills in 24 h windows,
      partial fills, BNB and USDT fees, a market order's lookup);
      `PlaceOrder`, `CancelOrder` and `CancelAll` are still stubs.
      Remaining: orders (never retry a POST; look the order up by client id),
      the order-rate limit, startup reconciliation of leftover orders, and
      flipping `HasLiveVenue`. The order-placement response shapes still
      come from memory; check them with `POST /api/v3/order/test` first.
- [x] HTTP response-time recording: `TimedHttpClient` on every factory HTTP
      client, a summary when `run` stops, and one-minute rollups in
      `venue_latency` with `[recording]`. `[http] slow_request_ms` sets the
      slow-call warning.
- [ ] Order-side latency: order acknowledgement, cancel acknowledgement, time
      until a fill shows in `GetFills`, and the venue's clock against ours.
      Needs order placement first; store it beside `venue_latency`.
- [x] Pushed executions: `TradingVenue::StreamExecutions`, the runner queues
      pushed fills and order changes as events and drops to a 30 s backstop
      poll, and the paper venue pushes. `MarketMaker` counts fills the venue
      has reported but `OnFill` has not delivered against its inventory room.
- [ ] Binance.US user-data stream behind `StreamExecutions`: listen key from
      the API key (no new credentials), keepalive about every 30 minutes,
      `executionReport` to fills and order states, a REST catch-up after a
      reconnect, and the event time against ours for the fill-visibility
      latency. Check the current Binance.US docs first: Binance.com has been
      moving from listen keys to a WebSocket API subscription. Until it lands
      the venue is polled, and polling `GetFills` (weight 10) and
      `GetOpenOrders` (weight 3) every second uses about 780 of the 1200
      weight per minute for one symbol, so a live run needs a longer
      `fill_poll_interval`.
- [ ] An optional delay on the paper venue's pushes (and fills), so paper
      runs can model live latency instead of seeing fills instantly.
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
- [ ] Reconcile `MarketMaker`'s unconfirmed fills. When one is older than a
      timeout (a few backstop polls), ask the venue: `GetOrder` for its real
      filled quantity (correcting a size assumed from the acknowledgement),
      and if the missing fill is older than the runner's duplicate check it
      will never be delivered, so resync the inventory from `GetBalances` and
      clear the entry. Reading `GetFills` from the strategy alone is not
      enough: it cannot tell a fill that is still on its way to `OnFill` from
      one that was lost, and counting it would count it twice when it lands.
      The cleaner long-term fix may be in the runner (report fills it drops,
      or let a strategy ask whether a fill ID was delivered).

### Backtesting and research
- [ ] Parameter sweeps and walk-forward testing
- [x] Load backtest bars from the database (`BarStore`) instead of building
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
- [ ] **Data retention for the database** (from the 10 minute paper run,
      2026-10-28; nothing is configured yet, no compression or retention
      policy exists on either hypertable). `quotes` is the only table that
      grows fast: about 6,400 to 12,000 rows an hour for one symbol, roughly
      520 bytes a row with its indexes, so 80 to 150 MB a day (29 to 55 GB a
      year) per symbol. The disk had 23 GB free, so one symbol fills it in
      about 5 to 9 months and each extra symbol shortens that. Proposed policy,
      as a migration:
      - `quotes`: compress chunks older than 7 days (segment by venue and
        symbol, order by `received_time`) and drop chunks older than 90 days.
        The expected 10x compression is a typical figure, not measured here;
        try it on the existing chunks first. Backtests mostly need recent
        quotes. Decide whether 90 days is enough before dropping anything, and
        make the ages configurable or at least documented.
      - `bars`, `runs`, `fills`, `pnl_snapshots`: keep forever. They are small
        (1-minute bars for 15 symbols grow about 1.7 GB a year; a day of P&L
        snapshots is about 360 kB).
      - `venue_latency`: keep 1 year, then review (about 1 MB a day live).
      - Add a check that warns at startup when the disk is nearly full.
- [x] Store the account's own trade history: `account_fills`, synced
      incrementally by `kestrel_app account --store` (live mode).
- [ ] Use `account_fills` for the cost basis of what the account holds at the
      start of a run, so selling a starting holding realizes a real profit or
      loss instead of the whole sale price (the 8 hour run's phantom
      position). Needs a rule for holdings older than the stored history
      (transfers in, buys before the first sync).
- [ ] Sync `account_fills` at the start of a live run, and now and then during
      it, instead of only from the `account` command, and compare a run's
      `fills` against it afterwards to catch fills the runner missed.
- [ ] Give every order Kestrel places a recognisable client order ID
      (`kst-<run_id>-...`), so `account_fills` and the startup check of open
      orders can tell Kestrel's orders from manual or other programs' ones.
      Needs order placement first.
- [ ] Persist orders and strategy decisions to the database (fills and P&L
      snapshots are recorded with `[recording]`; see `RunRecorder`). Store
      Kestrel's own orders with their final state, not the venue's whole order
      history: a market maker cancels most of what it places (about 125k
      orders a day at the paper run's pace), which would outgrow `quotes`.
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
- [x] Sanitizer builds: `-DKESTREL_SANITIZE=address,undefined` and `=thread`;
      the whole suite passes under both
- [ ] CI: build and run tests on every push, with a clang-tidy job and the
      ASan/UBSan and TSan builds above. TSan is worth having, since the runner
      and venues are multithreaded.
- [ ] The database tests share fixture rows, so they cannot run in parallel
      (`ctest -j` fails them); give each test its own venue name or schema
- [ ] Sanitize the Docker image's build as well, and run TSan against a long
      paper run (`kestrel_app run`), which the unit tests do not cover
- [ ] Integration tests against venue sandboxes or recorded responses
- [ ] Shorting and margin in the paper venue and the P&L tracker
