# Kestrel

A C++23 library for gathering stock and cryptocurrency prices and trading on
them. Providers: **Schwab**, **Kraken**, **Binance.US**.

Market data and order execution are separate interfaces (`data::DataSource`,
`trading::TradingVenue`). Only the execution side changes between paper and
live trading; a factory picks the right one from the config.

## Status

| Piece | State |
|---|---|
| Build system, `Decimal`, shared types, `Error`/`std::expected` | done |
| `DataSource` / `TradingVenue` interfaces, `Configuration`, factory | done |
| `PaperTradingVenue` (works with any `DataSource`) | done |
| `KrakenDataSource` (REST bars + symbol info, WebSocket quotes) | done, tested live |
| `BinanceUSDataSource` (klines, symbol info, WebSocket quotes, closed-bar stream) | done |
| Database: PostgreSQL + TimescaleDB `bars` and `quotes` hypertables, `BarStore`, `QuoteStore`/`QuoteWriter`, Binance.US closed-bar stream | done, tested live |
| `PnlTracker`: realized P&L from fills, used by the paper venue and the risk gate | done, tested with fakes |
| Strategy interface, `Runner` (event queue, warmup, gap backfill, fill polling to `OnFill`), example `SmaCrossover` | done, tested with fakes |
| `RiskGatedVenue` (order and position size, open orders, order rate, daily loss, symbol allowlist, kill switch) | done, tested with fakes |
| `ReplayDataSource` + `RunBacktest` (same `Runner`, paper venue, simulated clock) | done, tested with fakes |
| Schwab data source; Kraken, Schwab and Binance.US trading venues | **stubs** returning `kNotImplemented`; each header has a TODO listing the endpoints and auth needed. Timeframe mappings, symbol mapping and the Schwab token store are real |
| `kestrel_app run` and `backtest`, `[risk]` and `[strategy]` config, strategy registry | done; `run` has only been exercised briefly in paper mode on Binance.US |
| Indicators, more strategies, agent access | not started; see [TODO.md](TODO.md) |

The strategy, risk and backtest code has not run against a live venue, and
`run` in live mode is refused until a trading venue is implemented.

Known limitations, work in progress and planned projects are tracked in
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
