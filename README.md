# Kestrel

A C++23 library for gathering stock and cryptocurrency prices and trading on
them. Providers: **Schwab**, **Kraken**, **Binance.US**.

Market data and order execution are separate interfaces (`data::DataSource`,
`trading::TradingVenue`). Only the execution side changes between paper and
live trading; a factory picks the right one from the config.

Status, known limitations, work in progress and planned projects are tracked in
[TODO.md](TODO.md).

## Build

Requires CMake 3.25+ and GCC 12+/Clang 16+ (`std::expected`). The `Decimal`
implementation uses `__int128`, so MSVC is not supported yet.

```sh
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure     # or ./build/kestrel_tests
```

Dependencies are found with `find_package` first and fetched with
FetchContent otherwise (see `cmake/KestrelDependencies.cmake`): cpr (system
libcurl preferred via `CPR_USE_SYSTEM_CURL`), IXWebSocket (TLS via OpenSSL),
nlohmann/json, OpenSSL, toml++, spdlog, GoogleTest. The first configure needs
network access if any are missing. The cpr FetchContent fallback is written but
has not been exercised here (the system cpr was found); see [TODO.md](TODO.md).

Targets: `kestrel` (library), `kestrel_app`, `kestrel_tests`.
Options: `-DKESTREL_BUILD_APP=OFF`, `-DKESTREL_BUILD_TESTS=OFF`.

## Configure

```sh
cp config/kestrel.example.toml config/kestrel.toml
```

`[general]` sets `provider`, `mode` (`paper` | `live`) and `symbols`.
Symbols are normalized everywhere outside provider classes: `BTC/USD`, `AAPL`.
`[paper]` sets starting balances, maker/taker fees and slippage (in bps).

**Credentials never go in the main config** (it is rejected if they do). Use
either environment variables or a git-ignored secrets file
(`config/secrets.example.toml` → `config/secrets.toml`, `chmod 600`):

| Provider | Environment variables |
|---|---|
| Kraken | `KESTREL_KRAKEN_API_KEY`, `KESTREL_KRAKEN_API_SECRET` |
| Binance.US | `KESTREL_BINANCEUS_API_KEY`, `KESTREL_BINANCEUS_API_SECRET` |
| Schwab | `KESTREL_SCHWAB_APP_KEY`, `KESTREL_SCHWAB_APP_SECRET` |

Environment variables win over the file. `KESTREL_SECRETS_FILE` or
`[general] secrets_file` points at a different file. Kraken market data needs
no credentials; paper mode needs none at all. Live mode without credentials
fails at startup.

**Schwab tokens** are persisted to `[schwab] token_file` (mode 0600). The 7-day
refresh token can only be replaced by a human re-authorizing in a browser, so
Kestrel logs a warning in the last 24 hours and a `SCHWAB RE-AUTHORIZATION
REQUIRED` error, with the authorize URL, once it has expired.

## Run

```sh
./build/kestrel_app --config config/kestrel.toml bars 1h 24
```

`kestrel_app` takes the global options `-c/--config` (required) and
`-v/--verbose`, then one subcommand, which does only what its name says.
`kestrel_app --help` and `kestrel_app <subcommand> --help` list the arguments.

| Subcommand | Does |
|---|---|
| `bars <timeframe> <count> [--store]` | the last `count` completed bars per symbol, preceded by symbol info and followed by the venue's balances |
| `stream <seconds> [--store]` | live quotes for that many seconds |
| `stream-bars <timeframe> <seconds> [--store]` | completed bars as they close, for that many seconds (providers that offer it) |
| `init-db` | creates or updates the database schema |
| `run <strategy>` | trades a strategy on live data (see Strategies below) |
| `backtest <strategy> --from <date> --to <date>` | replays a strategy over historical bars |

The example above prints the last 24 hourly bars. The global options may also
come after the subcommand's arguments.

### Strategies

```sh
./build/kestrel_app -c config/kestrel.toml backtest sma_crossover --from 2026-09-01 --to 2026-10-01
./build/kestrel_app -c config/kestrel.toml run sma_crossover
```

