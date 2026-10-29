# Database

Kestrel stores market data, recorded runs and the account's trade history in
PostgreSQL with the TimescaleDB extension. Prices and quantities are exact
`NUMERIC`. The database is optional: only `init-db`, `--store`, `backtest`
over stored data and `[recording]` need it.

## Setting it up

Needs `libpq-dev` to build and a server with
`shared_preload_libraries = 'timescaledb'`.

```sh
scripts/setup_database.sh --config config/kestrel.toml
```

The script (idempotent; `--help` lists options) creates the `kestrel` role and
database, enables the timescaledb extension, and creates the tables through
`kestrel_app init-db`. It runs `psql` as the `postgres` OS user via sudo, or
as whatever `--admin-cmd` / `KESTREL_ADMIN_PSQL` says for a remote server. A
password is generated for a new role and printed once (or set
`KESTREL_DATABASE_PASSWORD` beforehand).

Then add a `[database]` section to the config; see
[configuration.md](configuration.md#database). Schema migrations are applied
automatically on startup (`src/db/migrations.cpp`, append-only).

## Tables

| Table / view | Written by | Contents |
|---|---|---|
| `bars` | `bars --store`, `stream-bars --store` | hypertable keyed by venue, symbol, timeframe, open_time |
| `quotes` | `stream --store` | hypertable chunked per day: one row per delivered quote |
| `runs`, `fills`, `pnl_snapshots`, `pnl_totals`, `venue_latency` | `run` with `[recording]` | one run's record; see below |
| `account_fills` | `account --store` | the account's whole trade history; see below |
| `market_trades` | `trades --store` | hypertable chunked per week: every public trade on the venue, keyed by venue, symbol, trade ID; see below |
| `schema_migrations` | every command that opens the database | which migrations have been applied |

## Market data

```sh
./build/kestrel_app -c config/kestrel.toml bars 1h 500 --store          # pull and store
./build/kestrel_app -c config/kestrel.toml stream-bars 1m 3600 --store  # stream closed bars into the DB
./build/kestrel_app -c config/kestrel.toml stream 3600 --store          # stream quotes into the DB
```

Saving bars is an upsert, so re-pulling a range is harmless. `stream-bars`
(`DataSource::StreamBars`, implemented for Binance.US klines) emits each bar
once, when it closes. Bars closed while the socket was down are not replayed;
backfill with `bars --store`.

`stream <seconds> --store` writes every streamed quote to the `quotes`
hypertable, with the merged bid/ask/last. `QuoteWriter` buffers on the
WebSocket thread and a background thread inserts in batches every 500 ms; a
failed batch is retried, with at most 100,000 quotes buffered. Each quote gets
a `seq`, counted per venue and symbol by `QuoteWriter` and continued from the
database after a restart, so arrival order survives quotes that share a
millisecond; reads are ordered by it. `bookTicker` updates are frequent:
expect many rows per symbol per minute. Quotes are the one table that grows
fast (tens of MB a day per symbol); see the retention item in
[TODO.md](../TODO.md).

### Trade history, for backtests without recorded quotes

Venues keep no history of their quotes, but Binance.US keeps every public
trade, back to each symbol's listing (ZEC/USDT: 2022-01-13), each with the
side that took liquidity. `trades` fetches them for the `[general] symbols`
a day at a time; no credentials are needed:

```sh
./build/kestrel_app -c config/kestrel.toml trades --from 2026-09-29 --to 2026-10-30 --store
./build/kestrel_app -c config/kestrel.toml backtest market_maker \
    --from 2026-10-01 --to 2026-10-08 --quotes-from-trades
```

Each request covers at most an hour or 1000 trades, so a day of ZEC/USDT is
about 20-40 requests, held to the data source's 10 requests a second. With
`--store`, each day starts from its newest stored trade and a trade already
stored is skipped, so an interrupted backfill can simply be run again.

`backtest --quotes-from-trades` replays quotes rebuilt from the stored trades
(`backtest::QuotesFromTrades`) instead of recorded ones: one quote per trade,
with the trade as the last trade, and a seller taking liquidity setting the
bid (a buyer, the ask). It is an approximation:

- the book changes between trades are not seen, so quotes are stale until
  the next trade and a strategy requotes less often than it would live;
- the size at the touch is unknown, so under `limit_fill_model =
  "conservative"` a resting order at the touch fills only when a trade goes
  through its price, never at it (pessimistic);
- a side is missing until it has traded once, and a trade through the other
  side moves that side to one tick away.

Compare a trade-based backtest with a quote-based one over a range that has
both before relying on it.

## Recording a run

With `[recording] enabled = true` (and a `[database]`), `run` writes what it
does to the database as it goes, so a long run can be watched from `psql` and
studied afterwards:

| Table / view | Contents |
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

P&L is the same average-cost figure the risk gate uses, realized net of fees,
starting from the positions the venue reported at startup; the mark is the
last quote mid or bar close. A failed write is logged and retried, never fatal
to the run; if the database cannot be reached at startup, `run` refuses to
start.

### Response times

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

This times the HTTP round trip only. Order-side figures (acknowledgement,
cancel, time until a fill is visible) are not measured yet; see
[TODO.md](../TODO.md).

## The account's trade history

`kestrel_app account --store` (live mode) copies the account's fills into
`account_fills`, keyed by venue and fill ID: every fill on the account,
whatever placed the order, where `fills` holds only what a run saw. The first
sync of a symbol reaches back `--days` (default 7, at most 90); later ones
carry on from its newest stored fill, so running it again only tops up, and a
fill stored twice is kept once. The listing and the sync share one fetch.

On Binance.US the fills are fetched per configured symbol, in 24 hour windows
of weight 10 each, so a first sync costs 10 × days × symbols: 900 for 90 days
of one symbol, 2700 for three. Kestrel keeps itself under 2400 weight a minute
by waiting (Binance.US allows 6000), so that is about a minute per 2400
weight, less the first 100, which go at once: a 90 day sync of one symbol
goes through in well under a minute. Binance's trade history carries
no client order ID, so `client_order_id` is empty in this table.

```sql
SELECT symbol, side, sum(quantity), round(sum(quantity * price) / sum(quantity), 2)
FROM account_fills WHERE venue = 'binanceus' GROUP BY 1, 2;
```

## Upgrading an existing database

After updating Kestrel, bring an existing database up to the new schema with:

```sh
build/kestrel_app --config config/kestrel.toml init-db
```

Any command that opens the database also migrates first, but running `init-db`
yourself lets you choose when it happens. Migrations are tracked in the
`schema_migrations` table: only those newer than the highest recorded version
are applied, each one is logged (`database: applying migration N (...)`), and
a database that is already current is left untouched. A lock keeps two
processes from migrating at once.

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

## Testing against a database

See [testing.md](testing.md#database-integration-tests).
