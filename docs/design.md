# Design

Kestrel is a library plus a thin command-line app. Everything the strategy
code touches is an interface, so the same `Runner` and strategy run against a
live feed, a recorded one (backtest), or a paper or live venue. Market data and
order execution are separate interfaces (`data::DataSource`,
`trading::TradingVenue`); only the execution side changes between paper and
live trading, and a factory picks the right one from the config.

## How information flows

```
  Provider: Kraken, Binance.US, Schwab (REST + WebSocket)
        |
        v
  DataSource  ----------------------------->  --store: BarStore, QuoteWriter
  GetBars, GetSymbolInfo,                              |
  StreamQuotes, StreamBars                             v
        |                                      PostgreSQL / TimescaleDB
        | bars, quotes (WebSocket thread, enqueue only)  ^
        v                                                |
  Runner: event queue, warmup, gap backfill              |
        |                                                |
        | one event at a time (Run() thread)             |
        v                                                |
  Strategy: OnBar / OnQuote / OnFill / OnOrderUpdate     |
        |                       ^                        |
        | PlaceOrder,           | fills and order        |
        | CancelOrder           | changes, pushed/polled |
        v                       |                        |
  OrderTracker  (remembers active orders)                |
        |                       |                        |
        v                       |                        |
  RiskGatedVenue  (optional: limits, kill switch)        |
        |                       |                        |
        v                       |                        |
  TradingVenue: PaperTradingVenue or a live venue         |
        ^                                                |
        | quotes (paper only: matches resting orders)    |
        +---- DataSource                                 |
                                                         |
  Runner --> RunRecorder (fills + marks) --> PnlTracker  |
                  |                                      |
                  v                                      |
              TradeLog = TradeStore ---------------------+
              (runs, fills, pnl_snapshots, venue_latency)
```

In words:

1. The **data source** turns a provider's REST and WebSocket APIs into bars,
   quotes and symbol info, with no provider types leaking out.
2. The **runner** subscribes to what the strategy's `Needs()` asks for, loads
   warmup bars, and queues the callbacks from the WebSocket thread. A single
   thread takes events off the queue and calls the strategy's hooks.
3. The **strategy** places orders through `Context::Venue()`. The call passes
   through the order tracker and the optional risk gate to the real venue.
4. The runner hands fills and order changes back to the strategy as `OnFill`
   and `OnOrderUpdate`. A venue that **pushes executions**
   (`TradingVenue::StreamExecutions`; today the paper venue) has them queued
   as events the moment they happen, and the runner **polls** only as a
   backstop every `fill_backstop_interval` (30 s). Other venues are polled
   every `fill_poll_interval` (1 s).
5. If recording is on, the runner also gives each fill and the latest prices to
   the **recorder**, which keeps P&L and writes it to the database.

`kestrel_app backtest` is the same picture with a `ReplayDataSource` in place
of the live one: `RunBacktest` pushes recorded bars and quotes in, in time
order, on one thread and a simulated clock.

## Major classes