- `backtest <strategy> --from <date> --to <date>` fetches the strategy's
  bars (plus its warmup) from the provider, replays them through the same
  `Runner` and a paper venue using `[paper]`, and prints the fills, positions
  and balances. `--to` is exclusive; dates are `YYYY-MM-DD` or ISO 8601 UTC.
- `run <strategy>` trades on live data through the paper or live venue until
  Ctrl-C or SIGTERM (a second signal exits immediately). If `[risk]` sets any
  limit, orders go through the risk gate, and the daily-loss count starts at
  startup. Without `[risk]` it warns and trades unchecked in paper mode. Live
  mode is refused without `[risk]`, and on any provider whose trading venue is
  still a stub (currently all of them).
- Risk limits come from `[risk]`, with `[risk.<strategy>]` laid over it key by
  key for that strategy (a section naming an unknown strategy is an error).
  Besides the size, rate and daily-loss limits, `max_orders_per_day`,
  `max_consecutive_losses` and `max_drawdown` suit a trend follower like
  `sma_crossover`. When one of the loss, drawdown or order-count limits is
  hit, only orders that reduce a position are allowed; the gate does not close
  the position itself.
- The strategy's parameters come from `[strategy]` in the config. Unknown
  parameters and unknown `[risk]` keys are errors. `sma_crossover` takes
  `symbol` and `quantity` (required), `timeframe` (`1h`), `fast_period` (10)
  and `slow_period` (30); the symbol must be listed in `[general] symbols`.
- `market_maker` also takes opt-in drawdown protection, switched on one rule at
  a time in `[strategy]`: `stop_loss`, `loss_cap`, `drawdown_cap`,
  `spike_standdown`, `downtrend_filter`, `momentum_filter` and
  `cooldown_after_stop` (each `true` / `false`, default `false`, with its own
  parameters). Unlike the `[risk]` limits they are checked on every quote and
  the stop and cap rules close the position instead of only blocking entries.
  See `config/binanceus-zec-marketmaker.example.toml`.

### Recording a run

With `[recording] enabled = true` (and a `[database]`), `run` writes what it
does to the database as it goes, so a long run can be watched from `psql` and
studied afterwards:

| table / view | contents |
|---|---|
| `runs` | one row per run: strategy, venue, mode, `started_at`, `ended_at` (NULL until a clean stop, so NULL on an old run means the process died) |
| `fills` | every fill of the run, with fee, fee asset and maker/taker |
| `pnl_snapshots` | every `pnl_interval_ms` (default 60 s) and at the end: per symbol, the position, average price, mark price, realized and unrealized P&L |
| `pnl_totals` | view summing the snapshots over symbols (only meaningful when they share a quote asset) |
| `venue_latency` | every `latency_interval_ms` (default 60 s) and at the end: per endpoint, the calls, errors, rate-limited calls (HTTP 429/418) and min / p50 / p95 / p99 / max response time in ms |

```sql
SELECT * FROM pnl_totals WHERE run_id = (SELECT max(run_id) FROM runs)
ORDER BY snapshot_time DESC LIMIT 10;
```

Response times come from the timing wrapper around every HTTP client the
factory builds (`TimedHttpClient`). Each call is timed for one attempt (a
retry is another sample) and filed under "METHOD url" without the query, so
signatures and parameters never reach the log or the table. `run` also logs the
whole-run figures per endpoint when it stops, and warns about any call at least
`[http] slow_request_ms` (default 2000) slow. `[recording] latency = false`
leaves the table empty; `latency_interval_ms` sets the window. Percentiles are
worked out from the last 4096 calls of each endpoint in the window (the counts,
min and max cover all of them), and a window that cannot be written is lost
rather than retried.

```sql
SELECT window_start, calls, errors, p50_ms, p99_ms, max_ms FROM venue_latency
WHERE run_id = 4 AND endpoint = 'GET https://api.binance.us/api/v3/exchangeInfo'
ORDER BY window_start;
```

This times the HTTP round trip only. The order-side figures (acknowledgement,
cancel, time until a fill is visible) arrive with order placement.

