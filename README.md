# Kestrel

A C++23 library for gathering stock and cryptocurrency prices and trading on
them. Providers: **Schwab**, **Kraken**, **Binance.US**.

Market data and order execution are separate interfaces (`data::DataSource`,
`trading::TradingVenue`). Only the execution side changes between paper and
live trading; a factory picks the right one from the config.

## Status (first pass)

| Piece | State |
|---|---|
| Build system, `Decimal`, shared types, `Error`/`std::expected` | done |
| `DataSource` / `TradingVenue` interfaces, `Configuration`, factory | done |
| `PaperTradingVenue` (works with any `DataSource`) | done |
| `KrakenDataSource` (REST bars + symbol info, WebSocket quotes) | done, tested live |
| Schwab data source, all three live venues | **stubs** returning `kNotImplemented`; each header has a TODO listing the endpoints and auth needed. Timeframe mappings, symbol mapping and the Schwab token store are real. The Binance.US data source (klines, symbol info, WebSocket quotes) is implemented |
| Database: PostgreSQL + TimescaleDB `bars` and `quotes` hypertables, `BarStore`, `QuoteStore`/`QuoteWriter`, Binance.US closed-bar stream | done, tested live |
| Indicators, strategies, agent access, risk gate | out of scope |

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
has not been exercised here (the system cpr was found).

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
./build/kestrel_app --config config/kestrel.toml --bars 1h 24 --stream 30
```

`kestrel_app` only does what it is asked:

- `--config` alone: the last 10 completed `1h` bars per symbol (the default for
  `--bars`), preceded by symbol info and followed by the venue's balances.
- `--bars <timeframe> <count>`: the same, with your timeframe and count.
- `--stream <seconds>`: live quotes for that many seconds, and nothing else.
- `--bars` and `--stream` together: bars first, then the quote stream.

The example above prints the last 24 hourly bars, then 30 seconds of quotes.

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
`kestrel_app --init-db`. It runs `psql` as the `postgres` OS user via sudo, or
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
build/kestrel_app --config config/kestrel.toml --init-db
```

`--init-db` cannot be combined with other actions. Any run with `--store` also
migrates first, but running `--init-db` yourself lets you choose when it
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
4. Run `--init-db`, then confirm with
   `SELECT * FROM schema_migrations ORDER BY version;`.

If a migration fails, the error names its version. Fix the cause and re-run; if
you are unsure whether it applied partway, compare the schema against the
migration's SQL, or restore the backup.

```sh
./build/kestrel_app -c config/kestrel.toml --bars 1h 500 --store          # pull and store
./build/kestrel_app -c config/kestrel.toml --stream-bars 1m 3600 --store  # stream closed bars into the DB
```

`--stream <seconds> --store` writes every streamed quote to the `quotes`
hypertable (chunked per day, one row per delivered quote with the merged
bid/ask/last). `QuoteWriter` buffers on the WebSocket thread and a background
thread inserts in batches every 500 ms; a failed batch is retried, with at most
100,000 quotes buffered. Each quote gets a `seq`, counted per venue and symbol by `QuoteWriter`
and continued from the database after a restart, so arrival order survives
quotes that share a millisecond; reads are ordered by it. `bookTicker` updates are frequent: expect many rows per symbol per minute.

`--stream-bars` (`DataSource::StreamBars`, implemented for Binance.US klines)
emits each bar once, when it closes. Bars closed while the socket was down are
not replayed; backfill with `--bars --store` (saves are upserts, so overlap is
harmless).

### Database integration tests

The database tests run only when `KESTREL_TEST_DBNAME` is set (plus optional
`KESTREL_TEST_DB{HOST,PORT,USER,PASSWORD}`); otherwise they are skipped. They
need an empty database with the timescaledb extension and a login role for it.
The tables are created by the tests themselves (they run the migrations), and
they only touch rows with their own test venue. Use a separate database from
your real one:

```bash
export KESTREL_DATABASE_PASSWORD=testpass   # optional; else one is generated and printed
scripts/setup_database.sh --db-name kestrel_test --db-user kestrel_test

export KESTREL_TEST_DBNAME=kestrel_test
export KESTREL_TEST_DBHOST=localhost
export KESTREL_TEST_DBUSER=kestrel_test
export KESTREL_TEST_DBPASSWORD=testpass
./build/kestrel_tests --gtest_filter='QuoteStore*:BarStore*'
```

Omit `--config` so the script doesn't create the tables itself. See the
script's `--help` for `--admin-cmd` if `sudo -u postgres psql` doesn't work on
your machine.

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
  provider's docs and should be re-verified before the stubs are completed.

## Layout

```
include/kestrel/{types,util,net,data,trading,config}/   public headers
src/                                                     implementation
app/main.cpp                                             kestrel_app
tests/                                                   kestrel_tests
```
