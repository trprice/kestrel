# Kestrel TODO

The strategy, risk and backtest code runs on Binance.US, in paper mode and
live, and on Schwab in paper mode; Schwab's trading venue is written but stays
off live until `[schwab] live_trading` (see Schwab data source under In
progress). `run` refuses live mode on Kraken, whose trading venue is a stub
(see Venues under Projects; `NoLiveVenue` in `factory.cpp`).

Order of this file: Priorities, In progress, Projects (each by priority), then
the reference sections, Known limitations and Done.

## Priorities

Empty for now: the next work is being decided. The code review's items
(Nov 5 2026) are done (see Done) or moved to their sections under Projects.

## In progress

### Schwab data source

Authorization is done: `kestrel_app authorize` saves the tokens, `SchwabAuth`
renews them and `SchwabApi` sends requests within the market-data rate limit.
`GetBars`, `GetSymbolInfo`, `StreamQuotes` and `StreamBars` work (see
`SchwabDataSource` in `include/kestrel/data/schwab_data_source.hpp` and
`SchwabStreamer` in `schwab_streamer.hpp`).

What the live API showed (Nov 2026, SPY), beyond Schwab's docs:
`/pricehistory` answers in whole days, whatever times are asked for; one
request took 30 days of 1-minute bars and 33 years of daily ones; 1-minute
bars go back about 48 days, 5-minute about 200, 30-minute about 260 (older
days come back empty, not as an error, as does an unknown symbol);
`needExtendedHoursData` defaults to true, and extended hours include the
overnight session (20:00 to 04:00 ET); daily, weekly and monthly bars are
stamped midnight US Central; share classes are `BRK/B` (`BRK.B` comes back
empty). The streamer: `userPreference` works for a Market Data app and gives
`wss://streamer-api.schwab.com/ws`; LEVELONE_EQUITIES sends a symbol's changes
about once a second; field 12 is the previous close, as the field table says
(the document's example is off); bid and ask sizes are in shares, as in REST
`/quotes`, not lots of 100 as the document says; an unknown symbol comes back
as zeros with status `Unknown`; a second LOGIN for the user disconnects the
first with notify code 12. CHART_EQUITY's fields are numbered one higher than
its table says (1 is the minute's sequence, 2 to 6 open, high, low, close and
volume, 7 the minute's start); it sends each closed minute once, about 2 s
after it closes, and the last closed minute again on subscribe, never the
forming one; the volume can be fractional (REST truncates it); the bars match
`/pricehistory`'s; an unknown symbol gets no data and no error.

#### Next steps
- [ ] When it happens, note Schwab's error body for an expired or revoked
      refresh token, a login refused for a revoked token, and a
      `STOP_STREAMING` (code 30): none has been seen live. The handling is
      defensive (see Done); renewals work, and do not rotate the refresh
      token, so the 7 days still run from `authorize`.
- [ ] Check the trading venue (see Done) on a real account, then default
      `[schwab] live_trading` on: `order --test` (previewOrder), a resting
      limit placed and cancelled, an ETF's `assetType`, an OCO's legs
      cancelling each other, `activityId` on fills, and the balance fields of
      cash and margin accounts.

## Projects

Sections in priority order: safety first, then what is in use.

### Risk
- [ ] FIFO and specific-lot P&L options in `PnlTracker`, for venues and tax
      reports that use them
- [ ] Closes from the risk gate during market hours only: it has no
      `MarketHours`, so a stock's close is sent while the market is shut
- [ ] The gate's count of what is held does not take off a fee paid in the
      coin (PnlTracker adds it to the cost instead); a close is capped at
      the coin's balance for that reason, but `max_position_notional` still
      counts the fee as held

### Venues
Each stub's header has a TODO comment listing the endpoints and auth it needs.
- [ ] **Kraken trading venue.** REST orders, balances, open orders and trade
      history, signed with the existing HMAC helper. Data source is already
      done.
- [ ] Schwab pushed executions: `ACCT_ACTIVITY` behind
      `SchwabTradingVenue::StreamExecutions`, on `SchwabStreamer`'s one
      connection. Until then fills and order changes are polled, every 5 s.
- [ ] Order-side latency: order acknowledgement, cancel acknowledgement, time
      until a fill shows in `GetFills`, and the venue's clock against ours,
      stored beside `venue_latency`.
- [ ] Binance.US user-data stream behind `StreamExecutions`: listen key from
      the API key (no new credentials), keepalive about every 30 minutes,
      `executionReport` to fills and order states, a REST catch-up after a
      reconnect, and the event time against ours for the fill-visibility
      latency. Check the current Binance.US docs first: Binance.com has been
      moving from listen keys to a WebSocket API subscription. Until it lands
      the venue is polled, and polling `GetFills` (weight 10) and
      `GetOpenOrders` (weight 3) every second uses about 780 of the 2400
      weight per minute the client keeps to, per symbol, so a live run on
      several symbols needs a longer `fill_poll_interval`.
- [ ] Re-verify the timeframe lists for Kraken and Binance.US against each
      provider's docs (Schwab's is a step under In progress). The lists in the code
      and docs/design.md came from memory.

- [ ] Follow redirects in `CprHttpClient` (bounded, HTTPS only), deciding
      whether `Authorization` goes to another host. Before the Kraken or
      Schwab trading venue lands.
- [ ] Keep `BinanceUSApi`'s rule in the Kraken and Schwab venues: a request
      that is not idempotent is never retried (`util::Retry` itself retries
      a timeout).
- [ ] A stop-limit order type, if a venue needs one.
- [ ] `RateLimiter::Penalize` with a hold, to honour `Retry-After`.

### Operations
- [ ] Record quotes continuously: `stream --store` under systemd (a loop
      that restarts it) on the server, for several pairs. Recordings so far
      were runs of a few days started by hand. Mind the retention item below.
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
- [ ] Use `account_fills` for the cost basis of what the account holds at the
      start of a run, so selling a starting holding realizes a real profit or
      loss instead of the whole sale price (the 8 hour run's phantom
      position). Needs a rule for holdings older than the stored history
      (transfers in, buys before the first sync).
- [ ] Sync `account_fills` at the start of a live run, and now and then during
      it, instead of only from the `account` command, and compare a run's
      `fills` against it afterwards to catch fills the runner missed.
- [ ] Tell Kestrel's fills in `account_fills` from manual or other programs'
      ones. Binance.US orders already get a `kst-<tag>-<n>` client order ID
      (the tag is random per process), but `myTrades` does not return client
      order IDs, so `account_fills.client_order_id` is always empty. Record
      the exchange order ID of every order Kestrel places, with its run (and
      make the tag the run ID), so `account_fills` can be joined on
      `order_id`. Older history can be backfilled once by looking each order
      up (weight 4 each, about 800 for the 197 orders stored so far). The
      startup cancel does not need this: it clears every open order on the
      configured symbols by design (see docs/running.md, "One trader per
      account").
- [ ] Persist orders and strategy decisions to the database (fills and P&L
      snapshots are recorded with `[recording]`; see `RunRecorder`). Store
      Kestrel's own orders with their final state, not the venue's whole order
      history: a market maker cancels most of what it places (about 125k
      orders a day at the paper run's pace), which would outgrow `quotes`.
- [ ] Alerts (email, webhook or push) when the kill switch trips or a stream
      stays down
- [ ] Health checks: stream reconnect counts and a clock-skew warning
      (stale feeds are already logged; see Done)
- [ ] Run several strategies in one process, sharing data subscriptions
- [ ] Quote `seq` is numbered per process: two recorders of one symbol
      interleave. Check the stored maximum at each flush, or number in SQL.
- [ ] Database rows that do not parse: `std::stoll`/`stod` throw and
      `sql::ParseDecimal` reads zero; return kParse instead.

### Backtesting and research
- [ ] Check the backtest against more live runs. Only run 12 (Oct 29,
      30 min) has stored quotes for its window (see Known limitations under
      Backtesting for the result); runs 7 to 11 have only public trades, which
      `--quotes-from-trades` replays without sizes. Keep `stream --store`
      running during live runs, and compare each against its replay.
- [ ] Parameter sweeps and walk-forward testing: `backtest --sweep
      half_spread_bps=2,3,4,6` replaying the loaded quotes once per value and
      printing one table (summary and markouts), then tuning on one day and
      testing on the next. The sweeps so far were shell loops.
- [ ] A pair scanner: rank a venue's pairs by the market spread against the
      one-minute volatility, trade rate, maker fee and tick size against the
      price. Market making pays where the spread is wide for how fast the
      price moves (ZEC/USDT: about 1.5 bps of half-spread against 11 bps a
      minute), so the pair matters more than tuning.
- [ ] Benchmark comparison against buy-and-hold: the backtest summary's "vs
      hold" covers holding the starting balances; comparing against other
      benchmarks (an index, a basket) is not done

- [ ] The backtest summary keeps every mark of the run; only the markout
      horizons' tail is needed.

### Strategies
- [ ] Agent access (out of scope so far). What it should look
      like isn't defined yet; decide that before building it.
- [ ] Volatility squeeze breakout: Bollinger bands inside Keltner channels,
      entered as they widen (needs `Keltner`)
- [ ] Pairs trading: the z-score of two symbols' spread (long-only on spot)
- [ ] `momentum_rotation` ranked and sized by volatility (`Atr` / close)
- [ ] Grid trading and dollar-cost averaging
- [ ] Session VWAP reversion (needs `SessionVwap`)
- [ ] `mean_reversion`: keep the position across a restart, and a resting
      stop order instead of the check at the close
- [ ] Position sizing helpers: fixed fractional, volatility-targeted
- [ ] Indicators: `Keltner`, `Adx`, `Supertrend` and `SessionVwap` in
      `patterns.hpp`

#### Market maker
Every item here is done (Nov 2026; see Done). The Nov 2026 replays found
none of them an edge on ZEC/USDT: every setting lost to holding over the
four recorded days. Check a change's markouts, not only "vs hold".

### Engineering
- [ ] From the Nov 2026 project review, not yet done:
  - Value BNB (and other third-asset) fees at their price, not 1:1 with
    the quote asset.
  - Paper venue: reserve a buy-stop for the spread as well as slippage; a
    helper for the lock/ProcessPending/ExpireDayOrders prologue its public
    methods repeat.
  - Binance.US: share the unknown-outcome recovery of PlaceOrder and
    PlaceOco; a shared WebSocket host for the bar and quote streams.
  - Kraken's quote stream: a silence watchdog like Schwab's.
  - `MakeTimedHttpClient` builds a connection pool per call.
  - `Secrets()` reads the secrets file twice per load.
  - `Base64Decode` accepts some malformed input `EVP_DecodeBlock` lets
    through.
  - `RunBacktest`: build the timeline in a function of its own.
  - Tests: `types/time.cpp` round trips, malformed database rows,
    `RateLimiter::Penalize`, `Migrate()` failures.
- [ ] Building components in the factory starts the paper venue's quote
      stream as a side effect, so `bars` and `run` open a WebSocket whether or
      not they need it (`stream`, `stream-bars` and `backtest` already avoid
      this by building only the data source); make starting the venue explicit
- [ ] Exercise the cpr FetchContent fallback in `cmake/KestrelDependencies.cmake`
      (build on a machine with no system cpr or libcurl). It is written but has
      only ever used the system cpr.
- [ ] CI: it builds and runs the tests on every push; add a clang-tidy job
      (`.clang-tidy`; the whole tree takes about 12 minutes on 4 cores, so
      perhaps changed files only) and the ASan/UBSan and TSan builds. TSan is
      worth having, since the runner and venues are multithreaded.
- [ ] The database tests share fixture rows, so they cannot run in parallel
      (`ctest -j` fails them); give each test its own venue name or schema
- [ ] Sanitize the Docker image's build as well, and run TSan against a long
      paper run (`kestrel_app run`), which the unit tests do not cover
- [ ] Integration tests against venue sandboxes or recorded responses
- [ ] Paper shorting: a borrow fee, and a margin call (or a forced cover)
      when a short rises past its collateral; neither is modelled.
- [ ] MSVC: `Decimal` uses `__int128`, which MSVC lacks; it needs a small
      wide-integer fallback (the TODO in `src/types/decimal.cpp`).
- [ ] `PaperTradingVenue::Deliver` calls the execution callbacks under the
      listeners' lock. The `ExecutionCallback` contract forbids calling back
      into the venue, so nothing deadlocks today; calling them outside the
      lock would make a contract breach harmless.
- [ ] Refuse a `limit_price` on a `kStop` (stop-market) request instead of
      ignoring it.
- [ ] Push an instant fill-or-kill expiry in the paper venue to
      `StreamExecutions` listeners (the acknowledgement already says it).

## Known limitations

Things that work but have edges a user could trip over. Each is also noted in
the header of the code concerned.

### Strategy runner (`strategy/runner.hpp`)
- Quotes are conflated per symbol, and queued bars are capped at
  `RunnerOptions::max_queued_bars` (1000; none in backtests): a bar arriving
  past it is dropped, logged, and recovered by the gap backfill, but the
  newest one dropped in a quiet market is not seen until the next bar.
- Fills and order changes are pushed only by venues that implement
  `StreamExecutions` (today only the paper venue). Others are polled, so
  `OnFill` can lag the fill by up to `fill_poll_interval` (default 1 s).
- With pushes, the backstop poll looks back `Runner::kSeenFillHorizon`
  (10 minutes) behind the newest fill, and a pushed fill older than that is
  dropped as stale. The paper venue pushes at once unless `[paper]
  report_latency_ms` is set.
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
- Optional in `RunnerOptions`. `run` refuses live mode without `[risk]`
  (`ValidateForRun`); a driver that builds its own `Runner` is not checked.
- Daily loss and drawdown are summed across symbols, so they must share a
  quote asset.
- `max_drawdown`, `max_drawdown_close` and `max_loss_per_position` are
  measured at each order and at each reference price, at most once a second
  and not while an order is being placed: a peak or trough shorter than that
  can be missed. Only the last two close positions; the other loss limits
  block entries.
- A close cancels the symbol's orders and sells at market straight away, so
  a cancel still on its way (venue latency) can let the strategy's stop sell
  too; on a spot venue the second sale is refused for want of the coin.
- The strategy can hear of its stop's cancel before the close's fill (and
  run a bar in between): `breakout` then places the stop again, or its time
  exit, which the venue refuses for want of the coin, and logs that it is
  held without a stop until the fill reaches it. Seen in a daily BTC
  backtest with `max_loss_per_position = 15`: harmless, but noisy.
- `max_symbol_share` reads the venue's balances at every entry, and values a
  coin by the reference price of `COIN/QUOTE` (the quote of the order's
  symbol): a coin traded against another quote is left out of equity.
- `max_quote_age` relies on the runner's check every 10 seconds, so an entry
  can pass up to 10 s after the age is reached; backtests do not check it.
- `max_consecutive_losses` counts losing sell fills, so a sell that fills in
  several pieces counts each losing piece.
- Daily loss counts only fills at or after the baseline call, relying on the
  venue's and the gate's clocks roughly agreeing. A fill landing between the
  positions snapshot and the baseline time is missed.
- A fee in an asset that is neither the quote nor the base (e.g. BNB) is
  treated as if it were quote currency; `PnlTracker` logs each such asset
  the first time it sees one.
- Market orders are valued at the last reference price (bar close or quote
  mid). `max_price_age` is unset by default, so a price never goes stale.
- Spot only, unless `RiskLimits::shorts` (set for a paper venue with
  `allow_short`): a sell larger than the holding is treated as not reducing a
  position.
- What is held is the venue's position, or what the gate has seen fill since
  its loss count started if that is more. On a venue that reports no
  positions (Binance.US spot), holdings from before the run are invisible:
  `max_position_notional` does not count them, and a halted gate will not let
  them be sold. If the fills cannot be read, an exit is checked against the
  venue's position alone, and an entry is refused.
- With a loss, drawdown or consecutive-loss limit set, an order is refused,
  exits included, while the venue's fills cannot be read.
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
- `run` keeps the newest 100,000 fills and closed orders
  (`PaperOptions::max_fills`, `max_closed_orders`): `GetFills` from before
  the oldest kept returns what is kept, and `GetOrder` of a forgotten order
  is kNotFound. Backtests keep everything, so their memory grows with the
  orders placed.
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
- With `loss_cap`, `drawdown_cap`, `spike_standdown` or
  `cooldown_after_stop` on and a `[database]`, the halt, the P&L, its peak
  and the stand-down and cooldown ends are kept in `strategy_state`, so a
  restart carries on from them and a halted maker stays halted until
  `reset_state = true`. The position is kept too, with its entry and the
  mid it was marked at, and taken back on at restart (as much of it as the
  account still holds), so its later moves count, the stop loss watches it
  and a halted maker exits it. The P&L saved is marked at the mid of the
  last quote before the save (at least once a minute, at once after a fill
  or a halt), so a crash loses the move since then. In paper mode the
  venue starts each run from `[paper] initial_balances`, so a carried
  position is assumed out of them. Two makers on one symbol, provider and
  mode share one saved state.
- Rules run only when a quote arrives, so a silent feed trips none of them;
  `quiet_pull_ms` pulls the quotes after a silence, but checks no rule.
- Volatility scaling of the spread (`vol_multiplier`) did not beat a fixed
  spread of the same average width on the two recorded ZEC/USDT quote days
  (2026-10-19, 8 h; 2026-10-30, 24 h; conservative fills). Fixed 4 bps made
  +5.9 / +6.4 USDT vs hold; scaled at base 3, multiplier 0.4 (averaging
  4.4-4.8 bps) +2.9 / +6.4, and other windows (1-30 min) and multipliers no
  better. Both beat the 2 bps example, which lost 10.5 vs hold on Oct 30.
  The amounts are mostly inventory timing, so retest on more recorded days
  before choosing; until then it stays off by default.
- Inventory is the base-asset balance at start and the fills since, less any
  fee charged in the base asset (Binance.US takes a taker buy's fee from the
  ZEC bought). Before that was deducted, live run 10 (2026-10-29) had 99 sells
  refused for the balance. The fee leaves dust under the lot size that is
  never sold.
- `post_only = true` stops a quote from filling as a taker when the book
  moves through it while the order is in flight (41 of 106 fills in run 10).
  A refused quote is retried on the next quote. A backtest sees refusals
  only with `[paper] order_latency_ms` set, as a pushed kRejected rather
  than an error from PlaceOrder, so they are not in the "post-only quote(s)
  refused" count.
- Each requote goes to the back of the queue, so a tight `requote_bps` costs
  fills under `limit_fill_model = "conservative"`. The ZEC/USDT example
  replaces a quote on about every other update (29.5k placements for 51k
  quotes).
- `requote_toward_bps = 0` (replace a quote as soon as the market moves a
  tick towards it) with a wide `requote_bps` did best in the ZEC/USDT
  replays with 130 / 600 ms latency. Vs hold in USDT, Oct 30 / Oct 19:

      half-spread, requote_bps   symmetric        toward 0
      4 bps, 1                   -2.0 /  +0.7     -2.6 / +1.6
      4 bps, 6                  -49.2 / -12.3     +0.6 / +1.4
      6 bps, 1                   +4.6 /  -0.6     +4.4 / -1.7
      6 bps, 6                  -17.2 /  -2.6     +5.4 / +2.0
      2 bps, 6                 -109.1 / -24.5    -13.9 / -4.1

  It removes the loss from leaving a quote in the market's path (Oct 30 at
  4 bps, requote 6: 8542 fills down to 402), and 6 / 6 / 0 sent fewer
  orders than the old 1 bps symmetric (59k against 102k on Oct 30, about
  41 a minute). The 1 s markout of the fills left did not improve (still -4
  to -5 bps on Oct 30): with 130 ms to cancel, a quote the market reaches
  in one jump is still filled, which only pulling on a faster signal or
  quoting wider avoids. Run 12's settings with `max_orders_per_minute = 60`
  had 2512 orders refused by the risk gate and 12 fills, so the order limit
  has to allow for it. Two days; retest on more.
- Centring on the microprice (`microprice_weight`) did not help. With 6 / 0
  requoting and 130 / 600 ms latency, vs hold in USDT, Oct 30 / Oct 19:

      weight        0               0.25            0.5             1
      4 bps     +0.6 / +1.4     -2.9 / -2.1     -4.6 / +0.8     -0.4 / +1.1
      6 bps     +5.4 / +2.0     +1.6 / -0.7     +1.1 / +0.5     -0.3 / +0.5

  and the 1 s markout did not improve. It moved the centre about 1 bps on
  average and sent 10-15% more orders on Oct 30. The touch imbalance does
  predict the next move of the mid on ZEC/USDT, but weakly: with under 20%
  of the touch size on the bid, the next move was up 33% of the time
  (average -0.33 bps) against the microprice's -1.69 bps; over 80%, up 70%
  (+0.51 against +2.18); in between, close to even. That is small against
  the 5 bps the fills lose within a second, which come from jumps the touch
  does not foretell. Using an extreme imbalance only to pull the threatened
  quote, rather than to move both, is untested.
- Pulling the quote recent trades run against (`flow_pull_imbalance`,
  `flow_window_ms`) helped at tight spreads but not at the best setting.
  With 6 / 0 requoting and 130 / 600 ms latency, vs hold in USDT and the
  1 s markout in bps, Oct 30 / Oct 19:

      half-spread, pull        Oct 30              Oct 19
      2 bps, off            -13.9  -3.47        -4.1  -1.64
      2 bps, 0.5 / 2 s       -8.0  -3.57        -3.6  -1.06
      2 bps, 0.2 / 5 s       -9.4  -3.36        -2.4  -0.79
      4 bps, off             +0.6  -5.04        +1.4  +0.55
      4 bps, 0.5 / 2 s       +1.7  -4.29        +0.5  +1.18
      4 bps, 0.2 / 2 s       +2.8  -4.08        +0.1  +1.14
      6 bps, off             +5.4  -5.08        +2.0  +1.79
      6 bps, 0.5 / 2 s       +3.1  -5.18        +0.9  +2.06
      6 bps, 1 / 15 s        +5.5  -5.44        +3.1  +2.18

  (thresholds 0.2, 0.5 and 1 and windows 1, 2, 5 and 15 s were run; these
  are representative). Each side was pulled 4-18% of the time, and fills
  fell 15-30%. At 2 and 4 bps the markout improved by up to 1 bps and the
  loss at 2 bps shrank on both days; at 6 bps the markout did not improve
  on Oct 30 and vs hold fell for most settings, with too few fills (60-145)
  to tell the 1 / 15 s result from noise. Measured before building it:
  fills after flow against them did 2-9 bps worse at 1 s than fills after
  flow with them, but two-thirds of fills had no trade in the 5 s before
  them, and many warning trades come within 200 ms of the fill (a sweep),
  too late to cancel. The flow barely predicts the mid (+0.3 to +0.7 bps
  after a 2 s window of all buying), so leaning the quotes on it was not
  built. The live runner conflates quotes that queue up, so a burst of
  trades counts as its last one there; replays see every trade.
- Fills the venue has reported but `OnFill` has not delivered
  (`mUnconfirmed`) hold their side's room until they arrive. After a minute
  the order is checked with the venue (`GetOrder`, at most once a minute):
  a smaller fill than the acknowledgement assumed is corrected, and one all
  of which has arrived is dropped. After about 11 minutes, past the
  runner's `kSeenFillHorizon`, the missing part is booked at the order's
  average price (else its own), without a fee, and a copy arriving later
  (within 20 minutes) is skipped. One order is checked per quote, so a slow
  venue holds up quoting for one call at a time. A venue that cannot answer
  `GetOrder` books nothing (logged once), and the side keeps the room held.
  Only the market maker does this; the cleaner fix may still be in the
  runner (report the fills it drops).
- Backtest ordering: a quote is matched against resting orders first, and
  without `[paper] report_latency_ms` the paper venue pushes the fill and the
  order's state at once, so the strategy hears of the fill before it sees the
  quote that caused it. Set the latencies (see Backtesting below) for
  anything meant to predict a live run.

### Backtesting (`backtest/backtest.hpp`)
- Latency is off unless `[paper] order_latency_ms` / `report_latency_ms` are
  set, and it changes the results more than any strategy parameter. On the
  ZEC/USDT quote days with 130 ms / 600 ms, the market maker filled 3 to 6
  times as often, mostly orders filled while their cancel was in flight, and
  its 1 s markout fell to -3 to -5 bps on Oct 30: 4 bps went from +5.9 to
  -2.0 vs hold, 2 bps from -10.5 to -33.1, and 6 bps kept +4.6 only on
  inventory. Each latency is one fixed delay, without jitter, and an order
  or cancel takes effect at the first quote at or after it is due.
- Replayed over live run 12's window (Oct 29 23:28-23:58, its settings and
  starting balances, `conservative` fills), the backtest with 130 / 600 ms
  matched the live 1 s markout (-2.99 bps against -3.00) but filled more
  often (55 fills against 43) and at worse prices against the mid at the
  fill (-0.74 bps against +0.76). Without latency the 1 s markout was
  -2.43 bps. Live fill times are the exchange's and the quotes' are ours,
  about 60 ms apart; shifting them makes little difference. The P&L at the
  end differs by inventory timing (+0.0004 against -0.138 USDT on about
  54 USDT traded), so it is no check. One 30 minute run, 43 fills: a hint,
  not a calibration.
- Warmup bars are loaded from a span of wall-clock time before `--from`
  (`warmup_bars + 1` bar lengths), so on a market that closes (stocks)
  far fewer bars than `warmup_bars` arrive, and indicators smoothed over
  the window (an EMA crossover, an ATR stop) start the backtest less
  settled than they would live.
- Monthly timeframes are rejected with `kUnsupported`. Strategies that need
  quotes replay stored quotes (`QuoteStore`); there are no stored quotes for
  a range that was never recorded. `--quotes-from-trades` replays quotes
  rebuilt from stored public trades (`trades --store`, Binance.US) instead:
  stale between trades, no size at the touch (so the conservative model
  never fills at our price, only through it), and a side missing until it
  has traded. How far it moves results from a quote-based replay is still to
  be measured on a range that has both.
- Resting limit orders fill per `[paper] limit_fill_model`. `cross` (the
  default) is optimistic for quoting strategies. `conservative` models queue
  position from trades, so it needs stored quotes carrying `last`/`last_size`
  (Binance.US does; Kraken's ticker has no trade size, so only price-through
  fills happen there). A trade at our price counts only if its taker was on
  the other side (`last_taker_side`; quotes stored before 2026-11-02 had it
  filled in from `market_trades`). It assumes our order joins at the back of
  the displayed size, takes no account of our own orders moving the market,
  and cannot see trades that print at our price while a better-priced level
  still stands. Identical back-to-back trades (same price, size, side and
  exchange time) look like one.
- Counting only trades from the other side at our price changed little: on
  the Oct 19, Oct 30 and run 12 replays at 2, 4 and 6 bps, with and without
  latency, at most 50 fills of 1172 went (Oct 30, 2 bps, no latency) and vs
  hold moved by under 0.4 USDT. Most fills come from the opposite touch or
  trades through our price, not from trades at it.
- Bar-only backtests walk each bar as open, nearer extreme, other extreme
  and close: an order placed on a bar's close fills at the next price walked
  (normally the next open), and a resting stop or limit at its own price
  inside a bar. Which extreme came first is a guess unless
  `--fill-timeframe` supplies finer bars.
- An order placed in `OnStart` is rejected because no quote exists yet.
- The summary has no annualized figures (Sharpe and the like); one day of
  data is too little for them. Its realized P&L gives the starting holding
  the first mark as cost basis, so on a trending day it includes the move on
  what was sold; "vs hold" is the figure that leaves the move out.
- Markouts take the mid from the replayed quotes: at the fill, the last mid
  stamped before the fill's time, so a quote in the same millisecond is not
  counted as before it. A fill closer to the end than a horizon is left out
  of that horizon. Fees are not included.
- `backtest` takes symbol info (tick, lot, minimum size) from the
  provider as it is today, not as it was over the replayed range; a direct
  `RunBacktest` caller must set it for every symbol.

### Run recording (`strategy/run_recorder.hpp`)
- Fills are recorded as the runner delivers them (pushed, or polled), so a
  fill the venue reports after the final poll at shutdown is missed. Fills and snapshots are written
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
- The Kraken trading venue is a stub that returns `kNotImplemented`.
- Schwab orders are for the regular session, even with `extended_hours`.
  There is no post-only order (refused), so an OCO's limit leg is a plain
  limit, and no client order ID: `client_order_id` is known only for orders
  the running process placed. A placement lost to a timeout is found by its
  symbol, side, type, duration, quantity, price and time, and not guessed at
  when two orders fit.
- Schwab executions give no fee (the SEC and TAF fees on sales) or
  liquidity: fills say 0 USD and taker.
- Schwab: one malformed order anywhere on the account (on any symbol) fails
  the whole read of open orders or fills, and so `run`'s startup.
- Schwab margin accounts: cash is read from the non-marginable fields, so
  that borrowing power does not count; a margin loan makes the USD total
  negative and nothing available.
- The Binance.US venue runs live under `run`, polled for fills (no user-data
  stream yet). Its `GetPositions` returns nothing, so the risk gate counts
  only what filled since the run started (see Risk gate above).
- A Binance.US placement that times out and cannot be found afterwards is
  reported as an error, but the order may still appear later; it would then
  rest on the account untracked by the runner until the next `run` cancels
  it at startup, or a manual `cancel`.
- Binance.US's test endpoint does not check the balance (a test sell of ZEC
  the account does not hold is accepted), so `order --test` proves the
  request, not that it can be paid for.
