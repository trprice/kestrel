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
`average = "ema"` exponential, which turn sooner. The warmup is the slow
period and a bar for simple averages, and four slow periods and a bar for
exponential ones, so the exponential average has settled from its seed.

Its orders go through `SingleOrderPosition` (see
[writing-strategies.md](../writing-strategies.md)). It goes long or flat when
the order's fills arrive (`OnFill`), and places no new order while one is
working: an order filled in parts is waited on until its last part, and one
the venue cancelled, rejected or expired (`OnOrderUpdate`) until the fills of
what it filled. The position is not kept across a restart: a run started
while holding begins flat, and does not sell what it holds.

## The rule in the research

The moving-average rule is one of the oldest in technical analysis. Its best
known test is Brock, Lakonishok and LeBaron, "Simple Technical Trading Rules
and the Stochastic Properties of Stock Returns" (Journal of Finance, 1992;
[doi](https://doi.org/10.1111/j.1540-6261.1992.tb04681.x)):
- *Data:* the Dow Jones Industrial Average, daily, 1897 to 1986.
- *Rule:* a buy signal while a fast average of the close is above a slow
  one, a sell signal while it is below. The fast average may be the close
  itself, and a band can be required (the fast average 1% beyond the slow).
  In the "variable length" form, a position is held as long as its signal
  stands; in the "fixed length" form, for a set number of days after a
  cross.
- *Results:* days after buy signals returned more than days after sell
  signals, and every rule they tested beat holding cash.

Sullivan, Timmermann and White, "Data-Snooping, Technical Trading Rule
Performance, and the Bootstrap" (Journal of Finance, 1999;
[doi](https://doi.org/10.1111/0022-1082.00163),
[PDF](https://bashtage.github.io/kevinsheppard.com/files/teaching/mfe/advanced-econometrics/Sullivan_Timmermann_White.pdf)),
tested nearly 8,000 rules, these among them, allowing for the chance of
finding a winner among so many. Over 1897 to 1986 some rules still beat
the benchmark. Over the next ten years, 1987 to 1996, there was scant
evidence that any of them, even the best, was of value, and on S&P 500
futures, from 1984, none beat the benchmark.

## How it differs from the research

| | The research | Kestrel |
|---|---|---|
| Holding | variable length: in the market while the fast average is above the slow | it buys and sells only on the bar of a cross. A cross that comes while an order is working, or an order the venue refuses, is not tried again until the averages cross back and over again: a missed buy can sit flat through a whole up-trend, a missed sell stay long through a down-trend (see TODO.md) |
| Sides | the returns after buy signals against those after sell signals | long or flat: a sell signal sells what it bought |
| Band | none, or the fast average 1% beyond the slow | none: any cross trades |
| Averages | simple, on daily closes | simple or exponential, on any `timeframe` (default `1h`, with 10 and 30 bars) |
| Size | not set by the research | a fixed `quantity` a trade |

## Parameters

In the `[strategy]` table (see [configuration.md](../configuration.md#strategy)).

| Key | Meaning |
|---|---|
| `symbol`, `quantity` | required; `quantity` is base units per order, above zero |
| `timeframe` | the bars it trades (default `1h`) |
| `fast_period`, `slow_period` | the averages' periods (defaults 10 and 30); the fast must be shorter |
| `average` | `sma` (default) or `ema` |
