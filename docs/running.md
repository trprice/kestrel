# Running Kestrel

## Required packages

A machine that only runs `kestrel_app`, without building it, needs the shared
libraries it links against. The Release binary that CI builds on Ubuntu 26.04
(the `kestrel_app-<commit>` artifact) runs on a clean Ubuntu 26.04 with:

```sh
sudo apt install --no-install-recommends ca-certificates libcpr1 libpq5 \
    libspdlog1.15 libfmt10 libtomlplusplus3t64 libssl3t64 zlib1g
```

| Package | Why |
|---|---|
| `ca-certificates` | verifies the venues' TLS certificates (REST and WebSocket) |
| `libcpr1` | HTTP client; pulls in libcurl |
| `libssl3t64`, `zlib1g` | TLS and compression, also for the WebSocket client (IXWebSocket is linked in) |
| `libpq5` | PostgreSQL client, for `--store`, `init-db`, `backtest` and `[recording]`; needed to start even without a database |
| `libspdlog1.15`, `libfmt10` | logging |
| `libtomlplusplus3t64` | reading the config file |

The package names and versions are Ubuntu 26.04's. A binary built elsewhere
links against whatever that machine had installed, and dependencies fetched
during the build (see [building.md](building.md)) are linked in rather than
needed at run time; `ldd kestrel_app` lists what a given binary needs, and any
line saying `not found` is a missing package.

The database is only needed by the commands above that use it. It can be on
another machine; the one that holds it needs PostgreSQL with TimescaleDB
(`postgresql-18 postgresql-18-timescaledb` on Ubuntu 26.04), and setting it up
with `scripts/setup_database.sh` needs `psql`. See [database.md](database.md).

## Usage

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
| `backtest <strategy> --from <date> --to <date> [--quotes-from-trades] [--fill-at-close] [--fill-timeframe TF] [--equity-csv FILE [--equity-interval TF]]` | replays a strategy over stored bars and quotes (or quotes rebuilt from stored trades) and prints a P&L summary; `--fill-at-close` fills orders placed on a bar's close at that close; `--fill-timeframe` fills on finer bars (e.g. `1m`) than the strategy's; `--equity-csv` also writes the equity curve, sampled every `--equity-interval` (default `1m`; use `1h` or `1d` for a backtest of years); without it only the curve's ends are kept |
| `trades --from <date> --to <date> [--store]` | the venue's public trades per symbol, a day at a time (Binance.US); `--store` saves them for `backtest --quotes-from-trades` |
| `run <strategy>` | trades a strategy on live data, through the paper or live venue |
| `account [--days N] [--order ID] [--store]` | the venue's balances, positions, open orders and the fills of the last `N` days (default 7), and one order by ID; read-only on the venue |
| `order <buy\|sell> <symbol> <quantity> [--limit P \| --stop P] [--tif gtc\|ioc\|fok\|day] [--post-only] [--client-id ID] [--no-risk] (--test \| --confirm)` | one order on the live venue, checked against the `[risk]` limits that apply to one order; Binance.US refuses `day`, and a stop order must be `gtc` (the default); `--post-only` makes a limit order refused rather than take liquidity |
| `cancel <order_id> \| --all --confirm` | cancels one order, or every open order on the configured symbols, on the live venue |
| `authorize [--status]` | Schwab only: authorizes Kestrel to use the account and saves its tokens; `--status` shows when they expire |

`--store` writes what a command fetches to the `[database]`; see
[database.md](database.md).

## Authorizing Schwab

Schwab's API needs OAuth tokens for everything, market data included, so
authorize once before any other command on `provider = "schwab"`:

1. Register an app at developer.schwab.com with the Market Data Production
   and Accounts and Trading Production products, and set its callback URL to
   `[schwab] redirect_uri` (default `https://127.0.0.1:8182`) exactly. Wait
   for its status to be Ready For Use.