- The latency rollups cover HTTP round trips only, and a window that fails to
  write is dropped, not retried.
- Schwab streams 1-minute bars only, so `StreamBars` builds 5, 10, 15 and
  30 minutes from them: such a bar whose last minute had no trades closes
  with the next trade, and the first one after a (re)connect is skipped when
  minutes of it were missed (the runner backfills it). Without
  `extended_hours` the regular session is taken as 09:30 to 16:00 New York
  time, so after-hours bars on an early-close day get through. Schwab has no
  hourly bars at all, and daily and longer ones from `GetBars` only.
- Market hours (US stocks on Schwab): the calendar is Kestrel's rules
  (holidays, early closes, the unscheduled closures since 2001), checked
  against Schwab's `/markets` for the days it answers (from a week back to a
  year ahead), asked in the background: until a date's answer is in (a
  moment after its first use), the rules hold. Schwab can only restrict a
  day (close it, open it later, close it earlier), never open one the rules
  close, since it fills in the next year's holidays late. A closure announced
  at short notice is known only once Schwab has it. Schwab's overnight session
  (20:00 to 07:00) does not count as open: with `extended_hours` its bars
  come, but orders are refused, and a gap in its bars is not backfilled. A bar
  closing at the session's close arrives after it, so an order on it is
  refused.
