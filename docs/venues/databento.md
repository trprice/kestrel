# Databento

`provider = "databento"` imports historical US equity market data from
Databento into Kestrel's PostgreSQL / TimescaleDB tables (`venue =
"databento"`) and replays it in `backtest` without needing Schwab credentials
or overwriting Schwab's stored rows.

Source: `include/kestrel/data/databento_client.hpp`,
`src/data/databento_client.cpp`,
`include/kestrel/data/databento_data_source.hpp`,
`src/data/databento_data_source.cpp`,
`include/kestrel/db/databento_import_store.hpp`,
`src/db/databento_import_store.cpp`, `app/commands/databento.cpp`.

## Why a separate provider

- **Usage-billed history:** Databento charges by data volume, so Kestrel never
  fetches from Databento automatically on a `backtest` cache miss. Data is
  imported explicitly with `kestrel_app databento`, which runs a pre-flight
  cost check (`metadata.get_cost`) before downloading anything.
- **Single-publisher provenance and isolated sequence numbers:** rows are
  stored under `venue = "databento"`, keeping composite or direct-feed equity
  datasets (`EQUS.MINI`, `XNAS.ITCH`, `XNYS.PILLAR`) separate from Schwab's
  feed and live quote recorder. Multi-publisher bundles such as `DBEQ.BASIC`
  (which emit separate per-exchange BBOs and bars for the same ticker) are
  rejected by the parser.
- **Shared calendar and symbol format:** `DatabentoDataSource` uses Kestrel's
  `UsEquityCalendar` (`09:30` to `16:00` New York time, `13:00` on early
  closes, and `07:00` to `20:00` when `[databento] extended_hours = true`;
  Databento's data starts at `04:00`, but `04:00` to `07:00` is not imported,
  so that a backtest trades only the hours Schwab's extended session does),
  standard Reg NMS >= $1 stock increments ($0.01 tick, 1 share lot; sub-$1
  stocks that quote in $0.0001 increments are not distinguished without a live
  quote lookup), and Kestrel's normalized tickers (`SPY`, `BRK.B`, matching
  Databento's `raw_symbol`). Switching a strategy between a Databento backtest
  and Schwab paper or live trading only changes `provider`.

## Importing data

### Over HTTP (`hist.databento.com/v0`)

Set `KESTREL_DATABENTO_API_KEY` in the environment or `[databento] api_key` in
`secrets.toml`. Every HTTP pull requires either `--dry-run` (prints the
estimated USD cost from `/v0/metadata.get_cost` and exits without downloading)
or `--max-cost <USD> --store` (refuses with exit code `2` if the estimated cost
exceeds the cap; `--store` is mandatory on paid HTTP pulls so downloaded data
is never discarded):

```sh
# 1. Check the cost first (spends $0; excludes slices recorded in [database])
./build/kestrel_app -c config/databento.toml databento \
    --schema bbo-1s --from 2025-01-01 --to 2026-01-01 --dry-run

# 2. Import 1-minute bars and roll them up into 5m, 15m and 30m bars
./build/kestrel_app -c config/databento.toml databento \
    --schema ohlcv-1m --rollup 5m,15m,30m \
    --from 2025-01-01 --to 2026-01-01 --max-cost 5.00 --store

# 3. Import 1-second sampled top-of-book quotes
./build/kestrel_app -c config/databento.toml databento \
    --schema bbo-1s --from 2025-01-01 --to 2026-01-01 --max-cost 5.00 --store