| Class | Role |
|---|---|
| `Configuration` (`config/`) | The parsed TOML file plus credentials from the environment or the secrets file. Rejects credentials in the main config and unknown keys. |
| `factory.hpp` | The only place that knows which concrete class goes with a provider name or strategy name. `CreateComponents` makes the data source and venue, `CreateStrategy` the strategy, `ResolveRiskLimits` the limits, and `CreateRunner` wires them into a ready-to-run `Runner`. |
| `DataSource` (`data/`) | Read-only market data: `GetBars`, `GetSymbolInfo`, and `StreamQuotes`/`StreamBars`, which return a `Subscription` to stop. Implemented by `KrakenDataSource`, `BinanceUSDataSource` and `ReplayDataSource`; `SchwabDataSource` is a stub. |
| `TradingVenue` (`trading/`) | Execution: place and cancel orders, list open orders and fills, positions and balances, and optionally push executions. Implemented by `PaperTradingVenue` and `BinanceUSTradingVenue`; the Kraken and Schwab venues are stubs. |
| `PaperTradingVenue` | A simulated exchange built on any `DataSource`. It fills orders from the quote stream with fees, slippage and a configurable limit-fill model, keeps balances, and pushes its fills. |
| `BinanceUSTradingVenue` / `BinanceUSApi` | The live Binance.US venue. `BinanceUSApi` signs requests, keeps the server clock offset, holds calls to the request-weight budget and stops calling during a ban; the venue maps orders, cancels, fills and balances onto it, and never sends a placement twice. |
| `RiskGatedVenue` | A `TradingVenue` that wraps another one and refuses orders that break a limit (size, open orders, order rate, daily loss, drawdown, symbol allowlist). It has a kill switch, `Halt()`. |
| `ApplySymbolRules` | Rounds an order to the symbol's tick and lot and refuses one under its minimum size or value; used by every venue. |
| `PnlTracker` | The single average-cost P&L calculation, built from fills alone. Used by the paper venue, the risk gate and the recorder. |
| `Strategy` / `Context` (`strategy/`) | A strategy implements the hooks (`OnStart`, `OnBar`, `OnQuote`, `OnFill`, `OnOrderUpdate`, `OnStop`) and declares `Requirements`. `Context` is what it can see: the clock, its venue, symbol info and its bar history. `SmaCrossover` and `MarketMaker` are the two included. |
| `Runner` | Owns one strategy. Cancels every order already open on the venue's symbols before the strategy starts (it assumes sole control of the account's orders). Queues events (conflating quotes per symbol), runs the strategy on one thread, takes fills and order updates pushed by the venue or polls for them, and puts the order tracker and risk gate between the strategy and the venue. `Begin`/`Drain`/`End` let a backtest drive it step by step. |
| `TimedHttpClient` (`net/`) | Wraps any `HttpClient` and times each call into `LatencyStats`, per endpoint. The factory wraps every client it builds, so all venue traffic is measured. |
| `RunRecorder` | Inside the runner. Tracks P&L from the fills the runner sees and the latest marks, and writes fills, periodic snapshots and response-time windows to a `TradeLog`. Failures are logged and retried, never fatal. |
| `TradeLog` / `TradeStore` | `TradeLog` is the recorder's output interface; `TradeStore` implements it on PostgreSQL (`runs`, `fills`, `pnl_snapshots`, `venue_latency`). |
| `Database`, `BarStore`, `QuoteStore`/`QuoteWriter`, `AccountFillStore` (`db/`) | The connection, schema migrations, and the bar, quote and account-fill tables. `QuoteWriter` buffers on the WebSocket thread and inserts from a background thread. |
| `app/commands/*` | One class per subcommand. Each parses its arguments, calls the factory and drives the result; `run` also owns the signal-watching thread that calls `Runner::Stop()`. |

Dependencies point one way: `app` uses `factory`, which uses everything else;
`strategy` uses the `DataSource` and `TradingVenue` interfaces but no concrete
provider; the providers know nothing about strategies.

## Threads

- **WebSocket thread** (one per stream): runs the data source's callbacks. The
  runner's callbacks only enqueue, and `QuoteWriter` only buffers. Do not call
  `Stop()` or destroy a subscription from inside a callback. The paper venue
  pushes executions from here too (a resting order filled by a quote), or
  from the Run() thread when an order fills or closes inside `PlaceOrder` /
  `CancelOrder`; the runner's execution callback also only enqueues.
- **Run() thread**: the only thread that calls the strategy. It is free to call
  the venue over HTTP, take fills, and write to the database.
- **Watch thread** (`run` only): collects SIGINT and SIGTERM and calls
  `Runner::Stop()`, which is not async-signal-safe.
- **Quote writer thread** (`--store` only): inserts buffered quotes in batches.

## Design notes