- Schwab allows one streamer connection per user, so only one process can
  stream from Schwab at a time: when a second one logs in (another
  `kestrel_app`, or thinkorswim), the first stops streaming, logs an error and
  its subscriptions go quiet. With `[schwab] takeover_reconnect_ms` set it
  reconnects after that wait instead, taking the connection back; two
  processes that both set it take it from each other at every wait.
- `PaperTradingVenue` has no leverage, and its shorts (`allow_short`) pay no
  borrow fee and are never called.
- `run` refuses live mode on a provider whose venue is a stub
  (`ValidateForRun`, with `HasLiveVenue` in `factory.cpp` to update as venues
  land), but `CreateComponents` still builds the stub, so `bars` in live mode
  runs until the first order-related call fails with `kNotImplemented`.
- Stream liveness: every WebSocket pings every 20 s, and IXWebSocket closes
  and reconnects one whose pong has not come back by the next ping; Schwab's
  streamer also reconnects after 60 s without a message. `run` logs a quote feed
  quiet for 2 minutes of open market time, and with `[risk]
  max_quote_age_ms` the gate refuses new positions in it until quotes
  return. A bar strategy's feed is not checked.
- Schwab's `GetSymbolInfo` sets the tick from the price when first asked
  (0.0001 under $1). The trading venue takes the tick from each order's
  price; paper mode keeps the first until the next run.