2. Give Kestrel the app's key and secret (see
   [configuration.md](configuration.md#credentials)).
3. Run `kestrel_app -c config/schwab.toml authorize`. Open the page it prints,
   log in to Schwab and approve access. Schwab sends the browser to the
   callback URL, which shows an error page because nothing listens there;
   that is expected. Paste the whole address from the address bar into the
   prompt straight away, since the code in it is short-lived.

The browser can be on any machine: Schwab never connects to the callback URL,
it only sends the browser there. The tokens are saved to `[schwab]
token_file`. After that the access token is renewed automatically, but the
refresh token expires 7 days after the authorization; run `authorize` again
before then (`authorize --status` shows when).

## Backtesting and running strategies

```sh
./build/kestrel_app -c config/kestrel.toml backtest ma_crossover --from 2026-09-01 --to 2026-10-01
./build/kestrel_app -c config/kestrel.toml run ma_crossover
```

- `backtest <strategy> --from <date> --to <date>` fetches the strategy's
  bars (plus its warmup) from the database, falling back to the provider for
  missing bars, replays them and any stored quotes through the same `Runner`
  and a paper venue using `[paper]`, and prints the fills, positions,
  balances and a summary (below). `--to` is exclusive; dates are
  `YYYY-MM-DD` or ISO 8601 UTC with the `Z` (`2026-10-19T17:00:00Z`).
  `backtest` records no run, fills or P&L. With a `[database]`, it does save
  the bars it had to fetch from the provider, so the next backtest finds them.
- **Fills.** Without recorded quotes, each bar is walked as open, the
  extreme nearer the open, the other extreme, close, and a resting stop or
  limit trades at its own price on the way. An order a strategy places on a
  bar's close (a market buy after a crossover, a target or time exit) trades
  at the next price replayed, since a signal computed from a close cannot
  also be filled at it: the next bar's open, the next session's open if the
  market has closed, or with recorded quotes the next quote. One placed on
  the last close replayed never fills. A buy that gaps above the funds the
  account has free is cancelled at the fill. `--fill-at-close` fills these
  orders at the close instead, the optimistic model of earlier versions.
  Orders placed elsewhere (in session hooks, on quotes, after a fill inside a
  bar) trade at once, as do immediate-or-cancel and fill-or-kill orders, which
  are always decided against the quote they are placed on.
  Which extreme of a bar came first is a guess (the nearer one), so a bar
  that reaches both an entry and its stop is decided by it.
  `--fill-timeframe 1m` walks 1-minute bars for the venue instead, so the
  path within each strategy bar is the recorded one; the strategy still
  sees only its own bars. A strategy bar with no 1-minute bars inside it
  (before the provider's 1-minute history, or a gap) is walked whole, and
  the report counts those. Not for a strategy that takes quotes, which
  fills on them.
- **The backtest summary** values every balance in the quote asset after
  each quote (at its mid, or the last trade when a side is missing) or, for
  a strategy without quotes, each bar close:

  ```
  summary (USDT), 2026-10-30T17:50:23.391Z to 2026-10-31T17:49:00.000Z
    start equity   12465.73
    end equity     12560.242185
    return         +0.76% (+94.512185)
    hold start     12570.79  (the starting balances valued at the end)
    vs hold        -10.547815
    max drawdown   -0.70% (-88.634435, 2026-10-31T01:37:45.765Z to 2026-10-31T04:55:35.511Z)
    realized       +88.45651371
    unrealized     +6.05567129
    fills          1172 (495 buy, 677 sell; 1172 maker, 0 taker)
    volume         55314.00937
    fees           0
    closing fills  677, 440 won (64%)
    markouts       against the mid, fees not included
      at the fill  +1.37 bps (+7.589135, 1172 fills)
      1 s          -1.02 bps (-5.617535, 1172 fills)
      10 s         -1.21 bps (-6.711875, 1172 fills)
      1 min        -1.30 bps (-7.209975, 1172 fills)
      5 min        -1.99 bps (-11.002845, 1170 fills)
      at the end   -1.91 bps (-10.547815, 1172 fills)
    ZEC/USDT  1232.865 -> 1285.395; held 1.711 .. 2.406 ZEC (start 2, end 2.009)
  ```

  - **vs hold** is the end equity less what the starting balances alone are
    worth at the end: what trading added beyond holding them. Read it before
    the return, which on a day the price moves is mostly the move.
  - **max drawdown** is the largest fall from a peak, checked at every price.
  - **realized** is average cost, fees included, with the starting holding
    given the first mark as its cost basis; **unrealized** is the rest of the
    change in equity.
  - **closing fills** are sells of something held and buys covering a short,
    and **won** those that realized more than zero: the rule the risk gate
    counts losing sells by.
  - **markouts** split what the fills made: for a buy, (the mid then - the
    price) * quantity, the reverse for a sell, in quote asset and in bps of
    the fills' volume. **at the fill** is against the mid before the fill:
    the spread captured. The fall from there to 1 s, 10 s, 1 min and 5 min
    is adverse selection, the price moving against the fill; above, quotes
    2 bps wide captured +1.37 bps and lost it within a second. **at the end**
    is what the fills made in all, equal to vs hold plus the fees, so what
    it gains after 5 min is the inventory riding the price rather than the
    quoting. A fill closer to the end than a horizon is left out of it.
  - The summary needs the symbols to share a quote asset, and every starting
    balance to be that asset or a symbol's base; otherwise it is skipped
    with the reason.
  - `--equity-csv FILE` also writes the equity curve: a row when every
    balance first has a price, then one every `--equity-interval` from
    `--from` (default `1m`; `1h` or `1d` keeps a backtest of years small),
    and a last one at `--to`: `time`, `equity`, `hold_equity`, and
    each symbol's mark and base asset held. A path that cannot be written is
    refused before anything is loaded.
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
- **On stop, `run` cancels entries and keeps exits.** Before the strategy's
  `OnStop`, `run` cancels the orders open on its symbols that would open or
  add to a position (a breakout's buy-stop, a resting buy) and leaves those
  that close one: sells up to what is held when long (buys when short),
  stops first. So a position keeps its protective stop while nothing runs,
  and no entry fills unwatched. An entry that filled first still reaches the
  strategy's `OnFill`, which places its stop, before `OnStop`. It logs what
  it cancelled and what it left, and warns about a symbol held with no
  closing order left. A sell beyond what is held is cancelled. If the
  holdings cannot be read, it keeps every sell and cancels every buy. The next `run` cancels what was
  left before it starts, and does not know the position it protected: close
  that position or watch it.
- With `[recording] enabled = true`, `run` stores its fills, P&L snapshots and
  response times as it goes; see [database.md](database.md#recording-a-run).
- `run` logs a quote feed that has gone quiet for 2 minutes of open market
  time (nights and weekends do not count), and again when quotes return. It
  only logs: the strategy carries on. Every stream pings its WebSocket every
  20 s and reconnects one whose reply does not come back by the next ping;
  Schwab's streamer also reconnects a connection that has sent nothing, not
  even a heartbeat, for 60 s.

The strategies, with their parameters, are described in
[strategies/](strategies/README.md), and the risk limits in
[configuration.md](configuration.md#risk). To write your own strategy, see
[writing-strategies.md](writing-strategies.md).

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
positions, so `GetPositions` returns nothing, and the risk gate counts what it
has seen fill since the run started instead: `max_position_notional`, and the
sells let through once a halt or a loss limit blocks entries, only know about
what the strategy built up this run. Holdings the account already had are
invisible to the gate (a halted gate will not let them be sold; `order
--no-risk` can): start from an empty account or sub-account.

If `run` dies without stopping cleanly (a crash, `kill -9`, or a second
Ctrl-C), all its strategy's orders stay open, entries too, until the next
`run` cancels them;
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

**Risk limits apply to manual orders too.** With `--test` or `--confirm`, the
order goes through the risk gate with the `[risk]` limits one order can trip:
`allowed_symbols`, `max_order_notional`, `max_position_notional`,
`max_total_notional`, `max_symbol_share`, `max_open_orders` and
`max_price_age`. The loss, drawdown, order-count and stale-quote limits are
left out, since a process that places one order would only count them from
its own start. A market order is valued at the newest 1-minute bar's
close, and is refused if there is none in the last 15 minutes and a notional
limit is set. `--no-risk` skips the gate, with a warning: for an exit larger
than `max_order_notional`, say.