P&L is the same average-cost figure the risk gate uses, realized net of fees,
starting from the positions the venue reported at startup; the mark is the
last quote mid or bar close. A failed write is logged and retried, never fatal
to the run; if the database cannot be reached at startup, `run` refuses to
start. `backtest` does not record.

## Database

Bars are stored in PostgreSQL with the TimescaleDB extension (`bars`
hypertable keyed by venue, symbol, timeframe, open_time; prices and volumes are
exact `NUMERIC`). Needs `libpq-dev` to build and a server with
`shared_preload_libraries = 'timescaledb'`.

```sh
scripts/setup_database.sh --config config/kestrel.toml
```

The script (idempotent; `--help` lists options) creates the `kestrel` role and
database, enables the timescaledb extension, and creates the tables through
`kestrel_app init-db`. It runs `psql` as the `postgres` OS user via sudo, or
as whatever `--admin-cmd` / `KESTREL_ADMIN_PSQL` says for a remote server. A
password is generated for a new role and printed once (or set
`KESTREL_DATABASE_PASSWORD` beforehand). Use `host = "localhost"` in
`[database]` when the role has a password, since the local socket normally
uses peer authentication.

Add a `[database]` section (see `config/kestrel.example.toml`; the password
comes from `KESTREL_DATABASE_PASSWORD`, the secrets file, or libpq's usual
`PGPASSWORD`/`~/.pgpass`). Schema migrations are applied automatically on
startup (`src/db/migrations.cpp`, append-only).

### Upgrading an existing database

After updating Kestrel, bring an existing database up to the new schema with:

```sh
build/kestrel_app --config config/kestrel.toml init-db
```

Any command with `--store` also migrates first, but running `init-db` yourself lets you choose when it
happens. Migrations are tracked in the `schema_migrations` table: only those
newer than the highest recorded version are applied, each one is logged
(`database: applying migration N (...)`), and a database that is already
current is left untouched. A lock keeps two processes from migrating at once.

Before upgrading a database that holds data you care about:

1. Back it up (`pg_dump`).
2. Stop any running `kestrel_app` that writes to it, and restart the new build
   after the migration. An older binary may not match the new schema.
3. Check the release's commits and `src/db/migrations.cpp` for what each new
   migration does. Some rewrite whole tables, which takes time, disk space and
   locks.
4. Run `init-db`, then confirm with
   `SELECT * FROM schema_migrations ORDER BY version;`.

If a migration fails, the error names its version. Fix the cause and re-run; if
you are unsure whether it applied partway, compare the schema against the
migration's SQL, or restore the backup.

```sh
./build/kestrel_app -c config/kestrel.toml bars 1h 500 --store          # pull and store
./build/kestrel_app -c config/kestrel.toml stream-bars 1m 3600 --store  # stream closed bars into the DB
```

`stream <seconds> --store` writes every streamed quote to the `quotes`
hypertable (chunked per day, one row per delivered quote with the merged
bid/ask/last). `QuoteWriter` buffers on the WebSocket thread and a background
thread inserts in batches every 500 ms; a failed batch is retried, with at most
100,000 quotes buffered. Each quote gets a `seq`, counted per venue and symbol by `QuoteWriter`
and continued from the database after a restart, so arrival order survives
quotes that share a millisecond; reads are ordered by it. `bookTicker` updates are frequent: expect many rows per symbol per minute.

`stream-bars` (`DataSource::StreamBars`, implemented for Binance.US klines)
emits each bar once, when it closes. Bars closed while the socket was down are
not replayed; backfill with `bars --store` (saves are upserts, so overlap is
harmless).

### Database integration tests

The database tests run only when `KESTREL_TEST_DBNAME` is set (plus optional
`KESTREL_TEST_DB{HOST,PORT,USER,PASSWORD}`); otherwise they are skipped. **No
venue API keys are needed**: real Kraken and Binance.US bars and quotes are
checked in under `tests/data/market/`, and the tests load them into the
database themselves. What you need to do:

1. Install PostgreSQL and the TimescaleDB package for your PostgreSQL version
   (Ubuntu: `postgresql-<version>-timescaledb`), and enable it in
   `postgresql.conf` with `shared_preload_libraries = 'timescaledb'`
   (`timescaledb-tune` can do this), then restart PostgreSQL.