## Done

### Overall
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
      polling to `OnFill`), example `MaCrossover` (tested with fakes)
- [x] `RiskGatedVenue`: order and position size, open orders, order rate,
      daily loss, symbol allowlist, kill switch (tested with fakes)
- [x] `ReplayDataSource` and `RunBacktest`: the same `Runner`, a paper venue
      and a simulated clock (tested with fakes)
- [x] `kestrel_app run` and `backtest`, `[risk]` and `[strategy]` config,
      strategy registry
- [x] `MarketMaker` strategy with opt-in drawdown protection
- [x] Run recording: fills and P&L snapshots in the database (`[recording]`)

### Strategy system and risk gate
- [x] `Strategy` / `Context` / `Requirements` interface
- [x] `Runner`: queue, warmup, gap backfill, duplicate bars dropped, hook
      exceptions contained, fill polling, `Begin`/`Drain`/`End` for
      deterministic drivers
- [x] `MaCrossover` example strategy
- [x] `mean_reversion`: RSI mean reversion (classic RSI(14) or Connors
      RSI(2)) with a Bollinger band, trend and exit averages, an ATR stop and
      a holding limit
- [x] `SingleOrderPosition`, the order handling `ma_crossover` and
      `mean_reversion` share (orders filled in parts, coin fees, lots), and
      the order helpers in `strategy/orders.hpp`
