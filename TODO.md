# Kestrel TODO

## In progress

### Strategy system and risk gate

Built and tested with fakes, and reachable from `kestrel_app run` and
`backtest`. Run on Binance.US in paper mode (including an 8 hour
market-making session) and live (`market_maker` on ZEC/USDT, Oct 2026).

#### To finish
- [ ] `run` does not cancel open orders on shutdown; decide whether it should.
      (`MarketMaker` cancels its own quotes in `OnStop`; `SmaCrossover` and
      other strategies leave theirs resting.)
- [ ] Bound the runner's bar queue (quotes are conflated per symbol)
- [ ] Cap the paper venue's fill history so long paper runs stay bounded and
      fast, and stop rescanning all of it in `GetFills`
- [ ] Backtest fill model: fill at the next bar's open by default (filling a
      signal computed on a close at that same close is a look-ahead), keeping
      fill-on-close as an option; use bar high/low for limit and stop order
      triggering

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
- [ ] Confirm which error body Schwab sends for an expired or revoked
      refresh token (`RefusesTheRefreshToken` in `src/data/schwab_auth.cpp`
      looks for `invalid_grant` or "refresh token"). Renewal works live
      (Nov 4 2026), and Schwab kept the same refresh token: renewals do not
      rotate it, so the 7 days still run from `authorize`.
- [ ] `GetSymbolInfo`'s tick is 0.01 for every symbol: sub-dollar stocks
      quote in 0.0001, which `SymbolInfo` cannot express (one tick per
      symbol). It does not check that the symbol exists.
- [ ] See a login that the streamer refuses for a token Schwab has revoked
      (only an expired one, code 3, is in the document), and a
      `STOP_STREAMING` (code 30), live.
- [ ] Market hours: the runner's gap backfill sees every overnight and
      weekend as a gap (one wasted `GetBars` call each), a quiet feed outside
      the session must not look like a dead one to any future stale-feed
      check, and a strategy needs to know when the market is closed.
- [ ] Docs: a `config/schwab.example.toml` (docs/configuration.md says
      there is none).
- [ ] Then the trading venue (see Venues under Projects). Its header's TODO
      predates `SchwabAuth` and `SchwabApi` and should be rewritten to build
      on them. Pushed executions (ACCT_ACTIVITY) would also have to share
      `SchwabStreamer`'s connection.

## Status

The strategy, risk and backtest code runs on Binance.US, in paper mode and
live; `run` refuses live mode on any other provider until its trading venue
is implemented (`HasLiveVenue` in `factory.cpp`).

### Stubs
- [ ] The Kraken and Schwab trading venues return `kNotImplemented`; each
      header has a TODO listing the endpoints and auth needed. Schwab's
      market data (bars, symbol info, the quote and bar streams) and
      authorization (`authorize`, token renewal, `SchwabApi`) are real. See
      Schwab data source under In progress, and Venues under Projects.

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
- No pulling of quotes when the feed goes quiet: it only acts when a quote
  arrives.
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
  (`mUnconfirmed`) are cleared only by that `OnFill`. If it never comes, the
  room on that side stays held for the rest of the run (logged once after
  30 s): safe, but the side can stop quoting for good. Ways it can never come:
  the runner drops a pushed fill older than `kSeenFillHorizon`; a fill stamped
  before the run started (clock skew) is never polled; and an order
  acknowledged as filled is assumed filled at the size asked for, so if a live
  venue fills a different (rounded) quantity the fills never add up to it.
  There is no reconciliation yet (see the project item under Strategies).
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
- Orders fill at the close of the bar that triggered them, plus slippage and
  fees. Resting limit and stop orders can only trigger at closes, never on an
  intrabar high or low.
- An order placed in `OnStart` is rejected because no quote exists yet.
- The summary has no annualized figures (Sharpe and the like); one day of
  data is too little for them. Its realized P&L gives the starting holding
  the first mark as cost basis, so on a trending day it includes the move on
  what was sold; "vs hold" is the figure that leaves the move out.
- Markouts take the mid from the replayed quotes: at the fill, the last mid
  stamped before the fill's time, so a quote in the same millisecond is not
  counted as before it. A fill closer to the end than a horizon is left out
  of that horizon. Fees are not included.
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
- The Binance.US venue runs live under `run`, polled for fills (no user-data
  stream yet). Its `GetPositions` returns nothing, so the risk gate's
  `max_position_notional` does not see holdings the account already had.
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
- Schwab allows one streamer connection per user, so only one process can
  stream from Schwab at a time: when a second one logs in (another
  `kestrel_app`, or thinkorswim), the first stops streaming, logs an error and
  its subscriptions go quiet. With `[schwab] takeover_reconnect_ms` set it
  reconnects after that wait instead, taking the connection back; two
  processes that both set it take it from each other at every wait.
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

## Projects

### Venues
Each stub's header has a TODO comment listing the endpoints and auth it needs.
- [ ] **Kraken trading venue.** REST orders, balances, open orders and trade
      history, signed with the existing HMAC helper. Data source is already
      done.
- [ ] **Schwab trading venue.** Orders, open orders, fills, positions and
      balances on the Trader API, after the data source (see Schwab data
      source under In progress). It can reuse `SchwabAuth` and `SchwabApi`,
      which already handle tokens, renewal and retrying once after a 401;
      order placement has its own rate limit.
