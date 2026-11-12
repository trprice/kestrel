# Moving-average crossover

`ma_crossover` (called `sma_crossover` before Nov 2026) is a long-only
moving-average crossover on closed bars. It is a worked example of the
`Strategy` interface (see [writing-strategies.md](../writing-strategies.md)),
not a strategy to trade.

Source: `include/kestrel/strategy/ma_crossover.hpp`,
`src/strategy/ma_crossover.cpp`.

## How it works

It buys at market when the fast average crosses above the slow one, and sells
at market when it crosses back below. The averages are simple, or with
`average = "ema"` exponential, which turn sooner. They are computed over a
window of four slow periods of bars, so an exponential average has settled
from its seed.

Its orders go through `SingleOrderPosition` (see
[writing-strategies.md](../writing-strategies.md)). It goes long or flat when
the order's fills arrive (`OnFill`), and places no new order while one is
working: an order filled in parts is waited on until its last part, and one
the venue cancelled, rejected or expired (`OnOrderUpdate`) until the fills of
what it filled. The position is not kept across a restart: a run started
while holding begins flat, and does not sell what it holds.

## Parameters

In the `[strategy]` table (see [configuration.md](../configuration.md#strategy)).

| Key | Meaning |
|---|---|
| `symbol`, `quantity` | required; `quantity` is base units per order, above zero |
| `timeframe` | the bars it trades (default `1h`) |
| `fast_period`, `slow_period` | the averages' periods (defaults 10 and 30); the fast must be shorter |
| `average` | `sma` (default) or `ema` |