- [x] `OnOrderUpdate` hook and `TradingVenue::GetOrder`; the runner tracks the
      strategy's active orders and reports state changes; `MaCrossover` stops
      waiting on an order that closed without a fill
- [x] Per-symbol quote conflation in the runner
- [x] `RiskGatedVenue::Halt` cancels again after an in-flight order lands
- [x] `RiskGatedVenue`: order and position notional, open orders, order rate,
      daily loss, symbol allowlist, price staleness, kill switch
- [x] `PnlTracker` as the single P&L implementation, used by both the paper
      venue and the risk gate
- [x] `ReplayDataSource` and `RunBacktest`
- [x] Backtest fill model: an order placed on a bar's close trades at the
      next price replayed (the next bar's open), not at that close
      (`PaperTradingVenue::SetDeferMatching`); `backtest --fill-at-close`
      keeps the old model.
- [x] The runner's bar queue is bounded (`RunnerOptions::max_queued_bars`,
      1000): a bar arriving past it is dropped and logged, and the gap
      backfill fetches it once the strategy catches up.
- [x] Paper history is bounded for `run`: the newest 100,000 fills and
      closed orders (`PaperOptions::max_fills`, `max_closed_orders`;
      backtests keep everything). `GetFills` already binary-searches by time.
- [x] `run` cancels entries when it stops and keeps the orders that close a
      position, so a protective stop stays (`RunnerOptions::
      cancel_entries_on_stop`, `EntriesToCancel`).