2. Run `scripts/setup_test_database.sh`. It must run as a PostgreSQL admin
   (by default through `sudo -u postgres psql`), because creating the
   database and enabling the extension need superuser rights. The tests
   cannot do this for you: their `kestrel_test` role is deliberately not a
   superuser and cannot create databases.
3. Export the variables it prints and run the tests:

```bash
export KESTREL_TEST_DBNAME=kestrel_test
export KESTREL_TEST_DBHOST=localhost
export KESTREL_TEST_DBUSER=kestrel_test
export KESTREL_TEST_DBPASSWORD=kestrel_test
ctest --test-dir build --output-on-failure
```

The script wraps `scripts/setup_database.sh` (see its `--help`, e.g.
`--admin-cmd` if `sudo -u postgres psql` doesn't work on your machine). The
tests run the migrations, then replace the rows of two venues named
`fixture-kraken` and `fixture-binanceus`; the other store tests only touch the
venue `kestrel-test`. Use a separate database from your real one anyway.

`tests/db_fixture.hpp` has the shared helpers (`ConnectTestDatabase`,
`ReadFixtureData`, `LoadFixtureData`); `tests/fixture_data_test.cpp` is the
suite built on them. To refresh the data, store a new pull into a scratch
database (`kestrel_app bars 1h 168 --store`, `kestrel_app stream 90 --store`)
and run `scripts/export_test_data.sh` with the `PG*` variables pointing at it.

### Sanitizers

`-DKESTREL_SANITIZE=` builds everything, the fetched dependencies included,
with AddressSanitizer, UndefinedBehaviorSanitizer or ThreadSanitizer. Use a
separate build directory for each; `build-*/` is git-ignored.

```sh
cmake -S . -B build-asan -DKESTREL_SANITIZE=address,undefined
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure

cmake -S . -B build-tsan -DKESTREL_SANITIZE=thread
cmake --build build-tsan -j
ctest --test-dir build-tsan --output-on-failure
```

Allowed values are `address`, `undefined`, `address,undefined` and `thread`
(thread cannot be combined with address). An undefined-behavior finding aborts
the test instead of printing and carrying on. A sanitized build is several
times slower to compile and run, and the first configure fetches any
dependency that is not installed. TSan is the one to run after touching the
runner, a venue, a stream or `QuoteWriter`, since they are multithreaded.

Run the database tests serially (plain `ctest`, not `-j`): they share the
fixture venues' rows, so parallel runs collide.

### Running the tests in Docker

The `Dockerfile` (Ubuntu 26.04) installs the build dependencies, PostgreSQL 18
with TimescaleDB and the `kestrel_test` database, so the whole suite, database
tests included, runs without installing anything on the host. Only building the
image needs network access (apt and an IXWebSocket clone); running it does not.

```sh
docker build -t kestrel-test .
docker run --rm --network none kestrel-test
```

The container starts PostgreSQL, builds `/src` into `/build` and runs `ctest`.
Arguments after the image name go to `ctest`. Mount your working tree over
`/src` to test local changes without rebuilding the image:

```sh
docker run --rm --network none -v "$PWD":/src kestrel-test -R fixture
```

The build runs 2 compile jobs by default because parallel C++23 compiles can
run a small container out of memory (the compiler is killed); change it with
`-e KESTREL_JOBS=N`. The source is compiled on every run.

To skip the entrypoint and get a shell (to run a single test, use `perf`,
or poke at the database), override it. PostgreSQL is not started for you in
that case:

```sh
docker run --rm -it --network none -v "$PWD":/src --entrypoint bash kestrel-test
# inside the container:
pg_ctlcluster "$PG_VERSION" main start
cmake -S /src -B /build -G Ninja && cmake --build /build -j2
/build/kestrel_tests --gtest_filter='BarStoreTest.*'
```

The image includes `perf`. Docker's default seccomp profile blocks the
`perf_event_open` syscall, so profiling needs
`--cap-add SYS_ADMIN --security-opt seccomp=unconfined` on the `docker run`
(and a host `kernel.perf_event_paranoid` that permits it).

## Design

Kestrel is a library plus a thin command-line app. Everything the strategy
code touches is an interface, so the same `Runner` and strategy run against a
live feed, a recorded one (backtest), or a paper or live venue.

### How information flows

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

### Major classes

| Class | Role |
|---|---|
| `Configuration` (`config/`) | The parsed TOML file plus credentials from the environment or the secrets file. Rejects credentials in the main config and unknown keys. |
| `factory.hpp` | The only place that knows which concrete class goes with a provider name or strategy name. `CreateComponents` makes the data source and venue, `CreateStrategy` the strategy, `ResolveRiskLimits` the limits, and `CreateRunner` wires them into a ready-to-run `Runner`. |
| `DataSource` (`data/`) | Read-only market data: `GetBars`, `GetSymbolInfo`, and `StreamQuotes`/`StreamBars`, which return a `Subscription` to stop. Implemented by `KrakenDataSource`, `BinanceUSDataSource` and `ReplayDataSource`; `SchwabDataSource` is a stub. |
| `TradingVenue` (`trading/`) | Execution: place and cancel orders, list open orders and fills, positions and balances. Implemented by `PaperTradingVenue`; the Kraken, Schwab and Binance.US venues are stubs. |
| `PaperTradingVenue` | A simulated exchange built on any `DataSource`. It fills orders from the quote stream with fees, slippage and a configurable limit-fill model, and keeps balances. |
| `RiskGatedVenue` | A `TradingVenue` that wraps another one and refuses orders that break a limit (size, open orders, order rate, daily loss, drawdown, symbol allowlist). It has a kill switch, `Halt()`. |
| `PnlTracker` | The single average-cost P&L calculation, built from fills alone. Used by the paper venue, the risk gate and the recorder. |
| `Strategy` / `Context` (`strategy/`) | A strategy implements the hooks (`OnStart`, `OnBar`, `OnQuote`, `OnFill`, `OnOrderUpdate`, `OnStop`) and declares `Requirements`. `Context` is what it can see: the clock, its venue, symbol info and its bar history. `SmaCrossover` and `MarketMaker` are the two included. |
| `Runner` | Owns one strategy. Queues events (conflating quotes per symbol), runs the strategy on one thread, takes fills and order updates pushed by the venue or polls for them, and puts the order tracker and risk gate between the strategy and the venue. `Begin`/`Drain`/`End` let a backtest drive it step by step. |
| `TimedHttpClient` (`net/`) | Wraps any `HttpClient` and times each call into `LatencyStats`, per endpoint. The factory wraps every client it builds, so all venue traffic is measured. |
| `RunRecorder` | Inside the runner. Tracks P&L from the fills the runner sees and the latest marks, and writes fills, periodic snapshots and response-time windows to a `TradeLog`. Failures are logged and retried, never fatal. |
| `TradeLog` / `TradeStore` | `TradeLog` is the recorder's output interface; `TradeStore` implements it on PostgreSQL (`runs`, `fills`, `pnl_snapshots`, `venue_latency`). |
| `Database`, `BarStore`, `QuoteStore`/`QuoteWriter` (`db/`) | The connection, schema migrations, and the bar and quote tables. `QuoteWriter` buffers on the WebSocket thread and inserts from a background thread. |
| `app/commands/*` | One class per subcommand. Each parses its arguments, calls the factory and drives the result; `run` also owns the signal-watching thread that calls `Runner::Stop()`. |

Dependencies point one way: `app` uses `factory`, which uses everything else;
`strategy` uses the `DataSource` and `TradingVenue` interfaces but no concrete
provider; the providers know nothing about strategies.

### Threads

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

### Design notes

- **Decimal**: int64 mantissa + scale (0–18). No floating point for prices or
  quantities; JSON numbers are parsed from their literal text
  (`util::ParseJson`). `RoundToIncrement(inc, kUp|kDown|kNearest)`; up/down are
  toward ±infinity, nearest breaks ties away from zero.
- **Errors**: recoverable failures return `std::expected<T, types::Error>`
  (category, message, HTTP status, provider code, retry-after). Exceptions are
  for programming errors (e.g. `Decimal` overflow, bad scale).
- **Retries / limits**: `util::Retry` does exponential backoff with jitter on
  transient categories (network, timeout, rate limit, server) and honors
  `Retry-After`; `util::RateLimiter` is a token bucket (Kraken public REST: 1/s).
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
  [TODO.md](TODO.md)).

