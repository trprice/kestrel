# Configuration

```sh
cp config/kestrel.example.toml config/kestrel.toml
```

Every command takes the file with `-c/--config`. `config/kestrel.example.toml`
is a commented Kraken setup with every table except the `[binanceus]` and
`[schwab]` provider tables (see [Provider tables](#provider-tables)).
`config/schwab.example.toml` is a commented setup for US stocks on Schwab in
paper mode, with the authorization steps, and
`config/binanceus-zec-marketmaker.example.toml` a complete Binance.US
market-making setup. Unknown keys, in any table, and
unknown tables are errors when the config is loaded, so a misspelled setting
is never silently replaced by its default. One exception: a config may
carry all three provider tables, and only its own provider's is read and
checked. Unknown `[strategy]` keys are errors too,
but only when a strategy is built (`run` and `backtest`), since the valid
keys depend on the strategy.

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
file. Kraken and Binance.US market data need no credentials, so paper mode on
them needs none at all. Schwab's market data is behind OAuth, so Schwab needs
its app key and secret, and tokens, in paper mode too. Live mode without
credentials fails at startup.

A Binance.US key needs read permission for `account` and trading permission
for `order` and `run`; never enable withdrawals. The same key serves the
user-data stream when it is added.

**Schwab tokens** come from `kestrel_app authorize` (see
[running.md](running.md#authorizing-schwab)) and are kept in `[schwab]
token_file` (default `schwab_tokens.json` next to the config, mode 0600; keep
it out of version control). The access token lasts 30 minutes and is renewed
automatically, and each renewal is saved to the file. The refresh token lasts
7 days from the authorization, and renewing does not extend it; only running
`authorize` again replaces it. Kestrel logs a warning in its last 24 hours,
and a `SCHWAB RE-AUTHORIZATION REQUIRED` error, saying to run `authorize`,
once it has expired or Schwab refuses it.

## Provider tables

`[kraken]` and `[binanceus]` take `rest_url` and `ws_url` (defaults: the
production endpoints). `[schwab]` takes `redirect_uri` (default
`https://127.0.0.1:8182`), which must match the app's callback URL in the
Schwab developer portal exactly, `token_file`, and `extended_hours` (default
`false`): intraday bars from outside the regular session too (pre-market,
after hours and the overnight session), from `GetBars` and from the bar
stream alike, and orders in Schwab's extended hours (07:00 to 09:30 and the
close to 20:00 New York time; not overnight). Without it, orders are refused
outside the regular session. The trading days, holidays and early closes come
from Kestrel's US equity calendar, checked against Schwab's `/markets` for the
coming days.

Schwab allows one streamer connection per user, so when another one logs in
(another `kestrel_app`, or thinkorswim streaming quotes) Kestrel's quote and
bar streams are disconnected. `[schwab] takeover_reconnect_ms` says what then
happens: 0, the default, stops the streams for good (the other one is presumed
to be wanted), and their subscriptions go quiet; more than 0 waits that long
and reconnects, which in turn disconnects the other one.

## `[paper]`

Used in paper mode, by `run` and by `backtest`.

| Key | Meaning |
|---|---|
| `initial_balances` | starting balances by asset, e.g. `{ USDT = "50", ZEC = "0.041" }` |
| `maker_fee_bps`, `taker_fee_bps` | fees in basis points |
| `slippage_bps` | how far through the touch a market order fills |
| `default_quote_asset` | quote asset for symbols that have none (equities) |
| `cap_limit_fills_at_quote_size` | limit fills capped at the size shown at the touch |
| `order_latency_ms` | how long an order or a cancel takes to reach the book (default 0); until then a new order cannot fill and an order being cancelled still can. Post-only and fill-or-kill are checked when it arrives. For a backtest, use the whole REST round trip (about 130 ms to Binance.US from the US West Coast), since the replayed quotes already carry the market data's delay |
| `report_latency_ms` | how long a fill or an order's new state takes to reach the strategy (default 0). Live Binance.US is polled every second, so about 600 ms on average |
| `allow_short` | sell short (default `false`): a sell of more than the free base asset sells what is free and shorts the rest. The base balance and the position go below zero, and the sale's proceeds and more stay locked in the quote asset as collateral. A buy covering the short is paid from that collateral and reserves no cash of its own (a second order covering the same short is an ordinary buy). No live venue here shorts: on Binance.US such a sale is refused for want of the asset |
| `short_margin` | the collateral, as a multiple of what the short sold for (default 1.5, Reg T's 150%: the proceeds and half as much again; at least 1). Placing a short sale reserves the part its proceeds will not cover; if that is no longer free at its fill, the order is cancelled. There is no borrow fee and no margin call: a short that rises past its collateral leaves the quote balance short of what is locked, or below zero once covered |
| `limit_fill_model` | `cross` (optimistic: the touch reaches the limit) or `conservative` (the touch goes through it, a trade prints through it, or trades at it clear the queue ahead, counting only takers from the other side when the quote says which side took); use `conservative` for anything that rests limit orders |

Set both latencies for any backtest meant to predict a live run. Without
them a cancel takes effect at once, so a quote is never filled after the
strategy decided to move it, which flatters a market maker more than any of
its parameters.

The paper venue applies the same symbol rules as a live one: tick, lot,
minimum size and minimum order value.

## `[risk]`

Optional in paper mode, required for a live `run`. Every limit is optional;
omit one to leave it unchecked. Amounts are in the quote asset.

| Key | Limit |
|---|---|
| `max_order_notional` | one order's value |
| `max_position_notional` | a position's value: what is held, plus open orders that add to it, plus the new order |
| `max_total_notional` | every symbol's position, counted that way, together; each at its reference price. Refused if a symbol held or on order has no price |
| `max_symbol_share` | a symbol's position as a share of the account's equity (above 0, at most 1). Equity is every balance on the venue: the quote asset at 1, other assets at their symbol's price; an asset with no price is left out (logged once). Refused if balances cannot be read or equity is not above zero |
| `max_open_orders` | orders open at once |
| `max_orders_per_minute` | order rate |
| `max_daily_loss` | realized loss, net of fees, since the start of the UTC day (or of the run) |
| `allowed_symbols` | the only symbols that may be traded |
| `max_price_age_ms` | refuse orders valued at a reference price older than this |
| `max_orders_per_day` | then only position-reducing orders |
| `max_consecutive_losses` | losing sells in a row; resets on a win or a new UTC day |
| `max_drawdown` | fall from peak equity (realized + unrealized), measured at each order and each price (at most once a second); then only position-reducing orders. Does not reset at midnight |
| `max_drawdown_close` | the same fall, at which the gate halts, cancels every open order and closes every position at market; trading stays halted until the next run, and what is still held is closed again each minute while the fall stands. Set it above `max_drawdown`, or alone |
| `max_loss_per_position` | one position's unrealized loss from its average cost, measured the same way; the gate cancels that symbol's orders, closes the position at market, and refuses new positions in the symbol for the rest of the UTC day. A backstop for the strategy's own stop |
| `max_quote_age_ms` | refuse new positions in a symbol whose quotes have stopped this long while its market is open, until they return (checked every 10 s; strategies that take quotes) |

`[risk.<strategy>]` lays keys over `[risk]` for that strategy only (a section
naming an unknown strategy is an error). When a loss, drawdown, order-count
or stale-quote limit is hit, only orders that reduce a position are allowed.
Only `max_drawdown_close` and `max_loss_per_position` close positions: with
market orders the gate sends itself, outside the other limits, rounded down to
the lot and to the coin's balance. The strategy sees their fills as it sees
any other (`breakout` goes flat when its holding is sold, though it may first
log that its stop was cancelled and could not be placed again), and a symbol
is closed at most once a minute. The gate does not know market hours, so a close
of a stock is sent even when the market is shut. `max_orders_per_day`,
`max_consecutive_losses` and `max_drawdown` suit a trend follower like
`ma_crossover`.

## `[strategy]`

The parameters of the strategy named on the command line. Unknown parameters
are errors, and the symbol must be listed in `[general] symbols`.

Each strategy's document lists its parameters:

| Strategy | Parameters |
|---|---|
| `breakout` | [strategies/breakout.md](strategies/breakout.md#parameters) |
| `market_maker` | [strategies/market-maker.md](strategies/market-maker.md#parameters) |
| `momentum_rotation` | [strategies/momentum-rotation.md](strategies/momentum-rotation.md#parameters) |
| `ma_crossover` | [strategies/ma-crossover.md](strategies/ma-crossover.md#parameters) |

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

`statement_timeout_ms` (default 300000, 5 minutes; 0 for no limit) cancels a
statement that runs longer. The connection also gives up connecting after
10 s and notices a dead server or network with TCP keepalives (about a
minute), so a database that stops answering fails the call instead of
hanging it.