- [x] `backtest --fill-timeframe 1m` (`BacktestOptions::fill_timeframe`):
      the paper venue walks finer bars than the strategy takes, so the path
      within a strategy bar is the recorded one rather than the open,
      nearer extreme, other extreme, close guess.
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
- [x] Run the runner against Binance.US data end to end in paper mode: an 8 hour
      `market_maker` session traded, recorded and stopped cleanly
- [x] `backtest` loads bars from `BarStore` first and falls back to the
      provider (`LoadReplayData`), saving what it fetched
- [x] Document how to write a strategy (docs/writing-strategies.md: hooks,
      threading rules, `Context`, the risk gate)
- [x] Backtest summary: equity, return, return against holding the starting
      balances, max drawdown, realized and unrealized P&L, fills, volume,
      fees, closing sells won, inventory range, and the equity curve
      (`--equity-csv`)
- [x] Paper venue latency: `[paper] order_latency_ms` (orders and cancels
      reach the book later, so an order can fill while its cancel is in
      flight, and post-only and fill-or-kill are checked on arrival) and
      `report_latency_ms` (pushed fills and order states arrive later)
- [x] Backtest markouts: each fill against the mid before it (spread
      captured), 1 s, 10 s, 1 min and 5 min after it, and at the end (equal
      to vs hold plus quote-asset fees)
- [x] Market maker volatility scaling of the spread (`vol_multiplier`,
      `vol_window_ms`, `max_half_spread_bps`), off by default
- [x] Market maker asymmetric requoting (`requote_toward_bps`): a quote the
      market moves towards is replaced on a smaller move than one it moves
      away from
- [x] Market maker microprice centring (`microprice_weight`), off by default
- [x] Market maker order-flow pull (`flow_pull_imbalance`,
      `flow_window_ms`): no quote on the side recent trades run against,
      off by default
- [x] Market maker queue position, off by default: `touch = "join"` /
      `"improve"` holds a computed price to the best price on its side (or
      a tick inside it), and `keep_at_touch` keeps a quote at the touch
      when its target moves away. Replayed on ZEC/USDT (Oct 19, Oct 30-31,
      Nov 1-2, Nov 7; post-only, 130 / 600 ms latency, conservative fills),
      vs hold summed over the four: at 2 bps -100.3, join -72.1, improve
      -88.1; at 4 bps -30.8 / -31.6 / -30.3; at 6 bps -18.9 / -19.6 /
      -19.7. keep_at_touch changed nothing: ZEC's tick is 0.08 bps, so a
      computed quote is seldom exactly at the touch, and with join a target
      is never better than the touch, so a quote there never moves away.
      Untested on a wide-tick pair. The
      same replays without post-only lose far more (4 bps: -134.4), as
      quotes the book moved through in flight fill as takers.
- [x] `OnTimer` (`Requirements::timer_interval`): a strategy hook called on
      the runner's clock whether or not anything arrives; `Run()` wakes for
      it, and backtests tick it at simulated times between events.
- [x] Market maker `quiet_pull_ms`: pulls both quotes after that long with
      no quote, on the timer. In the same replays, 30 and 60 s were
      neutral (vs hold within 0.3; 32 and 5 pulls in four days), while 2
      and 5 s pulled 41,819 and 11,399 times and lost more (4 bps: -34.2
      and -41.2 against -30.8): Binance.US's book stream is silent while
      the book is unchanged, over 10 s 2,695 times on ZEC/USDT. It guards
      against a stalled feed; it is not an edge.
- [x] Market maker inventory skew: `skew_power` (the lean raised to a
      power, Avellaneda-Stoikov's quadratic at 2) and `skew_age_ms` (the
      skew grows with how long the position has been held, doubling at
      that age). Off by default. Same replays, vs hold summed, 4 / 6 bps,
      skew_bps 4: linear -30.8 / -18.9; power 2 -42.9 / -19.4, and with
      skew_bps 8 -34.7 / -17.5; age 10 min -29.5 / -19.4, 30 min
      -27.2 / -16.7; linear skew_bps 8 -32.1 / -15.2. The days swing by
      several USDT each, so none is clearly better; a 30 min age was
      ahead of the linear skew_bps 4 on both spreads.
- [x] Market maker `levels` and `level_step_bps`: several quotes a side, a
      step apart, sharing max_inventory. Same replays, vs hold summed, 4 /
      6 bps: one level -30.8 / -18.9; two at the half-spread step -38.6 /
      -21.3, three -42.4 / -19.4; at twice the half-spread step, two
      -33.4 / -17.5, three -35.4 / -16.6. Deeper fills capture more at the
      fill, but on the falling days (Nov 1-2) the extra levels build more
      inventory (two levels at 4 bps: -19.6 against -11.8). Three levels
      sent three times the orders (162k against 54k on Nov 7).
- [x] Market maker state kept across restarts: a `strategy::StateStore`
      (`Requirements::state`, `Context::State`), kept in the database's
      `strategy_state` table (migration 9) and scoped by provider and mode.
      The halt, the protection P&L and its peak, and the stand-down and
      cooldown ends carry over, so the loss and drawdown caps count every
      run until `reset_state`. A state it cannot read stops it quoting; a
      live run without a [database] is refused.