```

Intraday HTTP imports request only open trading sessions (`[09:30, 16:00)` ET,
or `[07:00, 20:00)` ET with `extended_hours = true`), avoiding charges for
closed pre-market and after-hours records. A response is held in memory
whole before it is parsed, so tick schemas split each session into shorter
requests: an hour for `tbbo` and `trades`, a quarter of an hour for `mbp-1`
(every top-of-book change; an hour of SPY can run to hundreds of MB).
`ohlcv-1d` batches up to 365 days per request. A request for data
(`timeseries.get_range`) is sent again after a rate limit or a server error,
which deliver nothing, but not after a timeout or a lost connection, which may
have delivered and billed the data: the import stops, and running it again
resumes from the ledger. When
`--to` lies within the last 24 hours, Kestrel queries
`metadata.get_dataset_range` first and clamps `--to` to the dataset's available
end.

Completed HTTP slices are recorded per `(venue, dataset, symbol, target)` in
the `databento_imports` ledger table only after every row in the request has
been saved. Thinly traded or quiet sessions are therefore not re-downloaded on
resume, while any slice interrupted mid-save is fetched again (`--force`
bypasses the ledger). If you manually delete rows from `bars`, `quotes`, or
`market_trades`, delete the matching `databento_imports` rows too or re-run
with `--force`. A `--rollup` added later, for sessions whose `ohlcv-1m` bars
the ledger already records, is made from the stored 1m bars, without a
download or a cost (`--dry-run` says how many sessions would be). A rollup
is recorded only over its whole buckets, so a bucket cut by one import's
`--from` or `--to` is made by a later import that covers the rest. Local
`--file` imports do not write to `databento_imports`, so
a later HTTP pull covering the same dates will download those sessions unless
excluded by `--from` / `--to`. Over ranges of more than 10 session spans,
`metadata.get_cost` coalesces consecutive uncached sessions into a single cost
query (`up to $...`, a safe upper bound that includes closed hours between
sessions).

### From a local CSV batch file (`--file`)

Large multi-year pulls ordered as batch jobs on Databento's web portal arrive
as daily `.csv.zst` files. Stream them into Kestrel without an API key and in
constant memory (`ParseDatabentoCsv` flushes every 1,000 rows and skips
repeated CSV headers across concatenated files):

```sh
zstdcat EQUS-MINI-bbo-1s-*.csv.zst | \
    ./build/kestrel_app -c config/databento.toml databento \
        --schema bbo-1s --file - --store
```

Both raw fixed-point CSVs (`pretty_px=false`, `pretty_ts=false`) and portal
human-readable CSVs (`pretty_px=true`, `pretty_ts=true`) are accepted. Rows
for symbols not listed in `[general] symbols` are skipped.

## Schemas and mapping

| Schema | Table | Notes |
|---|---|---|
| `ohlcv-1m`, `ohlcv-1h`, `ohlcv-1d` | `bars` | `ts_event` is `open_time`. `--rollup` (e.g. `5m,15m,30m`) is supported on `ohlcv-1m`, anchored at each session's open (`09:30` ET, or `07:00` ET with `extended_hours = true`; note hourly rollups like `1h`/`2h`/`4h` shift from `:30` to `:00` if `extended_hours` changes, so purge `bars` before switching `extended_hours` on hourly rollups). Rollup buckets cut off by `--from` or `--to` are dropped rather than saved as partial bars. Native `ohlcv-1h` bars start on UTC clock hours (`09:00` ET holds `09:30-10:00` RTH plus `09:00-09:30` pre-market), and `ohlcv-1d` rolls at `00:00` UTC; prefer `ohlcv-1m` (with `--rollup`) for RTH intraday bars or Schwab for official daily bars. Bars of one timeframe on two grids (native `ohlcv-1h` on the hour and a `1h` rollup from `09:30`, or rollups made with and without `extended_hours`) would overlap in a backtest, so an import that would store them beside the other kind is refused, with the SQL to delete the stored ones |
| `bbo-1s`, `bbo-1m` | `quotes` | Sampled top-of-book bid/ask and sizes (in shares), plus the last trade when present. `UNDEF_PRICE` leaves that side unset |
| `tbbo` | `quotes` | Every trade paired with the prevailing pre-trade BBO (`bid`, `ask`, `last`, `last_size`, and `last_taker_side`) |
| `mbp-1` | `quotes` | Every top-of-book update; requires `action`, updates `last` on trades (`action = 'T'`), and carries it forward across book updates on the same New York trading date |
| `trades` | `market_trades` | Tick-by-tick trades for `backtest --quotes-from-trades`; neutral aggressor prints (`side = 'N'`) infer side by the tick rule |

Quote `seq` and trade `trade_id` values are the record's nanosecond
timestamp (`ts_recv`, or `ts_event` without it), moved up past the IDs given
before it in the same millisecond (a burst in one nanosecond, or a timestamp
stepping back). A request never splits a millisecond, so a record gets the
same ID whichever slice, file or session filter it came in.
Re-importing the same raw feed is skipped via `ON CONFLICT DO NOTHING`, and
backfilling an earlier day or year after a later one keeps `QuoteStore::Load`
(`ORDER BY seq`) in chronological order.
