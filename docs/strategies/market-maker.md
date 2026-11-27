# Market maker

`market_maker` quotes a bid and an ask around a reservation price and earns
the spread when both sides fill. It quotes from the quote stream (`OnQuote`),
not from bars, on a spot pair; it was built and run on Binance.US.

Source: `include/kestrel/strategy/market_maker.hpp`,
`src/strategy/market_maker.cpp`. An example configuration with every
parameter is `config/binanceus-zec-marketmaker.example.toml`. Results are in
[strategy-tests.md](../strategy-tests.md#market-maker-zecusdt): the Nov 2026
replays found no setting that beat holding ZEC/USDT over the four recorded
days, so check a change's markouts, not only "vs hold".

## The model in the research

Its inventory skew follows Avellaneda and Stoikov, "High-frequency trading
in a limit order book" (Quantitative Finance, 2008;
[PDF](https://math.nyu.edu/~avellane/HighFrequencyTrading.pdf),
[doi](https://doi.org/10.1080/14697680701381228)):
- *The market:* the mid s moves as a random walk with volatility σ. A quote
  δ from the mid is filled at a rate A·e^(−kδ), so quotes farther away fill
  less often. The dealer trades until a horizon T and is averse to risk, by
  γ.
- *The reservation price:* r = s − q·γ·σ²·(T − t), where q is the inventory.
  It is the price at which the dealer is indifferent to holding one more
  unit. Long, it is under the mid, so the ask is likelier to fill than the
  bid. It is linear in the inventory, and leans further with more
  volatility and more time left.
- *The spread:* the bid and ask sit either side of r, a total of
  γ·σ²·(T − t) + (2/γ)·ln(1 + γ/k) apart: wider with volatility and
  risk aversion, narrower where fills fall off quickly with distance.

Its `microprice_weight` option rests on the idea of Stoikov, "The
micro-price: a high-frequency estimator of future prices" (Quantitative
Finance, 2018;
[SSRN 2970694](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2970694),
[doi](https://doi.org/10.1080/14697688.2018.1489139)): the sizes at the
touch say where the price goes next. Stoikov's micro-price is the mid's
expected value in the long run given the book's imbalance and spread,
estimated from the data. He shows it predicts the price better than the mid
or the size-weighted mid.

## How it quotes

**The reservation price** is the mid, or with `microprice_weight` a point
towards the microprice: the mid weighted by the size at the touch,
`(bid · ask_size + ask · bid_size) / (bid_size + ask_size)`. The microprice
sits nearer the side with less size, the side a few orders can clear, so it
leans towards where the price is likelier to go next. The centre is
`mid + microprice_weight · (microprice − mid)`: 0 is the mid and 1 the full
microprice, which tends to overstate the next move. A quote without both sizes
uses the mid. This "microprice" is the size-weighted mid, not Stoikov's
micro-price (see [The model in the research](#the-model-in-the-research)).

**Inventory skew.** The reservation price leans away from the side the
position is heavy on: long, both quotes shift down, so a sale is likelier than
a purchase. At `max_inventory` away from where it started the lean is
`skew_bps`, in proportion for less. With `skew_power` above 1 the lean is the
position's share of `max_inventory` raised to that power (keeping its sign),
so a small position barely leans the quotes and a large one leans them hard.
Avellaneda and Stoikov's lean is linear in the position, `skew_power = 1`;
higher powers are Kestrel's own. With `skew_age_ms` the skew also grows with
how long the position has been held on its side, to double at that age and
no further, so a position the market will not take back is leaned off harder
as it ages.
The clock starts when the position last reached half a `quantity` on that
side; falling under that, or crossing zero, starts it again.

**The spread.** Each quote sits `half_spread_bps` of the mid from the
reservation price. That must cover the fees on both legs and leave room for
adverse selection. With `vol_multiplier`, the half-spread becomes the larger
of `half_spread_bps` and `vol_multiplier` times the mid's typical one-minute
move in bps, capped at `max_half_spread_bps` when that is set. The volatility
is worked out from the mid's moves over the last `vol_window_ms`, sampled at
most once a second and divided by the time they cover, so a gap in the feed
does not distort it. Until a whole window has been seen, `half_spread_bps`
alone is used.

**Levels.** With `levels` above 1, each side quotes that many orders of
`quantity`: the first where a single quote would be, each further one
`level_step_bps` of the mid (at least a tick) behind the one before. The
levels share `max_inventory`'s room, the touch's level first, so a deeper
level is quoted only while there is room left for it.

**Holding to the touch.** `touch` says how far into the spread a computed
price may go. `compute` quotes it; `join` holds a quote to the best price on
its side, so a bid computed inside the spread joins the best bid instead;
`improve` holds it to one tick better than that while the spread is at least
three ticks (else as `join`). A computed price behind the touch stays where it
is. Our own quote at the touch is the touch: it is not improved on, nor is the
last price quoted on that side, since a book delayed past a fill can still
show our quote there.

**Never crossing.** A bid is held to one tick under the best ask and an ask to
one tick over the best bid, and nothing is quoted while the book is crossed,
locked or missing a side. Such a quote pulls any resting quotes and, once a
good mid has been seen (or a position's mark resumed), still runs the
protection rules on that last good mid (using a one-sided book's live side for
`stop_loss` when it is the closing side). With `post_only`, a quote that would
still cross the book on arrival is refused by the venue instead of filling as a
taker, and the next quote tries again. Any other rejected or failed quote
placement (such as a `[risk]` limit or insufficient balance), or one the venue
refuses as soon as it arrives, pauses retries on
that level for 10 seconds, doubling on consecutive failures up to 5 minutes,
and resets once a placement succeeds or a fill arrives.

**Spot only.** The ask is quoted only while there is base asset to sell, and
the strategy starts from whatever the venue holds of the base asset. Past
`max_inventory` from where it started, the side that would add to the
position stops quoting. Nearer it, a quote is cut to the room left (a deeper
level first), and one cut under the venue's lot or minimum is not placed.

## Replacing quotes

Each replacement goes to the back of the queue at its price, so quotes are
replaced only when it is worth it:

- A resting quote is replaced once its target has moved `requote_bps` (and at
  least a tick).
- A market that has moved towards the quote (a bid whose target is now below
  it, an ask whose target is above it) is about to fill it at a stale price,
  and with order latency can fill it while its cancel is on the way. That is
  worth replacing sooner, at `requote_toward_bps`; 0 replaces it as soon as
  the target is a tick past it.
- `max_quote_age_ms` replaces a quote older than that even if its price is
  still right.
- With `keep_at_touch`, a quote still at the best price on its side is not
  replaced because its target moved away from it, so it keeps its place in
  the queue. On a pair whose spread is a tick or two wide, the place in the
  queue is most of what a quote earns. A move away is a target better than the
  quote (a higher bid); the skew or a widened spread moves the target the
  other way, towards the quote, and still replaces it. A market moving towards
  it, and the age and room rules, still replace it too.

**Order flow.** With `flow_pull_imbalance`, the quote recent trades run
against is pulled, so it is not left in the way of a sweep: no bid while the
trades of the last `flow_window_ms` were mostly sellers hitting the bid, no
ask while they were mostly buyers lifting the ask. The imbalance is
`(bought − sold) / (bought + sold)`, by size, over the trades the quotes
carried (a change in the last trade's price, size, side or time is a new
trade). The bid is pulled at `−flow_pull_imbalance` or below and the ask at
`+flow_pull_imbalance` or above, and each comes back once the imbalance is
inside that. A trade without a side or size is not counted.

**A quiet feed.** With `quiet_pull_ms`, both quotes are pulled once no quote
has arrived for that long, so nothing rests at a price the strategy can no
longer check (a stalled feed, a dropped connection); the next quote puts them
back. It is checked on a timer (`OnTimer`) every quarter of it, from 100 ms
to 1 s. The protection rules still run only when a quote arrives.

## Fills and races

- When a cancel loses a race with a fill, that side waits for the order's
  closing update or for its fills to cover the order before quoting again, so
  the inventory is never counted twice. If neither arrives within a minute
  (e.g. `GetOrder` answered `kNotFound` and the runner forgot the order), the
  quote is cleared so stops and quoting cannot stick, and any unseen remainder
  is handed to the unconfirmed-fill check below.
- Fills move the inventory (`OnFill`); order state comes from
  `OnOrderUpdate`, so a rejected or expired quote is quoted again, and a quote
  whose fills cover its whole size is cleared without waiting for the update.
- A fill the venue has reported in an order's state (a quote or an exit) but
  whose `OnFill` has not arrived counts against the room on its side and is
  excluded from a market exit on that side, so a quote that filled on arrival
  is not placed again and again before the inventory catches up, and a position
  is not exited twice. After a minute it is checked with the venue (an active
  exit whose closing update was missed is checked then too); still missing
  after about 11, it is booked without a fee, and a copy arriving later is
  skipped.
- An order the venue answers "not found" for (a quote or an exit) is settled
  from the base asset's balance after about 11 minutes: what the balance
  shows of it is booked, and the rest dropped. This assumes nothing else on
  the account trades the base asset. While other fills are outstanding, the
  balance settles an order only if it gives the same answer whether those
  filled or not; otherwise the order waits, as the balance cannot say which
  one moved it. An exit still waiting after 30 minutes is settled against
  what is held alone (taking the others as not filled), so it is booked or
  cleared and the stop can sell again; a lost sell on the same side may then
  be counted twice if its fill arrives after all. Any other lookup error (a
  timeout, a network error) settles nothing, as the exit may still be
  working.

## Protection rules

Every rule is off until its flag is set, and each has its own parameters, so a
run chooses which to use. The rules came out of the live and replayed
ZEC/USDT runs, where wins were a few basis points and a trending move cost
tens of them. Unlike the `[risk]` limits they are checked on every quote (using
the last good mid when a quote is crossed, locked or missing a side), and
the stop and cap rules close the position instead of only blocking entries.

"The position" is the base held beyond what the strategy started with, with
its average entry price worked out from the strategy's own fills; a rule never
sells the starting holding. "P&L" is the cash from the strategy's fills (fees
in the quote or base asset included; a fee in a third asset, such as BNB, is
not counted) plus that position marked at the mid,
so it is measured on every quote rather than when an order is attempted.

A rule that pauses a side cancels its resting quote. The exits are market
orders for the whole position, and no second exit is sent until the first
one's fills arrive.

| Rule | Parameters (defaults) | What it does |
|---|---|---|
| `stop_loss` | `stop_loss_bps` (40) | exit at market once the position is this far under its average entry (the bid for a long, the ask for a short), then cancel the quotes until it is closed. The live runs used 40; the replays found tighter stops only added stop-outs |
| `loss_cap` | `loss_cap_amount` (required, quote asset) | halt for good once the P&L since the start falls to `−loss_cap_amount`: cancel the quotes, exit the position and quote no more |
| `drawdown_cap` | `drawdown_cap_amount` (required, quote asset) | as `loss_cap`, for the fall from the P&L's peak |
| `spike_standdown` | `spike_3m_bps` (50), `spike_5m_bps` (80), `spike_15m_bps` (120), `spike_pause_ms` (900000) | cancel both quotes and hold off for the pause after the mid moves either way by at least that much in 3, 5 or 15 minutes. A move is measured against the newest mid at least that old, and counts as nil until that much history has been seen (the history is not kept across a restart). The pause starts again on every quote while the move is still past a threshold, so it ends `spike_pause_ms` after the move falls back |
| `downtrend_filter` | `downtrend_bps` (30) | no bid while the 15-minute move is at or below `−downtrend_bps` |
| `momentum_filter` | `momentum_bps` (0) | no bid while the 3-minute move is below `−momentum_bps` (0: any fall). The replays found this the one rule that helped on both tapes |
| `cooldown_after_stop` | `cooldown_ms` (1800000) | after a `stop_loss` exit, quote neither side for the cooldown. Does nothing without `stop_loss` |

**Kept state.** With `loss_cap`, `drawdown_cap`, `spike_standdown` or
`cooldown_after_stop` on, `run` keeps their state in the `[database]`
(`strategy_state`):

- what it keeps: whether it is halted, the P&L so far and its peak, when a
  stand-down or cooldown ends, and the position with its entry price;
- what a restart does with it: it carries on, so the caps count every run, not
  just the current one, the stop loss watches a position the last run left,
  and a halted maker stays halted (and exits such a position);
- `reset_state = true` forgets it and starts afresh (set it for one run, then
  take it out);
- a saved state that cannot be read stops the maker quoting.

With none of those four rules on (`stop_loss` alone, say), nothing is kept,
and a restart forgets the position the stop loss was watching. The mids the
spike, downtrend and momentum rules measure against are not kept either, so
after a restart they see no move until enough history has come in (up to 15
minutes).

Without a `[database]`, a live run is refused and a paper run warns and keeps
nothing; a backtest never keeps state. The state is per provider, mode and
symbol: run one maker per symbol on an account, or they overwrite each other's
state.

## How it differs from the research

| | Avellaneda and Stoikov | Kestrel |
|---|---|---|
| The lean | q·γ·σ²·(T − t): grows with volatility and the time left | `skew_bps` at `max_inventory`, in proportion for less: a fixed number of basis points, whatever the volatility. With `skew_power` above 1 it is not linear, and with `skew_age_ms` it grows with the position's age, both Kestrel's own |
| Inventory | counted from zero | counted from what the account held at the start |
| The spread | from γ, σ, k and the time left | `half_spread_bps`, a fixed number; with `vol_multiplier`, widened with the mid's typical one-minute move over `vol_window_ms`, which plays the part of σ |
| Horizon | a session ending at T, the lean and spread shrinking towards it (the paper also gives a stationary, infinite-horizon form) | none: quotes are set the same way all the time |
| Centre | the mid | the mid, or partway to the size-weighted mid (`microprice_weight`), which is not Stoikov's estimated micro-price |
| Fills | continuous quotes at any price, filled at random with the fill rate falling with distance | the venue's book: ticks, the place in the queue, latency, and quotes replaced only past `requote_bps`; held to the touch with `touch`, never crossing |
| Limits | none: any inventory | `max_inventory` stops the side that adds, and on spot there is no ask without coins to sell |
| Protection | none | the opt-in rules above (stop loss, loss and drawdown caps, stand-downs, filters, the flow pull) |

The paper tests its model in simulation, not on market data. Kestrel's
Nov 2026 replays found no setting that beat holding ZEC/USDT (see
[strategy-tests.md](../strategy-tests.md#market-maker-zecusdt)).

## Parameters

In the `[strategy]` table (see [configuration.md](../configuration.md#strategy)).

| Key | Meaning |
|---|---|
| `symbol`, `quantity` | required; `quantity` is base units per quote |
| `half_spread_bps` | required; distance of each quote from the reservation price |
| `vol_multiplier` | widen the spread with volatility (default 0: off); see [How it quotes](#how-it-quotes) |
| `vol_window_ms` | how far back that volatility is measured (default 300000) |
| `max_half_spread_bps` | cap on a widened half-spread (0, the default: none); at least `half_spread_bps` |
| `microprice_weight` | centre the quotes this far (0 to 1) from the mid towards the microprice (default 0: the mid) |
| `flow_pull_imbalance` | pull the quote recent trades run against; 0 to 1, where 1 pulls only when every trade went one way (default 0: off); see [Replacing quotes](#replacing-quotes) |
| `flow_window_ms` | how far back those trades count (default 5000) |
| `max_inventory` | required; how far the position may stray from where it started, either way; at least `quantity` |
| `skew_bps` | how far the reservation price leans when the position is `max_inventory` away (default 0); with `skew_age_ms`, up to twice this. A skew larger than the half-spread puts the quote that reduces the position past the mid, so it can sell (or buy back) at a loss against the entry before `stop_loss` fires |
| `skew_power` | the lean's power, 1 to 4 (default 1: linear) |
| `skew_age_ms` | grow the skew with the position's age, to double at this age (default 0: off) |
| `requote_bps` | how far the target must move before a resting quote is replaced (default 1) |
| `requote_toward_bps` | the same when the market has moved towards the quote; 0 replaces it on a tick's move (default: `requote_bps`) |
| `max_quote_age_ms` | replace a quote older than this even if its price is right (0, the default: never) |
| `post_only` | quote with post-only orders (default `false`). Recommended live, and in backtests meant to predict a live run, where without it a quote the book moved through while in flight fills as a taker; on in the example configuration |
| `touch` | `compute` (default), `join` or `improve`; see [How it quotes](#how-it-quotes) |
| `levels`, `level_step_bps` | levels a side (1 to 10; default 1) and the step between them (default: the half-spread quoted, widened by `vol_multiplier` when that is on). Each level is its own order, so the order traffic grows with them (three levels sent three times the orders in the replays): allow for it in `max_orders_per_minute` and the venue's rate limit |
| `quiet_pull_ms` | pull both quotes once no quote has arrived for this long (default 0: never). Binance.US's book stream sends nothing while the book does not change, so a quiet market is silent for seconds at a time (ZEC/USDT: over 10 s 2,695 times in four days, over 60 s 8 times): use 30000 to 60000, as 2000 to 5000 pulls thousands of times and lost money in the replays |
| `keep_at_touch` | keep a resting quote that is still at the best price on its side when its target moves away (default `false`). It matters on pairs whose tick is a large part of the spread; with `touch = "join"` a target is never better than the touch, so it adds nothing there |
| `reset_state` | forget the kept state and start afresh (default `false`); set it for one run, then take it out (see [Kept state](#protection-rules)) |
| protection rules | see [Protection rules](#protection-rules) |

`quantity` and `half_spread_bps` must be above zero. `skew_bps`,
`requote_bps`, `requote_toward_bps`, `max_quote_age_ms`, `quiet_pull_ms`,
`skew_age_ms`, `level_step_bps`, `max_half_spread_bps` and `vol_multiplier`
cannot be negative; `vol_window_ms` must be positive with
`vol_multiplier` set, and `flow_window_ms` with `flow_pull_imbalance` set. An
enabled protection rule's thresholds and amounts must be positive;
`momentum_bps`, `spike_pause_ms` and `cooldown_ms` may be zero.
