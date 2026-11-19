# Mean reversion

`mean_reversion` is a long-only RSI mean reversion on closed bars: it buys
when the Relative Strength Index falls to `oversold` and sells on the
rebound. With its defaults it is Wilder's RSI(14) at 30 and 70; with
`rsi_period = 2`, `oversold = 10`, `overbought = 90`, `trend_sma = 200` and
`exit_sma = 5` it is close to Larry Connors's RSI(2) pullback
([StockCharts: RSI(2)](https://chartschool.stockcharts.com/table-of-contents/trading-strategies-and-models/trading-strategies/rsi-2)).

Source: `include/kestrel/strategy/mean_reversion.hpp`,
`src/strategy/mean_reversion.cpp`.

```
kestrel_app run mean_reversion
kestrel_app backtest mean_reversion --from 2025-01-01 --to 2026-11-01
```

## The strategy in the sources

**The RSI** is J. Welles Wilder's, from *New Concepts in Technical Trading
Systems* (1978; described in
[StockCharts: RSI](https://chartschool.stockcharts.com/table-of-contents/technical-indicators-and-overlays/technical-indicators/relative-strength-index-rsi)):
- *Averages:* the average gain and the average loss of the last 14 closes'
  changes. The first is a simple average. Each after it is smoothed: 13
  parts the last average to one part the newest change.
- *The index:* RSI = 100 − 100 / (1 + average gain / average loss).
- *Signals:* above 70 is overbought, below 30 oversold. Wilder read these
  with failure swings and divergences between the RSI and the price, as
  warnings of a turn rather than orders to trade.

**Bollinger Bands** are John Bollinger's
([bollingerbands.com](https://www.bollingerbands.com/bollinger-bands)): a
20-period simple average with bands two standard deviations (of the
population) above and below it. He pairs a touch of a band with a
confirming indicator rather than trading the touch alone.

**Connors's RSI(2)** is from Connors and Alvarez, *Short Term Trading
Strategies That Work* (2008), as summarized by StockCharts:
- *Trend:* buy only above the 200-day simple average (and short only below
  it).
- *Entry:* buy when the 2-period RSI dips low. Connors found a dip under 5
  did better than one under 10.
- *Timing:* buy just before the close; the next open also works, though
  Connors prefers the close.
- *Exit:* sell on a move above the 5-day simple average.
- *Stops:* none. His tests found stops hurt on stocks and stock indices.

## How it works

On each closed bar, while flat, it buys `quantity` at market when the RSI is
at or below `oversold` and, if they are set:
- the close is at or below the lower Bollinger band (`bollinger_period`);
- the close is above the `trend_sma` average.

While long, it sells everything at market at the first of:
- the RSI at or above `overbought`;
- the close at or above the `exit_sma` average;
- the close at or below the stop, `atr_stop` ATRs under the average price
  paid (the ATR of the bar that signalled the entry);
- `max_hold_bars` bars held;
- the session closing, with `flatten_at_close`.

The stop is checked at each bar's close: no stop order rests at the venue,
so a fall inside a bar is not caught until it closes, and the sell is at the
next price, which can be well under the stop. `flatten_at_close` does nothing
on a crypto venue, which has no session close.

On a market that closes, an order due while it is closed is sent at the next
open. An entry goes only if the signal still stands on the last bar before
the open; an exit always goes.

Its orders go through `SingleOrderPosition` (see
[writing-strategies.md](../writing-strategies.md)): one order at a time, the
position counted from the fills, an order filled in parts waited on until
its last part, a buy's fee in the coin off what is held, and the sell rounded
down to the lot. The position is not kept across a restart: a run started
while holding begins flat, and does not sell what it holds.

## How it differs from the sources

| | The sources | Kestrel |
|---|---|---|
| RSI and bands | Wilder's smoothing; Bollinger's population deviation | the same: the RSI is seeded with a simple average and smoothed by 1/`rsi_period`, the bands use the population deviation |
| What the RSI means | Wilder: 70 and 30 as warnings, with failure swings and divergences | the level alone trades; no failure swings or divergences |
| Entry timing | Connors buys just before the close, on that day's signal | the signal is the bar's close and the order goes after it, at the next price: on daily stock bars the next open, so the overnight gap is the strategy's, not the source's |
| Entry level | under 10, better under 5 | at or under `oversold`; the example uses 10 |
| Exits | Connors: the 5-day average only, no stop | the 5-day average, and always an RSI at `overbought` too: it cannot be turned off (it must be under 100), and at the default of 70 an RSI(2) can sell before the average is reached, so the example sets 90. Optional ones of Kestrel's: an ATR stop checked at closes, `max_hold_bars` and the session's close (see TODO.md) |
| Shorts | Connors shorts the mirror image below the 200-day average | long only |
| Size | not set by the sources | a fixed `quantity` a trade |
| Restarts | not covered by the sources | a run started while holding begins flat |

Kestrel's backtests found Connors's RSI(2) profitable on SPY and QQQ,
though, in the market 11–16% of days, it made far less than holding them
([Mean reversion on stocks](../strategy-tests.md#mean-reversion-on-stocks)).
On 35 stocks it came closest of the daily setups to beating the drift of
holding the same stock over the same days, but not with confidence (t = 1.9;
[Daily stock setups against holding](../strategy-tests.md#daily-stock-setups-against-holding)).

## Parameters

In the `[strategy]` table (see [configuration.md](../configuration.md#strategy)).

| Key | Meaning |
|---|---|
| `symbol`, `quantity` | required; `quantity` is base units per order, above zero |
| `timeframe` | the bars it trades (default `1h`) |
| `rsi_period` | Wilder RSI period (default 14; above zero) |
| `oversold`, `overbought` | the RSI to buy at or under, and to sell at or over (defaults 30 and 70; `0 < oversold < overbought < 100`) |
| `bollinger_period` | the band's period; 0 (default) for no band filter, else at least 2 |
| `bollinger_width` | the band's width in standard deviations (default 2; above zero) |
| `exit_sma` | sell when the close reaches this average; 0 (default) for none, else at least 2 |
| `trend_sma` | buy only above this average; 0 (default) for none, else at least 2 |
| `atr_stop` | the stop's distance under the price paid, in ATRs; 0 (default) for none |
| `atr_bars` | the ATR's period (default 14) |
| `max_hold_bars` | sell after this many bars held; 0 (default) for no limit. A bar that closes while an order is working is not counted |
| `flatten_at_close` | sell before the session closes (default `false`) |

The warmup is six RSI periods and a bar (and six ATR periods and a bar with a
stop), so Wilder's smoothing has settled, or the longest average if that is
longer.

### Example: RSI(14) under the lower band, hourly

```toml
[general]
provider = "binanceus"
mode     = "paper"
symbols  = ["BTC/USDT"]

[strategy]
symbol           = "BTC/USDT"
quantity         = "0.01"
timeframe        = "1h"
bollinger_period = 20
exit_sma         = 20
atr_stop         = 2.5
```

### Example: Connors RSI(2) on a stock, daily

```toml
[general]
provider = "schwab"
mode     = "paper"
symbols  = ["SPY"]

[strategy]
symbol        = "SPY"
quantity      = "10"
timeframe     = "1d"
rsi_period    = 2
oversold      = 10
overbought    = 90
trend_sma     = 200
exit_sma      = 5
max_hold_bars = 10
```

A backtest on BTC/USDT hourly from January 2025 to November 2026 with the
first example's settings (at 0.0131 BTC) made 155 round trips, 52% of them
winners, for −2.27% of a 10,000 USDT account (worst drawdown −4.39%).