- [ ] **Binance.US trading venue.** The data source is done. The venue is
      reads checked against the live API on 2026-10-28 with `kestrel_app
      account`. Orders are built: `PlaceOrder` (LIMIT GTC/IOC/FOK, MARKET,
      STOP_LOSS; `kst-` client order IDs; never resent, looked up by client
      ID after a timeout or 5xx), `CancelOrder` (-2011 looked up to tell
      filled from closed), `CancelAll`, an order-rate limit (9/s, 10 burst,
      against Binance's 100 per 10 s), and the minimum order value
      (`SymbolInfo::min_notional`, also enforced by the paper venue). The test
      endpoint accepted limit buy, market buy and limit sell requests on
      2026-10-29 and refused a price under the band (-1013 PERCENT_PRICE).
      A real round trip on 2026-10-29 (limit buy 0.002 ZEC at 700, 46%
      under the market, order 412845258) checked the rest: the RESULT reply,
      the open order and 1.40 USDT locked, a cancel from a fresh process
      (symbol found by lookup), the balance released with no fill, and a
      second cancel answered -2011 and reported as already closed.
      Leftover orders: `Runner::Begin` now cancels every order open on the
      configured symbols before `OnStart`, and refuses to start if it cannot
      (Kestrel assumes it alone controls the account's orders while it runs).
      `HasLiveVenue` is now true for Binance.US, so `run` trades live on it.
      Exchange info advertises 6000
      request weight a minute, not 1200; the client now keeps to 2400 (it
      was 960).
- [ ] Order-side latency: order acknowledgement, cancel acknowledgement, time
      until a fill shows in `GetFills`, and the venue's clock against ours.
      Needs order placement first; store it beside `venue_latency`.
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

### Strategies
- [ ] Agent access (out of scope so far). What it should look
      like isn't defined yet; decide that before building it.
- [ ] Indicators library (SMA/EMA, RSI, ATR, Bollinger, MACD) as small
      reusable pieces so strategies do not each reimplement them
- [ ] EMA crossover and RSI mean-reversion
- [ ] Breakout / Donchian channel with an ATR stop
- [ ] Grid trading and dollar-cost averaging
- [ ] Multi-symbol strategies (pairs trading, momentum rotation across a basket)
- [ ] Position sizing helpers: fixed fractional, volatility-targeted

#### Market maker
Roughly in order of expected value. The backtest markouts show where the P&L comes
from: on the Oct 30 ZEC/USDT day, 2 bps quotes captured +1.37 bps at the
fill and were at -1.02 bps a second later (picked off), and 6 bps quotes
made +0.77 USDT in the five minutes after their fills but +9.99 by the end
(inventory riding the price). Check a change's markouts, not only "vs hold".
- [ ] Keep queue position: leave a resting quote alone while it is still at
      the touch even if its target moved, and optionally join the touch (or
      improve it by a tick) instead of quoting a computed price. Each
      replacement goes to the back of the queue (see Known limitations), and
      on wide-tick pairs queue position is most of the edge.
- [ ] Pull the quotes when the feed goes quiet, on a timer rather than on
      the next quote (see Known limitations), so stale quotes are not filled
      after a move the strategy never saw.
- [ ] Stronger inventory control: a skew that grows faster than linearly
      with the position (Avellaneda-Stoikov style) and with how long the
      position has been held. The current skew is linear in the position.
- [ ] Quote several levels a side with smaller sizes, for pairs that swing
      enough to fill them.
- [ ] Persist the halt state and protection P&L, so a restart does not reset
      the loss and drawdown caps (see Known limitations).
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
- [ ] Record quotes continuously: `stream --store` under systemd (a loop
      that restarts it) on the server, for several pairs, so backtests are not
      drawn from 32 hours of one pair. Mind the retention item below.
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
- [ ] CI: build and run tests on every push, with a clang-tidy job and the
      ASan/UBSan and TSan builds above. TSan is worth having, since the runner
      and venues are multithreaded.
- [ ] The database tests share fixture rows, so they cannot run in parallel
      (`ctest -j` fails them); give each test its own venue name or schema
- [ ] Sanitize the Docker image's build as well, and run TSan against a long
      paper run (`kestrel_app run`), which the unit tests do not cover
- [ ] Integration tests against venue sandboxes or recorded responses
- [ ] Shorting and margin in the paper venue and the P&L tracker


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
      polling to `OnFill`), example `SmaCrossover` (tested with fakes)
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
- [x] Run the runner against Binance.US data end to end in paper mode: an 8 hour
      `market_maker` session traded, recorded and stopped cleanly
- [x] `backtest` loads bars from `BarStore` first and falls back to the
      provider (`LoadReplayData`), saving what it fetched
- [x] Document how to write a strategy (docs/writing-strategies.md: hooks,
      threading rules, `Context`, the risk gate)
- [x] Backtest summary: equity, return, return against holding the starting
      balances, max drawdown, realized and unrealized P&L, fills, volume,
      fees, closing sells won, inventory range, and the equity curve sampled
      every minute (`--equity-csv`)
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