## Writing a strategy

A strategy is a class derived from `strategy::Strategy`
(`include/kestrel/strategy/strategy.hpp`). The runner calls its hooks; the
strategy reacts by placing orders. `SmaCrossover` is a small worked example and
`MarketMaker` a larger one.

### 1. Declare what you need

`Needs()` returns a `Requirements`, which the runner reads once before it
subscribes to anything:

| Field | Meaning |
|---|---|
| `symbols` | the symbols the strategy trades |
| `bar_timeframes` | closed-bar streams to subscribe to for every symbol (none by default) |
| `quotes` | whether to receive `OnQuote` |
| `warmup_bars` | closed bars kept per series and loaded before `OnStart`, so indicators start with a full window |

A backtest replays stored quotes only if `quotes` is true, and fetches the
warmup bars plus the range from the database or the provider.

### 2. Implement the hooks

All hooks are optional except `Name()` and `Needs()`.

| Hook | Called |
|---|---|
| `OnStart` | once, after the warmup bars are loaded and before the first event |
| `OnBar` | for each closed bar, in order; bars missed in a disconnect are backfilled |
| `OnQuote` | for each quote, if `quotes` is set; quotes are conflated, so you may skip intermediate ones |
| `OnFill` | once per fill of an order you placed, partial fills included |
| `OnOrderUpdate` | when an order you placed changes state or fills further, ending with a closed state (filled, cancelled, rejected, expired) |
| `OnStop` | once at shutdown, after the streams are closed; `run` does not cancel open orders itself, so cancel the ones you want gone |

