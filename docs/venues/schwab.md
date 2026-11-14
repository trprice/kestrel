# Schwab

How Kestrel works with Charles Schwab's API, for US stocks and ETFs. What is
the same on every provider (the data source and trading venue interfaces,
paper and live, the layers in front of a venue, how fills are polled) is in
[README.md](README.md); this document covers what is particular to Schwab.

**Status (Nov 2026).** Market data works, and strategies run on Schwab in
paper mode. The trading venue (`SchwabTradingVenue`) has traded on a real
account (Nov 13 2026: previewOrder, market orders bought and sold, and a
short live run), but `run` will not trade live until `[schwab]
live_trading = true`, while resting orders, OCO and margin accounts are
checked (see [TODO.md](../../TODO.md)). Setting up is in
[running.md](../running.md#authorizing-schwab) and
[configuration.md](../configuration.md#provider-tables), with a commented
example in `config/schwab.example.toml`.

## Terms

- **Trader API**: Schwab's API for individual developers. It comes as two
  **products**: **Market Data Production**
  (prices, price history, market hours, the streamer) and **Accounts and
  Trading Production** (accounts, orders, transactions). Kestrel's app must
  be approved for both.
- **App key** and **app secret**: the app's credentials from the developer
  portal (Kestrel's `KESTREL_SCHWAB_APP_KEY` and `KESTREL_SCHWAB_APP_SECRET`).
- **Callback URL** (`redirect_uri`): where Schwab sends the browser after the
  account holder approves the app. It carries the **authorization code**,
  which Kestrel exchanges for tokens.
- **Access token**: sent with every request; lasts 30 minutes. **Refresh
  token**: used to get new access tokens; lasts 7 days from the authorization,
  after which the account holder must authorize again.
- **Account number** and **hashValue**: Schwab's account endpoints take an
  encrypted form of the account number, the hashValue, in their URLs, never
  the number itself.
- **Streamer**: Schwab's WebSocket server for live data. A **service** is one
  kind of data on it: **LEVELONE_EQUITIES** (quotes for stocks and ETFs),
  **CHART_EQUITY** (1-minute bars), and **ACCT_ACTIVITY** (the account's
  order events, not used yet). Requests on it are commands: **LOGIN**,
  **SUBS** (subscribe), **ADD** (add symbols), **UNSUBS** (remove symbols).
  Schwab tells the client about the connection itself with **notify codes**.
- **Sessions**: the **regular session** is 09:30 to 16:00 New York time (13:00
  on an early-close day). **Extended hours** are the **pre-market** (07:00 to
  09:30) and **after hours** (the close to 20:00). The **overnight session**
  starts at 20:00; its bars were seen until 04:00 (Nov 2026), and Kestrel
  treats everything from 20:00 to 07:00 as outside the sessions it trades.
- **Order JSON**: the fields of Schwab's orders are described under
  [Schwab's order JSON](#schwabs-order-json).
- **Transaction**: an entry in the account's history. A **TRADE**
  transaction records a trade, with the order ID it came from.
- **Share class**: a second class of a company's stock, such as Berkshire
  Hathaway's class B: `BRK.B` in Kestrel, `BRK/B` at Schwab.

## Schwab's documentation

Schwab's API documents are on the developer portal,
[developer.schwab.com](https://developer.schwab.com), and need a free login
there. Under the Trader API (Individual) product, each of the two products
has its own specification of the REST endpoints; the streamer is described in
a separate Streamer document on the Market Data product. Schwab does not
publish them elsewhere, so this document names the endpoint (`GET
/pricehistory`, `POST /orders`, ...) to look up rather than linking to it.

The live API differs from those documents in places. What Kestrel found, and
works to, is noted where it matters below and collected under
[Where Schwab differs from its documents](#where-schwab-differs-from-its-documents).
The unofficial Python client
[schwab-py](https://schwab-py.readthedocs.io/en/stable/) is a useful second
reference, and its behaviour is what Kestrel follows for `previewOrder`.

## Authorization and requests

`kestrel_app authorize` (see [running.md](../running.md#authorizing-schwab))
saves the first tokens to `[schwab] token_file`. From then on `SchwabAuth`
(`include/kestrel/data/schwab_auth.hpp`) hands out the access token, renewing
it 5 minutes before it expires and saving each renewal to the token file.
Renewing does not give a new refresh token, so the 7 days always count from
the last `authorize`. Kestrel warns in the refresh token's last 24 hours, and
once it has expired or Schwab refuses it logs `SCHWAB RE-AUTHORIZATION
REQUIRED` with what to run.

Market data needs the tokens too, so Schwab needs the app key and secret and
a token file even in paper mode.

Every request goes through a `SchwabApi`, which adds the access token and
keeps to a rate limit. If Schwab answers 401 (unauthorized), the token is
renewed and the request sent once more; a 429 (too many requests) holds back
the requests that follow. Schwab allows an app 120 requests a minute, and
each `SchwabApi` keeps under that on its own: 1.8 a second, with bursts of 5.
The data source and the trading venue have one each, so together they could
exceed it; the trading venue's polling is kept slow for that reason (see
[Fills and order changes during a run](#fills-and-order-changes-during-a-run)).
The trading venue also holds placing and cancelling to 1 a second (bursts of
5).

Base URLs: `https://api.schwabapi.com/marketdata/v1` for market data and
`https://api.schwabapi.com/trader/v1` for accounts and trading.

## Market data

`SchwabDataSource` (`include/kestrel/data/schwab_data_source.hpp`).

### Symbols

Stocks and ETFs only; options and futures are not mapped. Symbols are the
ticker, except that a share class is written with a dot in Kestrel and a slash
at Schwab (`BRK.B` and `BRK/B`). Schwab answers `BRK.B` with no data rather
than an error.

### Historical bars

`GetBars` uses `GET /pricehistory`.

- **Timeframes**: 1, 5, 10, 15 and 30 minutes, 1 day, 1 week and 1 month.
  Schwab has no hourly bars.
- **Whole days**: Schwab answers in whole days whatever times are asked for,
  so Kestrel trims the bars to the range asked for. Kestrel asks for at most
  10 days of minute bars or 20 years of daily and longer ones a request
  (Schwab returned 30 days and 33 years to one request); longer ranges are
  fetched in pages, each retried on a timeout, 429 or server error.
- **History**: Schwab keeps a limited intraday history. In Nov 2026, for SPY:
  1-minute bars went back about 48 days, 5-minute about 200, 30-minute about
  260, and daily bars to 1993. Days before that come back empty, not as an
  error, and Kestrel logs a range that starts before the history.
- **Unknown symbols** also come back empty, not as an error.
- **Sessions**: intraday bars are the regular session's unless `[schwab]
  extended_hours` is set; then they include the pre-market, after hours and
  the overnight session. Kestrel always sends `needExtendedHoursData`, since
  Schwab's default is true.
- **Daily, weekly and monthly bars** are stamped midnight US Central time by
  Schwab. Kestrel gives them an open time of 00:00 UTC on their date.

### Symbol info

Schwab publishes no tick or lot size, so `GetSymbolInfo` applies US equity
rules: a tick of $0.01, or $0.0001 for a stock under $1 (SEC Rule 612), and a
lot and minimum of 1 share. It first checks the symbol with one `GET /quotes`:
an unknown symbol is `kNotFound` (so a run on it stops at startup), anything
but a stock or ETF `kUnsupported`, and the price decides the tick. The answer
is cached, so the tick is the one from when it was first asked. The trading
venue does not rely on that: it takes the tick from each order's own price.

### The streamer

Live quotes and bars come over Schwab's streamer (`SchwabStreamer`,
`include/kestrel/data/schwab_streamer.hpp`).

- **Connecting**: the first subscription asks `GET /userPreference` for the
  streamer's address and the login details (it works for an app with only
  the market data product, and gave `wss://streamer-api.schwab.com/ws`). On
  each connect the streamer sends LOGIN with a current access token, then
  subscribes to every symbol wanted on each service. Later subscriptions ADD
  their symbols, and a stopped one UNSUBS those nobody else wants. The
  connection closes when the last subscription stops.
- **One connection per user**: Schwab allows a user one streamer connection.
  A second LOGIN, from another `kestrel_app` or from thinkorswim, disconnects
  the first with notify code 12. So one process holds one connection, and
  its quote and bar streams share it. What happens to the process that was
  disconnected is set by `[schwab] takeover_reconnect_ms`: with 0 (the
  default) it stops streaming for good, logs an error, and its subscriptions
  go quiet; with more than 0 it waits that long and connects again, which
  disconnects the other one in turn. Two processes that both set it take the
  connection from each other at every wait.
- **A refused login** renews the access token and connects again. A second
  refusal in a row, or a refresh token that has expired, stops the streamer.
- **Silence**: Schwab sends a heartbeat about every 10 seconds, trading or
  not, so a connection that hears nothing for 60 seconds is taken as dead and
  reconnected.
- **STOP_STREAMING** (notify code 30, in the Streamer document but not seen
  live) would leave the connection open and silent, so the streamer closes it
  and connects again 30 seconds later.

### Quotes

`StreamQuotes` subscribes to LEVELONE_EQUITIES for the bid, ask, last price,
their sizes, the security status and the quote and trade times.

- Schwab sends a symbol's changes about once a second, and only the fields
  that changed; Kestrel merges them into full quotes.
- Bid and ask sizes are in shares, as in REST `/quotes`, not in lots of 100
  as the Streamer document says.
- A price of 0 means none yet and is not used.
- An unknown symbol comes back as zeros with security status `Unknown`; it is
  logged once and gives no quotes. Delayed (not real-time) data is logged
  once per symbol.
- Schwab does not say which side of a trade took liquidity, so quotes have no
  taker side.

### Streamed bars

`StreamBars` subscribes to CHART_EQUITY, which streams 1-minute bars only.

- Each minute is sent once, about 2 seconds after it closes, and the last
  closed minute is sent again on every subscribe; never the forming minute.
  Kestrel ignores the repeat.
- The fields are numbered one higher than the Streamer document says: 1 is
  the minute's sequence number, 2 to 5 the open, high, low and close, 6 the
  volume and 7 the minute's start. The volume can be fractional (REST
  `/pricehistory` truncates it). The bars match `/pricehistory`'s.
- 5, 10, 15 and 30-minute bars are built from the minutes
  (`SchwabBarAggregator`), opening at multiples of their length (a 30-minute
  bar at :00 or :30). A minute with no trades has no bar, so a longer bar
  whose last minute had none closes with the next minute that has trades.
  After a connect or reconnect, the first longer bar is skipped if minutes of
  it were missed; the runner backfills it with `GetBars`.
- Without `extended_hours`, only bars in the regular session are given, by
  the market calendar (so early closes count).
- An unknown symbol gets no bars and no error.

## Market hours

US stocks trade on a calendar, so the Schwab data source's `Hours()` is not
"always open": the runner refuses orders while the market is closed, the
paper venue fills nothing, and gap backfills skip closed time (see
[README.md](README.md#the-layers-between-a-strategy-and-the-venue)).

- **The calendar** is Kestrel's own rules (`UsEquityCalendar`: weekends,
  NYSE holidays and early closes, and the unscheduled closures since 2001),
  checked against Schwab's `GET /markets` (`SchwabCalendar`). Schwab answers
  for dates from a week back to a year ahead; each date is asked about once,
  in the background, and until its answer arrives the rules decide, so
  nothing waits on Schwab. A failed request is retried after 10 minutes.
- **Schwab can only restrict a day**: close it, open it later or close it
  earlier, for a closure announced at short notice. It cannot open a day the
  rules close, because it fills in the next year's holidays late (in Nov 2026
  every 2027 holiday read as a normal day). A difference is logged once.
- **Open**, for orders: the regular session, or with `extended_hours` also
  the pre-market and after hours. In paper mode orders can then fill in
  extended hours; live orders are still sent for the regular session (see
  [Placing orders](#placing-orders)). The overnight session never counts as
  open:
  with `extended_hours` its bars come, but orders are refused and a gap in its
  bars is not backfilled.
- A bar that closes at the session's close arrives after it, so an order
  placed on that bar is refused.

## Trading

`SchwabTradingVenue` (`include/kestrel/trading/schwab_trading_venue.hpp`). It
shares the data source's tokens through its own `SchwabApi`.

### The account

The venue reads `GET /accounts/accountNumbers` once, for the hashValue of the
account to trade: the one whose number or hashValue is `[schwab]
account_number`, or the first linked account (with a warning if there are
more). Account numbers are masked in every log line, and URLs are logged as
`.../accounts/{account}/orders/{id}`, so neither the hashValue nor an order ID
fills the logs or the request-timing statistics.

### Schwab's order JSON

The fields Kestrel writes when it places an order and reads back:

- `orderType`: `MARKET`, `LIMIT`, `STOP` and others;
- `session`: which session the order may trade in (`NORMAL` is the
  regular session);
- `duration`: Schwab's word for time in force (`DAY`, `GOOD_TILL_CANCEL`,
  `IMMEDIATE_OR_CANCEL`, `FILL_OR_KILL`);
- `orderStrategyType`: `SINGLE` for one order, `OCO` for a
  one-cancels-other pair, whose two orders are its
  `childOrderStrategies`;
- `orderLegCollection`: the order's legs, each with an `instruction`
  (`BUY`, `SELL`, ...), a quantity and an `instrument` (a symbol and an
  `assetType`: `EQUITY` for a stock, `COLLECTIVE_INVESTMENT` for some ETFs);
- `orderActivityCollection`: what has happened to the order; an
  `EXECUTION` activity is a fill, with an `activityId` and its
  `executionLegs` (quantity, price and time);
- `status`: `WORKING`, `FILLED`, `CANCELED` and others (see
  [Order states](#order-states)).

### Placing orders

`POST /accounts/{hash}/orders`. Before it is sent, an order is fitted to the
symbol's tick and lot (see [README.md](README.md#orders)). Every order is
for the stock's `EQUITY` asset type and `session` `NORMAL`: the regular
session, even with `extended_hours`.

| Kestrel order | `orderType` | Time in force allowed (`duration`) |
|---|---|---|
| market | `MARKET` | Day only (GTC is sent as Day) |
| limit | `LIMIT` | GTC, Day, IOC, FOK |
| stop (a stop-market) | `STOP` | GTC or Day |
| OCO | `orderStrategyType` `OCO`, a `LIMIT` and a `STOP` child, both GTC | |

- **No post-only**: Schwab has no such flag, so a post-only order is refused
  before it is sent, and an OCO's limit leg is an ordinary limit.
- **No client order ID**: Schwab keeps none, so an order's `client_order_id`
  is known only to the process that placed it.
- **The order ID** is read from the `Location` header of Schwab's 201
  response (or `orderId` in its body). For an OCO, the two legs' IDs are read
  back from the parent order.
- **`order --test`** sends the order to `POST /previewOrder`, which checks it
  without placing it. It takes the same body as placing, as schwab-py sends
  it, not the PreviewOrder schema in Schwab's document.

### A placement that ends without an answer

When a placement times out, the connection drops or Schwab answers with a
server error, the order may or may not be on the account, and sending it
again could buy twice. So it is never sent again. Instead the venue reads the
orders entered from a minute before the placement until now, twice, 200 ms
apart, and looks for one that matches: a `SINGLE` order (not a leg of an OCO
or a replacement) on the same symbol, side, type, duration, quantity and
price, that this process did not already know. One match is acknowledged as
the placed order; with none, the error is returned saying the order may still
have been placed; with more than one, it does not guess. An OCO is looked for
the same way, by its two legs.

### Cancelling

`DELETE /accounts/{hash}/orders/{id}`. When Schwab refuses the cancel, says
the order is unknown, or the outcome is unknown, the venue reads the order to
say what became of it: filled (`kAlreadyFilled`), closed
(`kAlreadyClosed`), or cancelled after all. An order already in
`PENDING_CANCEL` counts as cancelled. `CancelAll` cancels the open orders on
`[general] symbols` only.

### Reading orders and fills

Orders come from `GET /accounts/{hash}/orders`, which filters by the time the
order was entered (`fromEnteredTime`, `toEnteredTime`), not by when it last
changed, returns at most 3000 orders, and takes at most a year between the
two times. Each fetch also reads the orders inside an order: an OCO's
`childOrderStrategies` and an order's `replacingOrderCollection`.

- **How far back**: a GTC order can work for up to 180 days, so a fill or a
  working order could belong to an order entered that long ago. Fetching 180
  days on every poll is slow, so the venue remembers, for each fetch, when it
  ran and the entry time of the oldest of its symbols' orders working then.
  The next fetch starts there: every order that can still change was working
  at an earlier fetch or was entered since. For example, if a fetch at 10:00
  finds that the oldest working order was entered at 09:12, the fetch at
  10:05 asks for orders entered from 09:12 on. Only the first fetch in a
  process goes back 180 days.
- **The 3000 cap**: a fetch that gets 3000 orders may have missed some, so its
  span is split in two and each half fetched, down to one minute.
- **Fills in a fresh process** (`account`, or the **fill sync**, `account
  --store`, which copies the account's fills into the database), with no earlier
  fetch to start from: the venue reads the orders entered since the time
  asked for, and the account's TRADE transactions over the same time
  (`GET /accounts/{hash}/transactions`, `types=TRADE`). A transaction names
  the order it came from, so an older order that traded in that time is read
  by its ID (`GET /orders/{id}`). If the transactions or a lookup fail, a
  transaction has no order ID, there are 1000 or more transactions, or more
  than 8 older orders to look up, it fetches the full 180 days instead.
  Transactions only say which orders to read: their `activityId` is not the
  execution's, so the fills themselves always come from the orders. A
  transaction can lag its execution, so the fill sync starts an hour before
  the oldest of the symbols' newest stored fills.
- **Orders Kestrel cannot read** (an option, an unknown status or
  instruction) are skipped, so that one does not stop the rest being read.
  An order whose JSON is malformed fails the whole read (see
  [Known limitations](#known-limitations)).

**Fills** are the `EXECUTION` activities in an order's
`orderActivityCollection`, one per execution leg. A fill's ID is
`<orderId>:<activityId>:<leg>`, the leg being its place among the
activity's `executionLegs`. Schwab gives no fee or liquidity on an
execution, so fills say a fee of 0 USD and taker (see
[README.md](README.md#terms)).

#### Order states

| Schwab `status` | Kestrel state |
|---|---|
| `FILLED` | filled |
| `CANCELED`, `REPLACED` | cancelled |
| `REJECTED` | rejected |
| `EXPIRED` | expired |
| `WORKING`, `QUEUED`, `ACCEPTED`, `NEW`, the `AWAITING_...` and `PENDING_...` states | new, or partly filled if some has filled |
| `UNKNOWN` | taken as working (with a warning), so it is not ignored |

Durations that last past the day (`END_OF_WEEK`, `END_OF_MONTH`, ...) read as
GTC, and order types Kestrel does not place read as the nearest it has: a
`STOP_LIMIT` or trailing stop as a stop, `MARKET_ON_CLOSE` as a market order.

### Positions and balances

Both come from `GET /accounts/{hash}` with `fields=positions`.

- **Positions**: the account's stock and ETF positions, long or short, with
  their average price.
- **USD balance**: the cash available is `cashAvailableForTrading`, or failing
  that the available funds, taking the lower of the current and projected
  balances. On a margin account the non-marginable figure is read first, so
  that borrowing power does not count as cash. The total is `totalCash` or
  `cashBalance`; on a margin account without them, a negative
  `marginBalance` (a margin loan), which leaves nothing available; failing
  all of those, the available funds, then the start of day's balances.
- **Shares held** (long positions; shorts are left out) are listed as
  balances too, so that whatever values the
  account from its balances (the risk gate's equity, `momentum_rotation`)
  counts them alongside the cash.

### Fills and order changes during a run

Schwab does not push them to Kestrel yet (the streamer's ACCT_ACTIVITY
service would; see [TODO.md](../../TODO.md)). `run` polls every 5 seconds
on Schwab, rather than every second, as each poll is two requests against the
app's rate limit, which it shares with market data.

## Where Schwab differs from its documents

Seen on the live API in Nov 2026:

| Schwab's document says | Live |
|---|---|
| LEVELONE_EQUITIES bid and ask sizes are in lots of 100 | in shares, as in REST `/quotes` |
| LEVELONE_EQUITIES: the document's example shows field 12 as something other than the previous close | field 12 is the previous close, as the document's field table says |
| CHART_EQUITY fields: 0 is the key, 1 to 5 open to volume | numbered one higher: 1 is a sequence number, 2 to 6 open to volume, 7 the start time |
| `previewOrder` takes a PreviewOrder object | it takes the same body as placing an order (as schwab-py sends it) |

Not in the documents, and found live:

- `/pricehistory` answers in whole days and keeps a limited intraday history;
  unknown symbols and days before the history come back empty.
- `needExtendedHoursData` defaults to true, and extended hours include the
  overnight session.
- `userPreference` works for an app with only the market data product.
- A second streamer LOGIN for the same user disconnects the first (notify
  code 12).
- CHART_EQUITY volume can be fractional.
- Renewing the access token does not rotate the refresh token.

Not yet seen live, so handled from the documents: Schwab's error for an
expired or revoked refresh token, a streamer login refused for a revoked
token, and STOP_STREAMING (code 30).

## Known limitations

- **Live trading is off by default** until the rest of the venue has been
  checked on a real account: placing and cancelling a resting order, an
  OCO's legs cancelling each other, an ETF bought, and the balance fields of
  a margin account. Market orders, `activityId` on fills, previewOrder and a
  cash account's balances have been checked.
- **Regular session only** for live orders, even with `extended_hours`.
- **No fees on fills**: Schwab's executions give no fee (the SEC and FINRA TAF
  fees on sales) or liquidity, so recorded P&L leaves those fees out.
- **One malformed order** anywhere on the account, on any symbol, fails the
  whole read of open orders or fills, and so `run`'s startup.
- **Margin accounts**: cash is read from the non-marginable fields, and a
  margin loan makes the USD total negative and nothing available.
- **Bars**: no hourly bars, and no streamed daily or longer ones (they come
  from `GetBars` only).
- **One streaming process per user** (see [The streamer](#the-streamer)).
- **The tick in paper mode** is set from the price when the symbol is first
  asked about, and kept until the next run; a stock crossing $1 keeps its old
  tick until then.
- **Pushed executions** (ACCT_ACTIVITY) are not implemented: fills are
  polled every 5 seconds.