- **Decimal**: int64 mantissa + scale (0–18). No floating point for prices or
  quantities; JSON numbers are parsed from their literal text
  (`util::ParseJson`). `RoundToIncrement(inc, kUp|kDown|kNearest)`; up/down are
  toward ±infinity, nearest breaks ties away from zero.
- **Errors**: recoverable failures return `std::expected<T, types::Error>`
  (category, message, HTTP status, provider code, retry-after). Exceptions are
  for programming errors (e.g. `Decimal` overflow, bad scale).
- **Retries / limits**: `util::Retry` does exponential backoff with jitter on
  transient categories (network, timeout, rate limit, server) and honors
  `Retry-After`; `util::RateLimiter` is a token bucket (Kraken public REST: 1/s;
  Binance.US request weight and new orders). Writes to a venue are never
  retried blindly, since the first attempt may have taken effect.
- **Timestamps**: `std::chrono::sys_time<milliseconds>` (UTC).
- **Threading**: quote callbacks run on IXWebSocket's background thread. Do not
  call `Stop()` or destroy the subscription from inside the callback.
  `PaperTradingVenue` guards all state with one mutex and does no network I/O
  while holding it.
- **Kraken bars**: `GetBars` returns `start <= open < end`, excluding the
  still-forming bar. Kraken's OHLC endpoint only keeps the latest 720 bars per
  interval, so older ranges return `kUnsupported` rather than truncated data.
  The v2 ticker has no last-trade size, so `Quote::last_size` is empty.
- **Paper venue**: spot only, no shorting. Market orders fill at ask/bid ±
  slippage (taker fee); marketable limits take the touch; resting limits fill
  at their price (maker fee); stops become market orders when triggered.
  Buys reserve funds including worst-case fees (`Balance::locked`). Supports
  GTC/IOC/FOK/Day and optional partial fills from quote size. Positions only
  track quantity bought through the venue (initial holdings have no cost basis).
  `GetFills(since)` returns fills with `time >= since`.
- **Timeframes**: Schwab 1m 5m 10m 15m 30m 1d 1w 1M; Kraken 1m 5m 15m 30m 1h 4h
  1d 1w 15d; Binance.US 1m 3m 5m 15m 30m 1h 2h 4h 6h 8h 12h 1d 3d 1w 1M.
  Unsupported ones return `kUnsupported`. These lists come from memory of each
  provider's docs and should be re-verified before the stubs are completed (see
  [TODO.md](../TODO.md)).

## Source layout

```
include/kestrel/   public headers, one directory per area:
  types/             Decimal, Symbol, Timeframe, orders, errors, latency
  util/              retry, rate limiter, HMAC, JSON
  net/               HTTP client and its timing wrapper
  config/            configuration, credentials, Schwab token store
  data/              DataSource and the provider data sources, streams
  db/                database; bar, quote, trade and account-fill stores
  trading/           TradingVenue, paper and live venues, Binance.US API,
                     order rules, PnlTracker, RiskGatedVenue
  strategy/          Strategy, Runner, RunRecorder, SmaCrossover, MarketMaker
  backtest/          ReplayDataSource, LoadReplayData, RunBacktest
  factory.hpp        builds a provider's data source and venue, a strategy by
                     name, resolved risk limits, and a ready-to-run Runner
src/               implementation, same directories (plus factory.cpp)
app/main.cpp       kestrel_app: parses the command line and runs one command
app/commands/      one class per subcommand (bars, account, order, cancel,
                   stream, stream-bars, init-db, run, backtest);
                   command.hpp is the interface
app/common.*       helpers shared by the commands (argument validators, printing)
tests/             kestrel_tests; tests/data and cli_check.cmake drive the cli_* tests
                   that run the built kestrel_app with bad arguments
config/            example configuration files
docs/              this documentation
scripts/           database setup and test-data export
cmake/             dependency handling
docker/            the test image's entrypoint
```