Every hook gets a `Context&`, valid only during that call:

- `Now()`: the runner's clock (simulated in a backtest). Use it instead of
  the system clock.
- `Venue()`: the `TradingVenue` to trade through. Place and cancel orders
  here, never on a venue you obtained elsewhere: this one carries the risk gate
  and the order tracking that feeds `OnOrderUpdate`.
- `SymbolInfo(symbol)`: tick size, lot size and minimums, for rounding prices
  and quantities.
- `Bars(symbol, timeframe)`: the last closed bars, oldest first, up to
  `warmup_bars`. The span is invalidated by the next bar for that series, so
  do not keep it.

```cpp
class MyStrategy : public strategy::Strategy {
   public:
    std::string_view Name() const override { return "my_strategy"; }

    strategy::Requirements Needs() const override {
        strategy::Requirements needs;
        needs.symbols = {mSymbol};
        needs.bar_timeframes = {types::Timeframe::kH1};
        needs.warmup_bars = 50;
        return needs;
    }

    void OnBar(strategy::Context& context, const types::Symbol& symbol,
               types::Timeframe, const types::Bar&) override {
        const auto bars = context.Bars(symbol, types::Timeframe::kH1);
        if (!ShouldBuy(bars) || mPending) {
            return;
        }
        types::OrderRequest request;
        request.symbol = symbol;
        request.side = types::Side::kBuy;
        request.type = types::OrderType::kMarket;
        request.quantity = mQuantity;
        auto ack = context.Venue().PlaceOrder(request);
        mPending = ack.has_value();
    }

    void OnFill(strategy::Context&, const types::Fill&) override {
        mPending = false;  // update your own position here
    }
};
```

### 3. Rules to follow

- **Threading:** every hook runs on the runner's one strategy thread, one at a
  time, so the strategy needs no locks. Do not start your own threads that call
  the venue or the context.
- **Be quick:** events queue up behind a slow hook. Quotes are conflated, but
  bars never are.
- **Exceptions:** a hook that throws is logged and the runner moves on to the
  next event. Do not rely on that; handle errors where they happen.
- **Check every result:** `PlaceOrder` returns a `Result`. It can fail (a risk
  limit, the venue), or succeed with an acknowledgement that is already
  closed (for example an unfillable fill-or-kill order). Neither produces a
  fill, so do not wait for one.
