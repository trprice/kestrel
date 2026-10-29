# Configuration

```sh
cp config/kestrel.example.toml config/kestrel.toml
```

Every command takes the file with `-c/--config`. `config/kestrel.example.toml`
is a commented Kraken setup with every table except the `[binanceus]` and
`[schwab]` provider tables (see [Provider tables](#provider-tables); there is
no Schwab example file). `config/binanceus-zec-marketmaker.example.toml` is a
complete Binance.US market-making setup. Unknown keys in `[risk]`,
`[recording]` and `[http]` are errors when the config is loaded, so a
misspelled limit is not silently ignored. Unknown `[strategy]` keys are errors
too, but only when a strategy is built (`run` and `backtest`), since the
valid keys depend on the strategy.

## `[general]`

| Key | Meaning |
|---|---|
| `provider` | `kraken`, `binanceus` or `schwab` |
| `mode` | `paper` (simulated venue, no credentials) or `live` |
| `symbols` | the symbols to trade and stream |
| `secrets_file` | where the secrets file is (default: `secrets.toml` next to the config, if present) |

Symbols are normalized everywhere outside provider classes: `BTC/USD`, `AAPL`.

## Credentials

**Credentials never go in the main config** (it is rejected if they do). Use
either environment variables or a git-ignored secrets file
(`config/secrets.example.toml` → `config/secrets.toml`, `chmod 600`; Kestrel
warns if other users can read it):

| Provider | Environment variables | Secrets file |
|---|---|---|
| Kraken | `KESTREL_KRAKEN_API_KEY`, `KESTREL_KRAKEN_API_SECRET` | `[kraken] api_key`, `api_secret` |
| Binance.US | `KESTREL_BINANCEUS_API_KEY`, `KESTREL_BINANCEUS_API_SECRET` | `[binanceus] api_key`, `api_secret` |
| Schwab | `KESTREL_SCHWAB_APP_KEY`, `KESTREL_SCHWAB_APP_SECRET` | `[schwab] app_key`, `app_secret` |
| Database | `KESTREL_DATABASE_PASSWORD` | `[database] password` |

Environment variables win over the file, and a key and its secret must both be
given. `KESTREL_SECRETS_FILE` or `[general] secrets_file` points at a different
file. Kraken market data needs no credentials; paper mode needs none at all.
Live mode without credentials fails at startup.

A Binance.US key needs read permission for `account` and trading permission
for `order` and `run`; never enable withdrawals. The same key serves the
user-data stream when it is added.

**Schwab tokens** are persisted to `[schwab] token_file` (default
`schwab_tokens.json`, mode 0600). The 7-day refresh token can only be replaced
by a human re-authorizing in a browser, so Kestrel logs a warning in the last
24 hours and a `SCHWAB RE-AUTHORIZATION REQUIRED` error, with the authorize
URL, once it has expired.

## Provider tables

`[kraken]` and `[binanceus]` take `rest_url` and `ws_url` (defaults: the
production endpoints). `[schwab]` takes `redirect_uri` and `token_file`.

## `[paper]`

Used in paper mode, by `run` and by `backtest`.

| Key | Meaning |
|---|---|
| `initial_balances` | starting balances by asset, e.g. `{ USDT = "50", ZEC = "0.041" }` |
| `maker_fee_bps`, `taker_fee_bps` | fees in basis points |
| `slippage_bps` | how far through the touch a market order fills |
| `default_quote_asset` | quote asset for symbols that have none (equities) |
| `cap_limit_fills_at_quote_size` | limit fills capped at the size shown at the touch |
| `limit_fill_model` | `cross` (optimistic: the touch reaches the limit) or `conservative` (the touch goes through it, a trade prints through it, or trades at it clear the queue ahead); use `conservative` for anything that rests limit orders |

The paper venue applies the same symbol rules as a live one: tick, lot,
minimum size and minimum order value.

## `[risk]`

Optional in paper mode, required for a live `run`. Every limit is optional;
omit one to leave it unchecked. Amounts are in the quote asset.

| Key | Limit |
|---|---|
| `max_order_notional` | one order's value |
| `max_position_notional` | a position's value |
| `max_open_orders` | orders open at once |
| `max_orders_per_minute` | order rate |
| `max_daily_loss` | realized loss, net of fees, since the start of the UTC day (or of the run) |
| `allowed_symbols` | the only symbols that may be traded |
| `max_price_age_ms` | refuse orders valued at a reference price older than this |
| `max_orders_per_day` | then only position-reducing orders |
| `max_consecutive_losses` | losing sells in a row; resets on a win or a new UTC day |
| `max_drawdown` | fall from peak equity (realized + unrealized), checked at order time; does not reset at midnight |

`[risk.<strategy>]` lays keys over `[risk]` for that strategy only (a section
naming an unknown strategy is an error). When a loss, drawdown or order-count
limit is hit, only orders that reduce a position are allowed; the gate does
not close the position itself. `max_orders_per_day`, `max_consecutive_losses`
and `max_drawdown` suit a trend follower like `sma_crossover`.

## `[strategy]`

The parameters of the strategy named on the command line. Unknown parameters
are errors, and the symbol must be listed in `[general] symbols`.

**`sma_crossover`**: `symbol` and `quantity` (required), `timeframe` (`1h`),
`fast_period` (10), `slow_period` (30).

**`market_maker`**:

| Key | Meaning |
|---|---|
| `symbol`, `quantity` | required; `quantity` is base units per quote |
| `half_spread_bps` | required; distance of each quote from the reservation price |
| `max_inventory` | required; how far the position may stray from where it started, either way |
| `skew_bps` | how far the reservation price leans when the position is `max_inventory` away (default 0) |
| `requote_bps` | how far the target must move before a resting quote is replaced (default 1) |
| `max_quote_age_ms` | replace a quote older than this even if its price is right (0, the default: never) |

Drawdown protection is opt-in, one rule at a time, each with its own
parameters: `stop_loss`, `loss_cap`, `drawdown_cap`, `spike_standdown`,
`downtrend_filter`, `momentum_filter` and `cooldown_after_stop` (each `true` /
`false`, default `false`). Unlike the `[risk]` limits they are checked on
every quote, and the stop and cap rules close the position instead of only
blocking entries. `config/binanceus-zec-marketmaker.example.toml` lists every
rule's parameters and defaults.

## `[recording]`

Whether `run` stores what it does in the `[database]`; see
[database.md](database.md#recording-a-run).

| Key | Meaning |
|---|---|
| `enabled` | default `false`; needs `[database]` |
| `pnl_interval_ms` | P&L snapshot period (default 60000; 0: only the last one) |
| `latency` | also store venue response times (default `true`) |
| `latency_interval_ms` | one row per endpoint per window (default 60000) |

## `[http]`

| Key | Meaning |
|---|---|
| `slow_request_ms` | log a warning for any HTTP call at least this slow (default 2000) |

## `[database]`

`host` (omit for the local socket), `port`, `dbname`, `user`, `sslmode`. The
password comes from `KESTREL_DATABASE_PASSWORD`, `[database] password` in the
secrets file, or libpq's `PGPASSWORD` / `~/.pgpass`. Use `host = "localhost"`
when the role has a password, since the local socket normally uses peer
authentication. See [database.md](database.md) for setting the database up.
