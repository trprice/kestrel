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
and `max_drawdown` suit a trend follower like `ma_crossover`.

## `[strategy]`

The parameters of the strategy named on the command line. Unknown parameters
are errors, and the symbol must be listed in `[general] symbols`.

**`ma_crossover`** (called `sma_crossover` before Nov 2026): `symbol` and
`quantity` (required), `timeframe` (`1h`),
`fast_period` (10), `slow_period` (30), `average` (`sma`, or `ema` for
exponential averages, which turn sooner; they are computed over four slow
periods of bars so they settle).

**`breakout`**: long-only breakout of a bar setup (after Fidelity's
"Identifying Chart Patterns" deck): a buy-stop just above the setup's high, a
protective sell-stop below its low, and a target at the setup's height above
the entry, checked on closes.

| Key | Meaning |
|---|---|
| `symbol`, `quantity` | required; `quantity` is base units (shares) per entry |
| `timeframe` | the bars it trades (default `1h`; Schwab streams `30m` or shorter for `run`, and backtests also take `1d`) |
| `setup` | `nr`, `inside`, `range`, `rectangle`, `pipe`, `failed_breakdown`, `double_bottom`, `triple_bottom` or `opening_range` (default `nr`); see the setups below |
| `nr_bars`, `range_bars` | 4 (NR4) and 20 |
| `touch_zone` | a fraction of the pattern's height (default 0.1). `rectangle`: a bar touches a bound when it comes within it of the bound (consecutive touching bars count once). `double_bottom`, `triple_bottom`: how far apart the lows may be, and how far below them the price may go before the pattern has failed |
| `min_touches` | `rectangle`: touches needed of each bound (default 2) |
| `decline_bars` | `pipe`: the first bar's low must be below all of these bars before it, and both bars wider than their average range (default 10) |
| `swing_bars` | `double_bottom`, `triple_bottom`: a swing point is beyond this many bars before it and no less extreme than this many after (default 2), so it is seen that many bars late. `range_bars` must be at least `2·swing_bars + 1 + (bottoms − 1)·(swing_bars + 1)` (8 for a double, 11 for a triple at the default) |
| `opening_bars` | `opening_range`: the bars of the session's opening range (default 6: the first half hour of 5-minute bars). Intraday bars only |
| `reclaim_bars` | `failed_breakdown`: the range is the `range_bars` bars before the newest `reclaim_bars + 1`, so the break below it must start within these bars (default 3) |
| `shortfall` | `rectangle`: also require the deck's shortfall: since the last touch of the top, a fall below the middle that turned before reaching the bottom (default `false`) |
| `confirm` | `none` (a buy-stop one tick above the high), `closes` (buy at market after `confirm_closes` closes above it) or `percent` (a buy-stop `confirm_percent` % above the high) (default `none`) |
| `confirm_closes`, `confirm_percent` | 2 and 0.5 |
| `entry` | `stop` (enter as `confirm` says) or `throwback` (after a close above the trigger, rest a post-only limit buy at the level broken (the setup's high; for `pipe` and `failed_breakdown`, the high of the bar the entry triggers above) for `throwback_bars` bars) (default `stop`). With `throwback`, `confirm` decides the close: with `closes`, `confirm_closes` of them in a row; with `percent`, above the raised trigger. The first must come within `arm_bars` bars of the setup, so with the default of 1 a breakout that closes above it only two bars later is missed: raise `arm_bars` to wait longer |
| `throwback_bars` | bars the throwback's limit rests before it is cancelled (default 3) |
| `stop_buffer_bps` | how far below the setup's low the protective stop sits (default 10) |
| `atr_stop`, `atr_bars` | instead, put the protective stop `atr_stop` average true ranges (Wilder's, over `atr_bars` bars, measured at the setup) below the level broken, so its distance follows the market's volatility rather than the setup's shape (defaults 0: off, and 14). The ATR is smoothed over the bars loaded, so it asks for `6 · atr_bars + 1` of warmup. A setup whose ATR stop would fall to zero or below arms nothing. `breakeven_r` measures the risk from this stop, `trail_bars` can still raise it, and the target is still the setup's height. A tight stop can sit above a `confirm = "closes"` market entry that gaps down: the venue then refuses the stop or fills it at once; without a stop in place, the next close at or below it sells at market, and a close above it places the stop again |
| `target_multiple` | the target is the entry plus this many times the setup's height (default 1; 0: none) |
| `arm_bars` | bars an untriggered entry stays (default 1); with `closes`, bars for the closes to start |
| `max_hold_bars` | exit after this many whole bars held (default 0: never) |
| `trend_sma` | enter only while the close is above this many bars' average close (default 0: off) |
| `flatten_at_close` | sell in the closing hook, before the session closes (default `false`) |
| `volume_ratio`, `volume_bars` | volume confirmation: the breakout's first close above the trigger counts only on a bar that traded at least `volume_ratio` times the average volume of the `volume_bars` bars before it (defaults 0: off, and 20). A close on less is treated as a close below. Needs `confirm = "closes"` or `entry = "throwback"`: a buy-stop fills before its bar's volume is known |
| `breakeven_r` | once a bar closes this many times the trade's risk (the entry less the first stop) above the entry, the stop is raised to the entry (rounded down to the tick), so the trade loses little more than fees, unless the price gaps through it (default 0: off). With the default `target_multiple` of 1 the target usually comes first, as the risk is about the setup's height |
| `trail_bars` | on each close, raise the stop to the lowest low of the last `trail_bars` bars less `stop_buffer_bps` (default 0: off) |

A raised stop is never lowered, and never placed at or above the close. It
replaces the resting stop (cancelled, then placed again); on daily bars, and
a bar that closes the session, at the next open. If the venue refuses the
new stop (an open that gapped below it), the position is sold at market.
Bars without volume never confirm a `volume_ratio` breakout.

The setups. Each arms a buy-stop one tick above the level shown, with the
protective stop `stop_buffer_bps` below the low shown and the target the
height shown above the fill:

| Setup | Pattern | Level | Low | Height |
|---|---|---|---|---|
| `nr` | the newest bar narrower than each of the `nr_bars - 1` before it | its high | its low | its range |
| `inside` | the newest bar inside the bar before | its high | its low | its range |
| `range` | the high and low of the last `range_bars` bars | the high | the low | the range |
| `rectangle` | that range, once the price has touched its high and its low `min_touches` times each | the high | the low | the range |
| `pipe` | a pipe bottom: after a `decline_bars` low, a wide bar closing in the lower quarter of its range, then a wide bar closing in the upper half of its own | the second bar's high | the lower low | both bars |
| `failed_breakdown` | the long side of the deck's false breakout: a close back above the low of the `range_bars` range, the bar after a close below it | that bar's high | the false low | the range |
| `double_bottom`, `triple_bottom` | in the last `range_bars` bars, the newest 2 (or 3) swing lows, each pair separated by a swing high and all within `touch_zone` of the pattern's height of each other, not yet broken out above the neckline (the highest high between the first and last low; for a triple, the higher peak) nor failed below the lows; no prior decline is required, and a formed triple also counts as a `double_bottom` of its newest two lows | the neckline | the lowest low | neckline to lowest low |
| `opening_range` | the session's first `opening_bars` bars, on the close of the last of them (once a session). The session is the data source's (with Schwab's `extended_hours`, from the pre-market open); on a market that never closes, the UTC day. The range is the first `opening_bars` bars to open at or after the open, counted rather than timed: a bar straddling the open is skipped (`1h` bars on a 09:30 open make the range from 10:00), and a missing bar (a minute with no trades) delays the setup by a bar. For the usual opening range breakout, set `arm_bars` to the session's remaining bars and `flatten_at_close = true` | the range's high | the range's low | the range |

The target and the time exit sell at market on a bar's close; only the
protective stop rests at the venue, since a spot account cannot hold the same
shares for two sell orders. A bar-only backtest fills the entry and the stop
inside the bar at their own prices (see `RunBacktest`).

On a market that closes (Schwab), daily bars close after the market, so the
entry a daily setup calls for is placed at the next open (a buy-stop the open
gaps through fills at the open), and a target or time exit is sold at the next
open. An untriggered entry is cancelled at each close and placed again at the
next open while `arm_bars` remain. Intraday bars never carry a setup overnight.

A throwback entry fills only if the price comes back to the broken level, and
then as a maker: no slippage, and the maker fee (zero on many Binance.US
pairs) instead of the taker fee. Breakouts that never look back are missed.
The stop and target sit where they would for a stop entry, so the target is
the same height above the (lower) fill.

Two departures from the deck: every setup triggers one tick above its high
(the deck buys an inside bar on an open above the inside bar's close), and
the target is measured from the entry's fill price rather than the breakout
level, so an open gapping through the trigger raises the target with it. An
entry that closes partly filled (a live venue) is held as a smaller long, with
its stop.

**`market_maker`**:

| Key | Meaning |
|---|---|
| `symbol`, `quantity` | required; `quantity` is base units per quote |
| `half_spread_bps` | required; distance of each quote from the reservation price |
| `vol_multiplier` | widen the spread with volatility: the half-spread becomes the larger of `half_spread_bps` and this times the mid's typical one-minute move in bps (default 0: off) |
| `vol_window_ms` | how far back that volatility is measured; until this much has been seen, `half_spread_bps` alone is used (default 300000) |
| `max_half_spread_bps` | cap on a widened half-spread (0, the default: none); at least `half_spread_bps` |
| `microprice_weight` | centre the quotes this far (0 to 1) from the mid towards the microprice, the mid weighted by the size at the touch (default 0: the mid) |
| `flow_pull_imbalance` | pull the quote recent trades run against: no bid while the trades of the last `flow_window_ms` were sold into it by at least this imbalance ((bought - sold) / (bought + sold), by size), no ask while they bought it up by as much; 0 to 1 (default 0: off) |
| `flow_window_ms` | how far back those trades count (default 5000) |
| `max_inventory` | required; how far the position may stray from where it started, either way |
| `skew_bps` | how far the reservation price leans when the position is `max_inventory` away (default 0); with `skew_age_ms`, up to twice this. A skew larger than the half-spread puts the quote that reduces the position past the mid, so it can sell (or buy back) at a loss against the entry before `stop_loss` fires |
| `skew_power` | with it above 1, the lean is the position's share of `max_inventory` raised to it, keeping its sign, so a small position barely leans the quotes and a large one leans them hard (2: an inventory penalty growing with the square of the position, as in Avellaneda-Stoikov); 1 to 4 (default 1: linear) |
| `skew_age_ms` | also grow the skew with how long the position has been held on its side, to double at this age and no further (default 0: off). It is held from when it last reached half a `quantity` on that side; falling under that, or crossing zero, starts the clock again |
| `requote_bps` | how far the target must move before a resting quote is replaced (default 1) |
| `requote_toward_bps` | the same when the market has moved towards the quote (the target below a resting bid, above a resting ask), which is the quote about to be filled at a stale price; 0 replaces it on a tick's move (default: `requote_bps`) |
| `max_quote_age_ms` | replace a quote older than this even if its price is right (0, the default: never) |
| `post_only` | quote with post-only orders, which the venue refuses instead of filling as a taker; recommended live, and in backtests meant to predict a live run, where without it a quote the book moved through while in flight fills as a taker (default `false`; on in `binanceus-zec-marketmaker.example.toml`) |
| `touch` | how far into the spread a computed price may go: `compute` (the default) quotes it; `join` holds it to the best price on its side, so a bid computed inside the spread joins the best bid; `improve` holds it to one tick better than that while the spread is at least three ticks wide (else as `join`). A price computed behind the touch stays there, and our own quote at the touch is not improved on |
| `levels`, `level_step_bps` | quote this many levels a side (1 to 10; default 1), each of `quantity`: the first where a single quote would be, each further one `level_step_bps` of the mid (at least a tick; default: the half-spread quoted, widened by `vol_multiplier` when that is on) behind the one before. They share `max_inventory`'s room, the touch's level first. Each level is its own order, so the order traffic grows with them (three levels sent three times the orders in the replays): allow for it in `max_orders_per_minute` and the venue's rate limit |
| `quiet_pull_ms` | pull both quotes once no quote has arrived for this long, so nothing rests at a price the strategy can no longer check (a stalled feed, a dropped connection); the next quote puts them back (default 0: never). Checked on a timer every quarter of it, from 100 ms to 1 s. The protection rules still run only on quotes. Binance.US's book stream sends nothing while the book does not change, so a quiet market is silent for seconds at a time (ZEC/USDT: over 10 s 2,695 times in four days, over 60 s 8 times): use 30000 to 60000, as 2000 to 5000 pulls thousands of times and lost money in the replays |
| `keep_at_touch` | keep a resting quote that is still at the best price on its side when its target moves away, so it keeps its place in the queue; a move towards it still replaces it (default `false`). A move away is a target better than the quote (a higher bid); the skew or a widened spread moves it the other way and still replaces the quote. It matters on pairs whose tick is a large part of the spread; with `touch = "join"` a target is never better than the touch, so it adds nothing there |

Drawdown protection is opt-in, one rule at a time, each with its own
parameters: `stop_loss`, `loss_cap`, `drawdown_cap`, `spike_standdown`,
`downtrend_filter`, `momentum_filter` and `cooldown_after_stop` (each `true` /
`false`, default `false`). Unlike the `[risk]` limits they are checked on
every quote, and the stop and cap rules close the position instead of only
blocking entries. `config/binanceus-zec-marketmaker.example.toml` lists every
rule's parameters and defaults.

With `loss_cap`, `drawdown_cap`, `spike_standdown` or `cooldown_after_stop`
on, `run` keeps their state in the `[database]` (`strategy_state`): whether
it is halted, the P&L so far and its peak, and when a stand-down or cooldown
ends, and the position with its entry price. A restart carries on from it,
so the caps count every run, not just the current one, the stop loss
watches a position the last run left, and a halted maker stays halted (and
exits such a position). `reset_state = true`
forgets it and starts afresh (set it for one run, then take it out). A saved
state that cannot be read stops the maker quoting. Without a `[database]`, a
live run is refused and a paper run warns and keeps nothing; a backtest
never keeps state. The state is per provider, mode and symbol: run one
maker per symbol on an account, or they overwrite each other's state.

**`momentum_rotation`**: cross-sectional momentum across every symbol in
`[general] symbols` (two or more, all with one quote asset, which is the
cash). Every `rebalance_bars` bars it ranks them by their return over
`lookback_bars` and holds the strongest `top_n`, each with an equal share of
`invest_fraction` of equity (cash plus its holdings at the last close). It
sells what dropped out of the top at market first, then buys what entered
once those sells have filled; what stays in the top is not resized. A period
is ranked once every symbol's bar for it has closed (or, if one never comes,
on the next period's first bar). The lookback is measured in time: a symbol
without a bar for the period, or for `lookback_bars` before it, is not ranked,
and if held it is kept (with its slot) rather than sold. With fewer than half
the symbols ranked there is no rebalance. Only what the strategy bought counts
as held (less fees taken in the coin), not the account's other balances: it
warns at startup about coins the account already holds, which it neither
counts nor sells, so a restart forgets what the last run bought. Run it on an
account (or sub-account) of its own. Leftovers under the minimum order count
as nothing held.

| Key | Meaning |
|---|---|
| `timeframe` | the bars it ranks on (default `1d`) |
| `lookback_bars` | momentum: the return over this many bars (default 28) |
| `rebalance_bars` | rebalance every this many bars, starting with the first (default 7) |
| `top_n` | how many symbols to hold (default 3); at most the number of symbols |
| `positive_only` | hold only symbols whose momentum is above zero, else cash (default `true`) |
| `invest_fraction` | share of equity invested, split over the `top_n` slots; the rest covers fees and the move to the fill (default 0.95) |

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