- **Fills arrive late:** `OnFill` always comes after the fill, even from a
  venue that pushes executions (an order that fills inside `PlaceOrder` is
  reported on a later event), and a polled venue can lag by
  `fill_poll_interval` (1 s by default). Track your own pending orders and
  your own position from `OnFill` rather than assuming an order filled when
  you placed it, and count a fill the venue has already reported (an
  acknowledgement in `kFilled`, a closing `OnOrderUpdate`) against your limits
  until its `OnFill` arrives, as `MarketMaker` does.
- **Cancels race with fills:** an order can fill while your cancel is in
  flight. `CancelOrder` reports `kAlreadyFilled` or `kAlreadyClosed` for that,
  and it is a normal outcome, not an error. `MarketMaker` tracks the race as
  `cancel_raced`.
- **Round to the symbol's rules:** prices to the tick, quantities to the lot
  size, using `SymbolInfo` and `Decimal::RoundToIncrement`.
- **Use `Decimal`** for prices and quantities, never `double`.
- **Spot only:** the paper venue does not short, so do not sell more than you
  hold.
- **The risk gate sits between you and the venue.** If `[risk]` is set, an
  order that breaks a limit comes back as an error. After a loss, drawdown or
  order-count limit trips, only orders that reduce a position are allowed.
  The gate never closes your position for you; do that in the strategy, as
  `MarketMaker`'s protection rules do.

### 4. Register it

1. Add `src/strategy/my_strategy.cpp` and `include/kestrel/strategy/my_strategy.hpp`
   (and the source file to `CMakeLists.txt`, with a test under `tests/`).
2. In `src/factory.cpp`, write a `Build` function that reads the strategy's
   parameters from `[strategy]` with the `ParamReader` (`Symbol()`,
   `Quantity(key)`, `Timeframe(key, default)`, `Count(key, default)`, and so
   on), calls `p.Finish()` so an unknown parameter is an error, and returns
   the new strategy. A constructor that throws `std::invalid_argument` becomes
   a config error. Copy `BuildSmaCrossover`.
3. Add `{"my_strategy", BuildMyStrategy}` to the `kStrategies` table. That one
   entry makes `kestrel_app run my_strategy` and `backtest my_strategy` work,
   and allows a `[risk.my_strategy]` section.
4. Document the parameters in `config/kestrel.example.toml`.

### 5. Test it

`tests/strategy_fakes.hpp` has `FakeContext`, a recording `FakeVenue` and a
`FakeTradeLog`. Feed the strategy bars or quotes and assert on the orders it
placed, as `tests/sma_crossover_test.cpp` does. Then run a backtest over stored
data (`kestrel_app backtest my_strategy --from ... --to ...`). Use
`limit_fill_model = "conservative"` for anything that rests limit orders; the
default `cross` model flatters those. Paper-trade it with `run` before trusting
the result, and see [TODO.md](TODO.md) for what the paper venue and backtest do
not model.

## Layout

```
include/kestrel/   public headers, one directory per area:
  types/             Decimal, Symbol, Timeframe, orders, errors
  util/              retry, rate limiter, HMAC, JSON
  net/               HTTP client
  config/            configuration, credentials, Schwab token store
  data/              DataSource and the provider data sources, streams
  db/                database, bar and quote stores
  trading/           TradingVenue, paper and live venues, PnlTracker, RiskGatedVenue
  strategy/          Strategy, Runner, SmaCrossover
  backtest/          ReplayDataSource, LoadReplayData, RunBacktest
  factory.hpp        builds a provider's data source and venue, a strategy by
                     name, resolved risk limits, and a ready-to-run Runner
src/               implementation, same directories (plus factory.cpp)
app/main.cpp       kestrel_app: parses the command line and runs one command
app/commands/      one class per subcommand (bars, stream, stream-bars, init-db,
                   run, backtest); command.hpp is the interface
app/common.*       helpers shared by the commands (argument validators, printing)
tests/             kestrel_tests; tests/data and cli_check.cmake drive the cli_* tests
                   that run the built kestrel_app with bad arguments
config/            example configuration files
scripts/           setup_database.sh
cmake/             dependency handling
```