- [x] Market maker reconciles unconfirmed fills: `GetOrder` after a
      minute corrects an assumed size, and a fill still missing after
      about 11 minutes (past the runner's duplicate check) is booked, with
      a late copy skipped. It does not resync the inventory to the venue's
      balance: that also holds fills still on their way, which would then
      count twice.
- [x] Risk gate holdings: the larger of the venue's position and what the
      gate has seen fill since its count started, so on Binance.US (no
      positions) a halt or loss limit still lets the exit of what the run
      bought through, and `max_position_notional` counts it (review item, Nov
      2026)
- [x] `order` goes through the risk gate with the limits one order can trip
      (`ManualOrderLimits`), `--test` included; `--no-risk` skips it with a
      warning
- [x] Portfolio limits: `max_total_notional` (every symbol's position
      together) and `max_symbol_share` (one symbol's position as a share of
      the account's equity, from the venue's balances)
- [x] Drawdown measured at each reference price as well as each order, and
      `max_drawdown_close`, which halts and closes every position at market
- [x] `max_loss_per_position`: a position's unrealized loss from its cost;
      the gate closes it at market and pauses the symbol for the UTC day
- [x] `max_quote_age_ms`: no new positions in a symbol whose quotes have
      stopped while its market is open, until they return
- [x] `breakout` `risk_per_trade`: entries sized from the distance to the
      stop, capped at `quantity`
- [x] Per-strategy structured log context (`strategy::LogContext`,
      `Context::Log()`, `RunnerOptions::log`): prepends the strategy name and
      `key=value` fields (including `symbol=<symbol>` for single-symbol
      strategies) to every runner, recorder and strategy log line

### Venues
- [x] HTTP response-time recording: `TimedHttpClient` on every factory HTTP
      client, a summary when `run` stops, and one-minute rollups in
      `venue_latency` with `[recording]`. `[http] slow_request_ms` sets the
      slow-call warning.
- [x] Pushed executions: `TradingVenue::StreamExecutions`, the runner queues
      pushed fills and order changes as events and drops to a 30 s backstop
      poll, and the paper venue pushes. `MarketMaker` counts fills the venue
      has reported but `OnFill` has not delivered against its inventory room.
- [x] Schwab `GetBars` on `/pricehistory` (10-day pages of minute bars,
      trimmed to the range, daily and longer at 00:00 UTC of their date,
      `[schwab] extended_hours`) and `GetSymbolInfo` from US equity rules;
      share classes map `BRK.B` to Schwab's `BRK/B`
- [x] Schwab `StreamQuotes` on the streamer's LEVELONE_EQUITIES, over one
      connection shared by the process (`SchwabStreamer`): login with a
      current token, ADD and UNSUBS as subscriptions come and go, one token
      renewal on a denied login, and a stop when another connection takes
      over (or, with `[schwab] takeover_reconnect_ms`, a reconnect after that
      wait). `bars` and `run` now work on Schwab in paper mode.
- [x] Schwab `StreamBars` on CHART_EQUITY, on the same connection: 1-minute
      bars, and 5 to 30 minutes built from them, in the regular session
      unless `[schwab] extended_hours`. Bar strategies run on Schwab in paper
      mode.
- [x] Binance.US order statuses with an unknown side, type or time in force
      fail with kParse instead of being read as a buy, a limit order or GTC
      (a missing time in force, as on market orders, is still GTC)
- [x] Binance.US trading venue: `PlaceOrder` (LIMIT GTC/IOC/FOK, MARKET,
      STOP_LOSS, LIMIT_MAKER; `kst-` client order IDs, never resent, looked
      up by client ID after a timeout or 5xx), `CancelOrder` (-2011 looked up
      to tell filled from closed), `CancelAll`, a 9/s order-rate limit, the
      minimum order value, and request weight kept to 2400 of the 6000 a
      minute advertised. Checked live (a resting order placed and cancelled
      on 2026-10-29), and `market_maker` traded live on ZEC/USDT in Oct-Nov
      2026. `run` cancels every open order on its symbols before starting.
      Fills are polled; the user-data stream is under Venues in Projects.
- [x] Schwab trading venue (`SchwabTradingVenue`), on `SchwabAuth` and its
      own `SchwabApi`: market, limit and stop orders, OCO, previewOrder
      (`order --test`), cancels, open orders, fills, positions and balances
      (cash and margin accounts, with the shares held). Only the configured
      symbols' orders and fills count, so `CancelAll` leaves the rest of the
      account alone. Order fetches start at the oldest order working at the
      last one, not 180 days back each poll. `[schwab] account_number` picks
      the account; `[schwab] live_trading` (default off) lets `run` trade.

### Market hours
- [x] `DataSource::Hours()`: always open for crypto; for Schwab, the US
      equity calendar (`UsEquityCalendar`, matching NYSE's published 2024 to
      2027 holidays and early closes, and Schwab's `/markets` for the rest of
      2026) checked against `/markets` (`SchwabCalendar`). `ParseIso8601`
      reads UTC offsets.
- [x] The runner skips gap backfill across closed time, refuses orders while
      the market is closed (`SessionGatedVenue`), and calls
      `OnSessionOpen` and `OnSessionClosing`; `Context::MarketOpen` and
      `CurrentOrNextSession`. Backtests step their clock through the session
      events. The paper venue fills nothing while closed. The bar stream's
      regular-session filter uses the calendar (early closes, holidays).

### Breakout strategy
- [x] `breakout` (`strategy/breakout.hpp`): long-only breakout of an NR,
      inside-bar or N-bar range setup, with a buy-stop entry (or `closes` /
      `percent` confirmation), a protective stop below the setup, a target
      at its height checked on closes, `arm_bars`, `max_hold_bars`, a
      `trend_sma` filter and `flatten_at_close`; market-hours aware.
- [x] Review fixes for live venues: an entry that closes partly filled is
      held with a stop for what it bought (and a late part resizes the
      stop); a failed entry cancel stays armed and retries each bar; when the
      stop or the exit sells out, the other is cancelled, and a fill the
      state machine does not expect is logged as an error.
- [x] Bar-only backtests walk each symbol's own finest series with bars, so
      a symbol missing the finer data still trades.
- [x] `entry = "throwback"`: after a confirming close above the trigger, a
      post-only limit buy at the broken level for `throwback_bars` bars, for
      every setup (the deck's throwback; a maker fill, no slippage).
- [x] `setup = "rectangle"`: the last `range_bars` bars' range once both
      bounds have been touched `min_touches` times (within `touch_zone` of
      the height), optionally after a `shortfall`; `RectangleOf` in
      `patterns.hpp`.
- [x] `setup = "pipe"`: the deck's pipe bottom (two-bar reversal) after a
      `decline_bars` low, entered above the second bar; `PipeBottomOf` in
      `patterns.hpp`.
- [x] `setup = "failed_breakdown"`: the long side of the deck's false
      breakout (a close back above a broken range low within
      `reclaim_bars`), stopped below the false low; `FailedBreakdownOf` in
      `patterns.hpp`. On daily bars for 33 stocks and ETFs since 2016:
      +0.9% a trade pooled (+2.3% in 2016-2021, -0.4% since); only broad
      index ETFs stayed positive in both halves.
- [x] `setup = "double_bottom"` / `"triple_bottom"`: 2 or 3 swing lows
      within `touch_zone` of the height, entered above the neckline;
      `SwingPoints` and `MultipleBottomOf` in `patterns.hpp` (swing points
      also underlie triangles, head-and-shoulders and cup-and-handle).
- [x] `setup = "inverse_head_shoulders"`: three swing lows, the middle
      lowest, bought above a neckline through the highest highs between
      them, which may slope; `InverseHeadShouldersOf` in `patterns.hpp`.
- [x] `setup = "ascending_triangle"` / `"symmetrical_triangle"`: flat or
      falling swing highs (a least-squares line through the falling ones)
      over rising swing lows, `min_touches` of each, bought above the
      resistance and stopped below the newest low; `TriangleOf` in
      `patterns.hpp`.
- [x] `resting_target`: `breakout` rests its target as a sell limit
      paired with the protective stop, one-cancels-other
      (`TradingVenue::PlaceOco`, on the paper and Binance.US venues), instead
      of selling at market on a close past it.
- [x] `setup = "cup_handle"`: two swing-high rims around a rounded cup
      and a short handle in its upper half, bought above the right rim and
      stopped below the handle; `CupHandleOf` in `patterns.hpp`. The last
      of the deck's patterns.
- [x] Shorting: the paper venue sells short with `[paper] allow_short`
      (quote-asset collateral, `short_margin`), `PnlTracker` keeps signed
      positions, the risk gate counts shorts, and `breakout` trades
      `direction = "short"` (each setup mirrored) and `reverse`
      (stop-and-reverse on a failed breakout).
- [x] `setup = "opening_range"`: the session's first `opening_bars` bars
      (`OpeningRangeOf` in `patterns.hpp`), a setup once a session on the
      close of the last of them; the UTC day on a market that never closes.
- [x] `backtest` keeps the equity curve only for `--equity-csv`, sampled
      every `--equity-interval` (default 1m): it had sampled every minute
      regardless, 2.9 GB for 11 pairs over five years; now 156 MB. What is
      left grows with the run: the bar timeline and the summary's mark
      history (each symbol's mark at every event, for the markouts).
- [x] `momentum_rotation`: hold the `top_n` of `[general] symbols` by their
      return over `lookback_bars`, rebalanced every `rebalance_bars`. On 11
      Binance.US USDT pairs, daily bars, 2022 to Oct 2026 (1h fills, 2 bps
      fees, 5 bps slippage), 28 days and the top 3 made +973% (drawdown
      68%), +76k of the +97k from ZEC; without ZEC +128% (drawdown 56%),
      against +63% (67%) for holding BTC: by year -41% / +167% / +53% /
      -12% / +7% against BTC's -64% / +156% / +121% / -6% / -12%. Short
      lookbacks with one holding were erratic without ZEC.
- [x] Volume confirmation (`volume_ratio`, `volume_bars`) for entries
      confirmed on a close, and a moving stop: breakeven (`breakeven_r`)
      and trailing (`trail_bars`). From Kraken's day-trading guide. Range 1h
      on 7 crypto pairs, May-Oct 2026, filled on 1m bars: the buy-stop
      baseline made +4,677; a close-confirmed entry +686, and +2,141 with
      `volume_ratio = 2` (+1,845 and all 7 pairs up with `arm_bars = 3`);
      `trail_bars = 10` +4,115 with a smaller drawdown (5.5% against 7.0%),
      `trail_bars = 3` +3,141; `breakeven_r = 0.5` +3,743, and at 1 no
      change, as the target comes first. None beat the baseline there.
- [x] Daily bars on a market that closes: breakout places a daily setup's
      entry, and sells a target or time exit, at the next open (it never
      traded on daily stock bars before), and the backtest walks a daily
      bar through its session with the open's hooks after the open quote.

### Bar patterns
- [x] `strategy/patterns.hpp`: `Range`, `IsNarrowRange` (NR4 and the like),
      `IsInsideBar`, `RangeOf` (the last N bars' high and low) and `Sma`,
      which `MaCrossover` now uses instead of its own.
- [x] Indicators in `patterns.hpp`: `Ema`, `Rsi` and `Atr` (Wilder's),
      `TrueRange`, `Bollinger` (population deviation) and `Macd`, in
      `Decimal`, each checked against an exact rational computation.
- [x] Strategies on the indicators: `ma_crossover` takes `average =
      "ema"`, and `breakout` an ATR stop (`atr_stop`, `atr_bars`); its
      trailing stop's low comes from `RangeOf`. With the options off,
      backtests are identical to before. Range 1h on 7 crypto pairs,
      May-Oct 2026, 1m fills: the setup-low stop made +4,677 (7.0% worst
      drawdown); ATR stops of 1.5 / 2 / 3 ATRs +4,571 / +4,935 / +5,053
      (7.2% / 5.6% / 5.6%), but +1,822 / +1,436 / +1,102 without ZEC
      against +1,304. The EMA crossover (10/30, 1h) made +1,067 against the
      SMA's +2,301, 3 of 7 pairs up against 5.

### Backtest fills within a bar
- [x] Bar-only backtests walk each bar of the finest series for the paper
      venue (open, nearer extreme, other extreme, close, and every resting
      order's price on the way), so a stop or limit inside a bar fills at its
      own price, a gap through it at the open, and a protective stop placed
      on a fill can trade later in the same bar. Before, the venue saw only
      each close: a stop filled only on a close beyond it, at that close.

### Schwab data, polish
- [x] `config/schwab.example.toml`: a commented paper setup with the
      authorization steps; `config/schwab.toml` and the token file's
      temporary files are git- and docker-ignored.
- [x] `data::RecentBars`: the newest N closed bars across closed time (the
      span doubles until it has them). The runner's warmup uses it, so a
      30-minute strategy on Schwab starts with its full window rather than
      the ~13 bars of the last 16 hours; and `bars <tf> <count>` prints
      exactly `count`.
- [x] Schwab `GetSymbolInfo` checks the symbol with a quote: unknown is
      kNotFound (and `bars` skips it), a non-equity kUnsupported, and a
      stock under $1 gets a 0.0001 tick. The runner fetches every symbol's
      info in `Begin`, so an unknown symbol stops a run (or a backtest)
      before it starts, and orders never wait on the request.
- [x] Defensive Schwab handling for what has not been seen live: a token
      renewal refused with a 4xx RefusesTheRefreshToken does not recognise
      is an authorization error once the access token has run out (the
      streamer stops on it, with the re-authorization instructions); a
      STOP_STREAMING closes the connection and reconnects after 30 s.
- [x] Stale feeds: Schwab's streamer reconnects after 60 s without even a
      heartbeat; `run` logs a quote feed quiet for
      `RunnerOptions::stale_quote_warning` (2 min) of open market time
      (`MarketHours::OpenDuration`), and its recovery the same way.

### Build and operations hygiene
- [x] `nlohmann_json`'s release tarball pinned with `URL_HASH`;
      `.dockerignore` leaves out the `build-*/` directories (about 2 GB)
- [x] `setup_database.sh` sends the password to psql on stdin (a `\set`
      line) instead of `psql -v`, which put it in the process list
- [x] `-Wall -Wextra -Wpedantic` on `kestrel_app` and the tests too, with
      their warnings fixed; `KESTREL_WERROR`, on in CI
- [x] The Schwab token file is written to a temporary file unique to the
      process and call, created 0600, synced, renamed over the file, and the
      directory synced

### Recording and configuration
- [x] Schwab `GetBars` retries each page on a timeout, 429 or 5xx
      (`util::Retry`, honouring Retry-After), as the other providers do
- [x] `QuoteStore::Save` skips quotes already stored (`ON CONFLICT ... DO
      NOTHING`), so a retried flush no longer fails forever and drops quotes
- [x] Database connections: a 10 s connect timeout, TCP keepalives and
      user timeout, and `[database] statement_timeout_ms` (default 5 min)
- [x] Unknown keys in every config table, and unknown tables, are errors at
      load; the shipped examples are loaded by a test

### Backtesting and research
- [x] Load backtest bars from the database (`BarStore`) instead of building
      `ReplayDataSource` by hand
- [x] The aggressor side of trades: `market_trades.taker_side` for the trade
      history, and `Quote::last_taker_side` in the live quote stream (the
      Binance.US `m` flag), stored in `quotes`, and used by the conservative
      fill model

### Operations
- [x] Store the account's own trade history: `account_fills`, synced
      incrementally by `kestrel_app account --store` (live mode).

### Engineering
- [x] Sanitizer builds: `-DKESTREL_SANITIZE=address,undefined` and `=thread`;
      the whole suite passes under both

### Documentation
- [x] A document for each strategy in `docs/strategies/` (how it works,
      what Kestrel does for it, its parameters, moved from
      `configuration.md`), with the strategy headers' descriptions moved
      into them; the code's comments keep to why it does what it does.
