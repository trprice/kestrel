# Venues

How Kestrel connects to a market: where its prices come from, how orders
reach an exchange or broker, and what is the same on every provider. Each
provider's own details are in its document:

| Provider | Document | Market | Live trading |
|---|---|---|---|
| `binanceus` | below, under [Binance.US](#binanceus) | crypto spot | yes |
| `kraken` | below, under [Kraken](#kraken) | crypto spot | no: the trading venue is a stub |
| `schwab` | [schwab.md](schwab.md) | US stocks and ETFs | written, off until `[schwab] live_trading = true` |

The config file's `[general] provider` picks the provider and `[general]
mode` picks `paper` or `live` (see
[configuration.md](../configuration.md#general)).

## Terms

- **Provider**: a company Kestrel talks to (Binance.US, Kraken, Schwab), and
  the `provider` name in the config.
- **Data source**: Kestrel's read-only connection to a provider's market data
  (`data::DataSource`): historical bars, live quotes and bars, symbol rules
  and market hours.
- **Trading venue**: where orders go (`trading::TradingVenue`): placing and
  cancelling orders, and reading open orders, fills, positions and balances.
- **Paper venue**: a trading venue simulated inside Kestrel
  (`PaperTradingVenue`). It fills orders against the data source's live
  quotes and never sends an order anywhere.
- **Live venue**: a trading venue that sends orders to the provider and trades
  the real account (`BinanceUSTradingVenue`, `SchwabTradingVenue`).
- **Symbol**: Kestrel writes every symbol one way, whatever the provider
  calls it: `BTC/USD` for a pair (base/quote), `AAPL` or `BRK.B` for a stock.
  Each provider class converts to and from its own form, and nothing outside
  it sees the provider's form.
- **Tick size**: the smallest price step a symbol trades in. **Lot size**:
  the smallest quantity step. **Minimum order size** and **minimum notional**
  (price times quantity): the smallest order the provider accepts.
- **Bar**: open, high, low, close and volume over one timeframe (`1m`, `1h`,
  `1d`, ...). A bar is **closed** once its time is over, and **forming**
  until then.
- **Maker** and **taker**: an order that rests on the book and is filled by
  someone else's is the maker; the order that trades against it at once is
  the taker. Many venues charge the taker more.
- **Quote**: the best bid and ask, with their sizes, and the last trade.
- **Fill**: one execution of an order, whole or part. An order may fill in
  several pieces.
- **OCO** (one-cancels-other): a limit order and a stop order on the same
  quantity; when one fills, the venue cancels the other. Used for a take-profit
  and a stop-loss on one position.
- **Time in force**: how long an order lasts: GTC (good till cancelled), Day
  (until the session's close), IOC (immediate or cancel: what does not fill
  at once is cancelled) or FOK (fill or kill: all at once or nothing).

## Paper and live

The same strategy code runs on either. Only the trading venue changes; the
data source is the provider's live one in both:

| | Paper (`mode = "paper"`) | Live (`mode = "live"`) |
|---|---|---|
| Market data | the provider's, live | the provider's, live |
| Orders | filled by `PaperTradingVenue` against the live quotes, with the fees, slippage and latency in `[paper]` | sent to the provider |
| Balances | `[paper] initial_balances` | the account's |
| Credentials | none, except Schwab, whose market data needs them | the provider's API key and secret, or Schwab's tokens |
| `[risk]` limits | optional | required by `run` |

`backtest` uses the paper venue too, on a `ReplayDataSource` that plays back
stored bars and quotes instead of a live feed.

`kestrel_app` builds both halves in one place, `factory.cpp`
(`CreateComponents`), from the provider and mode. `run` refuses live mode on a
provider without a working trading venue (`NoLiveVenue` in `factory.cpp`):
Kraken always, and Schwab until `[schwab] live_trading` is set.

## The layers between a strategy and the venue

A strategy never holds the trading venue itself. The runner puts these in
front of it, each a `TradingVenue` that wraps the next:

```
  Strategy  (Context::Venue())
     |
  OrderTracker       remembers the strategy's active orders, so the runner
     |               can poll their state
  SessionGatedVenue  refuses new orders while the market is closed (only
     |               for a data source whose market closes: Schwab)
  RiskGatedVenue     refuses orders that break a [risk] limit, and can close
     |               positions itself (only with a [risk] table)
  the venue          PaperTradingVenue, BinanceUSTradingVenue or
                     SchwabTradingVenue
```

Cancels and reads pass through every layer unchanged, so a strategy can always
take its orders down. The `order` command checks a manual order against the
`[risk]` limits that apply to one order (see
[running.md](../running.md#live-trading-on-binanceus)); `account` and `cancel`
use the venue directly.

## Market data

What every data source does (`include/kestrel/data/data_source.hpp`):

- **`GetBars(symbol, timeframe, start, end)`**: the closed bars that open
  from `start` up to `end`, oldest first, fetched in as many requests as the
  provider needs. The forming bar is never returned.
- **`StreamQuotes(symbols)`**: live quotes over a WebSocket. A provider that
  sends only the fields that changed has them merged, so every quote carries
  the latest value of each field. The stream reconnects by itself after a
  disconnect.
- **`StreamBars(symbols, timeframe)`**: each bar once, when it closes. Bars
  that closed while the stream was disconnected are not replayed; the runner
  fetches them with `GetBars` (it **backfills the gap**). A provider without a
  bar stream returns `kNotImplemented`.
- **`GetSymbolInfo(symbol)`**: the symbol's tick size, lot size and minimums,
  fetched once and then cached. The runner fetches every symbol's at startup,
  so an unknown symbol stops a run before it starts.
- **`GetTrades(symbol, start, end)`**: the market's public trades, for
  `trades --store` and `backtest --quotes-from-trades`. Binance.US only.
- **`Hours()`**: when the market can be traded. Crypto never closes. For US
  stocks it is the exchange calendar (see [schwab.md](schwab.md#market-hours)).

Stream callbacks run on the WebSocket library's thread, never the caller's,
so the runner only queues what they deliver; its own thread hands each event
to the strategy (see [design.md](../design.md#threads)).

Every WebSocket is pinged every 20 seconds and reconnected when the answer
does not come back by the next ping. `run` logs a quote stream that has been
quiet for 2 minutes of open market time, and with `[risk] max_quote_age_ms`
the risk gate refuses new positions until quotes return. A bar strategy's
feed is not checked.

## Orders

What every trading venue does (`include/kestrel/trading/trading_venue.hpp`):

- **`PlaceOrder`**: a market, limit or stop order. Before it is sent, the
  order is fitted to the symbol's rules (`ApplySymbolRules`): a buy limit's
  price is rounded down to the tick and a sell limit's up, a stop price to the
  nearest tick, and the quantity down to the lot size. An order under the
  minimum size, or a limit order under the minimum notional, is refused
  without being sent. Returns the venue's order ID and the order's first
  state.
- **`PlaceOco`**: a one-cancels-other pair. Optional: the paper, Binance.US
  and Schwab venues have it.
- **`CancelOrder`**: says whether the order was cancelled, had already filled
  (a normal outcome, not an error), or was already closed. An unknown ID is
  `kNotFound`.
- **`CancelAll`**: cancels every open order; on a live venue, only those on
  the configured symbols.
- **`GetOpenOrders`**, **`GetOrder(id)`**: the orders still working, and one
  order's current state, open or closed.
- **`GetFills(since)`**: every fill at or after `since`, partial ones
  included. Each fill has an ID unique on its venue, so the same fill seen
  twice is counted once.
- **`GetPositions`**, **`GetBalances`**: what the account holds.
- **`StreamExecutions`**: fills and order changes pushed as they happen.
  Optional: only the paper venue has it so far.

Not every venue takes every order. Which time-in-force values and order
options a venue refuses is in its section or document.

### How fills reach the strategy

The strategy hears of fills through `OnFill` and of order changes through
`OnOrderUpdate`. The runner learns of them in one of two ways:

- **Pushed**: a venue with `StreamExecutions` tells the runner the moment a
  fill happens, and the runner still polls every 30 seconds
  (`fill_backstop_interval`) in case a push was missed.
- **Polled**: otherwise a second thread asks the venue for new fills and the
  state of the strategy's orders every `fill_poll_interval`: 1 second by
  default, 5 seconds on Schwab, whose requests count against a smaller limit.

Either way the same fill is delivered once, checked by its fill ID.

## Rules every live venue keeps

- **A placement is never sent twice.** Placing and cancelling are not retried,
  since the first attempt may have taken effect. When a placement ends
  without an answer (a timeout, a dropped connection, a server error), the
  venue looks for the order on the account instead: found, it is acknowledged
  as it stands; not found, the error is returned, and the order may still
  appear later. Reads are retried with backoff.
- **Requests stay under the provider's rate limits**, which each venue keeps
  with its own counters (`util::RateLimiter`), and a provider's request to
  slow down (HTTP 429, `Retry-After`) is honoured.
- **Only the configured symbols are Kestrel's.** `CancelAll`, the open orders
  and the fills cover the symbols in `[general] symbols` only, so orders on
  other symbols are left alone.
- **One trader per account.** `run` cancels every open order on its symbols
  when it starts, and assumes nothing else trades them while it runs. See
  [running.md](../running.md#one-trader-per-account) for why.
- **When `run` stops**, it cancels its strategy's open entry orders and keeps
  the orders that protect a position, such as its stop.
- **Secrets stay out of logs.** API keys, tokens and account numbers are never
  logged or put in an error message.

## The paper venue

`PaperTradingVenue` fills orders from the data source's quote stream, on any
provider: market orders at the bid or ask plus `[paper] slippage_bps` (taker
fee), resting limit orders at their price (maker fee), and stop orders become
market orders when the price reaches them. It keeps balances, reserves the
funds an open buy needs, and pushes its fills. While the market is closed it
fills nothing. Its options, including order and report latency and short
selling, are in [configuration.md](../configuration.md#paper), and the details
of its fill model in [design.md](../design.md#design-notes).

On a provider whose symbols have no quote asset (US stocks), balances are
kept in `[paper] default_quote_asset` (default `USD`).

## Binance.US

- **Market data** needs no credentials: REST bars and symbol info, and the
  WebSocket for quotes and bars. Public trades too (`GetTrades`), with the
  side that took liquidity.
- **Trading** (`BinanceUSTradingVenue`) signs each request with the API key.
  Orders: market, limit (GTC, IOC or FOK; post-only as `LIMIT_MAKER`), stop
  (a stop-market, GTC only) and OCO. Binance.US has no day orders. Each order
  gets a client order ID, `kst-`, a tag for the process and a counter,
  unless one is given, which is
  how a placement that timed out is found again. `order --test` sends an
  order to Binance's test endpoint, which checks it without placing it, but
  not the balance.
- **Not pushed yet:** fills are polled. Every poll asks about each symbol in
  turn, and the requests count against Binance's per-minute request weight,
  so a run on several symbols needs a longer `fill_poll_interval`. The
  user-data stream that would push them is in [TODO.md](../../TODO.md).
- **No positions:** spot accounts have only balances, so `GetPositions` is
  empty, and the risk gate counts what it has seen fill since the run started
  (see [running.md](../running.md#one-trader-per-account)).

## Kraken

- **Market data** needs no credentials: REST bars (only the newest 720 of a
  timeframe; older ranges are `kUnsupported`) and symbol info, and quotes over
  the v2 WebSocket. No bar stream, so bar strategies get their bars from
  `GetBars`, and no public trade history.
- **Trading** is a stub: every call returns `kNotImplemented`, and `run`
  refuses live mode. The endpoints it needs are listed in
  `include/kestrel/trading/kraken_trading_venue.hpp`. Paper mode works.

## Where the code is

| Code | What it is |
|---|---|
| `include/kestrel/data/data_source.hpp` | the `DataSource` interface |
| `include/kestrel/trading/trading_venue.hpp` | the `TradingVenue` interface |
| `include/kestrel/data/<provider>_*.hpp` | each provider's data source and streams |
| `include/kestrel/trading/<provider>_*.hpp` | each provider's trading venue (and Binance.US's signed API client) |
| `include/kestrel/data/symbols.hpp` | symbol conversion to and from each provider's form |
| `include/kestrel/trading/paper_trading_venue.hpp` | the paper venue |
| `include/kestrel/trading/order_rules.hpp` | `ApplySymbolRules` |
| `include/kestrel/trading/session_gated_venue.hpp`, `risk_gate.hpp` | the layers in front of a venue |
| `src/factory.cpp` | which classes a provider and mode get |
