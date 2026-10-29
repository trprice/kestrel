# Running Kestrel

```sh
./build/kestrel_app --config config/kestrel.toml bars 1h 24
```

`kestrel_app` takes the global options `-c/--config` (required) and
`-v/--verbose`, then one subcommand, which does only what its name says.
`kestrel_app --help` and `kestrel_app <subcommand> --help` list the arguments.
The global options may also come after the subcommand's arguments. The example
above prints the last 24 hourly bars. See [configuration.md](configuration.md)
for the config file.

| Subcommand | Does |
|---|---|
| `bars <timeframe> <count> [--store]` | the last `count` completed bars per symbol, preceded by symbol info and followed by the venue's balances |
| `stream <seconds> [--store]` | live quotes for that many seconds |
| `stream-bars <timeframe> <seconds> [--store]` | completed bars as they close, for that many seconds (providers that offer it) |
| `init-db` | creates or updates the database schema |
| `backtest <strategy> --from <date> --to <date>` | replays a strategy over stored bars and quotes |
| `run <strategy>` | trades a strategy on live data, through the paper or live venue |
| `account [--days N] [--order ID] [--store]` | the venue's balances, positions, open orders and the fills of the last `N` days (default 7), and one order by ID; read-only on the venue |
| `order <buy\|sell> <symbol> <quantity> [--limit P \| --stop P] [--tif gtc\|ioc\|fok\|day] (--test \| --confirm)` | one order on the live venue; Binance.US refuses `day`, and a stop order must be `gtc` (the default) |
| `cancel <order_id> \| --all --confirm` | cancels one order, or every open order on the configured symbols, on the live venue |

`--store` writes what a command fetches to the `[database]`; see
[database.md](database.md).

## Backtesting and running strategies

```sh
./build/kestrel_app -c config/kestrel.toml backtest sma_crossover --from 2026-09-01 --to 2026-10-01
./build/kestrel_app -c config/kestrel.toml run sma_crossover
```

- `backtest <strategy> --from <date> --to <date>` fetches the strategy's
  bars (plus its warmup) from the database, falling back to the provider for
  missing bars, replays them and any stored quotes through the same `Runner`
  and a paper venue using `[paper]`, and prints the fills, positions and
  balances. `--to` is exclusive; dates are `YYYY-MM-DD` or ISO 8601 UTC.
  `backtest` records no run, fills or P&L. With a `[database]`, it does save
  the bars it had to fetch from the provider, so the next backtest finds them.
- `run <strategy>` trades on live data through the paper or live venue until
  Ctrl-C or SIGTERM (a second signal exits immediately), and logs each HTTP
  endpoint's response times when it stops. If `[risk]` sets any limit, orders
  go through the risk gate, and the daily-loss count starts at startup.
  Without `[risk]` it warns and trades unchecked in paper mode. Live mode is
  refused without `[risk]`, and on a provider whose venue is still a stub
  (Kraken and Schwab); Binance.US runs live. Read
  [One trader per account](#one-trader-per-account) before a live run.
- **`run` cancels every open order first.** Before the strategy starts,
  `run` cancels every order open on the `[general] symbols`, whoever placed
  it, and logs the IDs it cancelled. If the cancel fails, `run` exits without
  trading. See [One trader per account](#one-trader-per-account).
- With `[recording] enabled = true`, `run` stores its fills, P&L snapshots and
  response times as it goes; see [database.md](database.md#recording-a-run).

The strategies' parameters, the risk limits and the market maker's protection
rules are described in [configuration.md](configuration.md). To write your own
strategy, see [writing-strategies.md](writing-strategies.md).

## Live trading on Binance.US

These commands talk to the live account (`mode = "live"`, with the Binance.US
credentials set; see [configuration.md](configuration.md#credentials)). None
of them is needed for paper trading.

### One trader per account

**Kestrel presumes it is in control of the account's orders while `run` is
running.** It cancels whatever is open on its symbols when it starts, and the
strategy, the order tracking and the risk gate only know about the orders the
strategy placed. Any other order on the account can interrupt the strategy:

- it holds funds the strategy expects to use, so the strategy's own orders can
  be refused for an insufficient balance;
- its fills change the balance without the strategy or the risk gate counting
  them, so position limits, inventory and the recorded P&L are wrong;
- a second process placing orders (another Kestrel, another bot, the website,
  `kestrel_app order`) is cancelled the next time `run` starts, and can trade
  against the strategy's own quotes while it runs.

So while `run` is trading live, do not place orders on its symbols by any other
means, and use a separate account (or sub-account) for anything else. Orders on
symbols not listed in `[general] symbols` are not cancelled, but they still
share the account's balances. The read-only `account` command is safe to use
at any time.

Also mind what was on the account before `run` started. Binance.US spot has no
positions, so `GetPositions` returns nothing and the risk gate's
`max_position_notional` and the inventory only know about what the strategy
built up this run. Holdings the account already had are invisible to the gate:
start from an empty account or sub-account.

If `run` dies without stopping cleanly (a crash, `kill -9`, or a second
Ctrl-C), its strategy's orders stay open until the next `run` cancels them;
`cancel --all --confirm` clears them by hand.

A cancel clears the order book, not history. An order that filled moments
before the cancel has already moved its fill into history, and the next `run`
starts from its position and balance without an `OnFill`: the risk gate and
the recorded P&L are blind to it. Catching such a fill needs the
`account_fills` sync at run start, still to come (see [TODO.md](../TODO.md));
meanwhile `account --store` writes fetched fills into `account_fills` by hand.

**Check the account** (read-only: nothing is placed or cancelled). This is the
way to check that live credentials work:

```sh
./build/kestrel_app -c live.toml account --days 30
./build/kestrel_app -c live.toml account --order 412845258
./build/kestrel_app -c live.toml account --store   # also sync fills into account_fills
```

**Check an order without placing it.** `--test` sends it to Binance's test
endpoint, which checks the signature, the key's permissions and the symbol's
filters (price band, minimum value) without placing anything. It does not
check the balance.

```sh
./build/kestrel_app -c live.toml order buy ZEC/USDT 0.002 --limit 700 --test
```

**Place and cancel for real.** `--confirm` is required, so a real order is
never sent by accident:

```sh
./build/kestrel_app -c live.toml order buy ZEC/USDT 0.002 --limit 700 --confirm
./build/kestrel_app -c live.toml cancel 412845258 --confirm
./build/kestrel_app -c live.toml cancel --all --confirm
```

An order without `--limit` or `--stop` is a market order. Orders are rounded to
the symbol's tick and lot and refused before they are sent if they fall under
its minimum size or value. Every order gets a `kst-...` client order ID unless
`--client-id` gives one. A placement that times out is never sent again: it
is looked up by its client order ID instead.
