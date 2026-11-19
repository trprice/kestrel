# Momentum rotation

`momentum_rotation` is cross-sectional momentum, long only. It ranks a
universe of symbols by their recent return and holds the strongest, equally
weighted, rebalancing on a schedule. Momentum is the anomaly that holds up
best in crypto research; Kestrel's version follows the weekly momentum
portfolios of Liu, Tsyvinski and Wu, "Common Risk Factors in
Cryptocurrency" (Journal of Finance, 2022;
[NBER w25882](https://www.nber.org/papers/w25882),
[doi](https://doi.org/10.1111/jofi.13119)).

Source: `include/kestrel/strategy/momentum_rotation.hpp`,
`src/strategy/momentum_rotation.cpp`. Results are in
[strategy-tests.md](../strategy-tests.md#momentum-rotation).

## The strategy in the research

Cross-sectional momentum buys what has risen most over a recent window and,
in its classic form, sells short what has risen least. It was first shown in
US stocks by Jegadeesh and Titman, "Returns to Buying Winners and Selling
Losers" (Journal of Finance, 1993;
[doi](https://doi.org/10.1111/j.1540-6261.1993.tb04702.x)).

**In crypto** (Liu, Tsyvinski and Wu):
- *Universe:* every coin with a market capitalization above $1 million,
  from 2014 to 2018 (109 coins at the start, 1,583 at the end).
- *Portfolios:* each week the coins are sorted into fifths by their return
  over the past one, two, three or four weeks. Each fifth is held,
  value-weighted, for the week that follows, then sorted again.
- *Results:* long the top fifth and short the bottom one made 2.5% to 4.1% a
  week before costs (4.1% on three weeks' return, 2.5% on four). Shorting
  Bitcoin instead of the bottom fifth gave about the same. It worked among
  the larger half of the coins (4.2% a week) but not the smaller (0.6%, not
  significant).

Yang, "Behavioral Anomalies in Cryptocurrency Markets"
([SSRN 3174421](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=3174421);
first titled "Momentum and Reversal in Cryptocurrency Markets"), tests more
than 20 stock-market anomalies on daily coin data. Price momentum is the one
that stays significant once the market and size are allowed for.

## How it works

The universe is every symbol in `[general] symbols`: two or more, none listed
twice, all with one quote asset (a symbol without one is refused), which is
the cash.

Every `rebalance_bars` bars it ranks the symbols by their return over
`lookback_bars` and holds the strongest `top_n`, each with `invest_fraction`
of equity divided by `top_n`. Equity is the account's whole quote balance
plus the strategy's holdings at the last close, so run it on an account of
its own. With
`positive_only` (absolute momentum), only symbols that rose are held, so in a
falling market it sits in cash. The rest of equity is room for fees and moves
between the sizing and the fill.

**Ranking.** Bars of the symbols arrive one at a time. A period is ranked once
every symbol's bar for it has closed, or, if one never comes, when a bar of
the next period arrives. A symbol that missed a period (halted, or not listed
yet) is not waited for again until it sends a bar, so it holds up only the
first period it misses. The lookback is measured in time: a symbol without a
bar for the ranked period, or for exactly `lookback_bars` periods of time
before it, sits that ranking out, and if held it is kept (with its slot)
rather than sold. With fewer than half the symbols ranked there is no
rebalance. On stock daily bars, which skip weekends and holidays, a lookback
that lands on a day without bars leaves every symbol unranked, so that
rebalance does not happen: keep `lookback_bars` a multiple of 7 there, and
expect holidays to skip one (see TODO.md).

**Rebalancing.** A rebalance sells what dropped out of the top at market
first. Once those sells have filled and their fills have been seen (or the
sells closed), it buys what entered the top at market into the slots left,
sized from equity then and capped by `invest_fraction` of the free cash.
Symbols still in the top are kept as they are, not resized. An order counts as
out until its fills have been seen, even one the venue reports filled at once.
Orders still out at a rebalance are checked with the venue; if one is still
open, that rebalance is skipped, and the last one's buys still go once its
sells fill. While the market is closed (daily stock bars), a rebalance waits
for the next session's open.

**What it holds.** What the strategy bought counts as held, from its fills,
less fees taken in the coin; leftovers under the minimum order count as
nothing held. By default it does not count the account's other balances: it
warns at startup about coins of its universe the account already holds, which
it neither counts nor sells, so a restart forgets what the last run bought.

With `adopt_holdings`, those coins count as held from startup (the part not
locked in an order), so a restarted run picks up where the last one left off:
at the next rebalance a coin still in the top keeps its slot as it is, and one
ranked out is sold. It cannot tell its coins from anyone else's, so run it on
an account (or sub-account) of its own, the only place it should run live. A
restart also starts the `rebalance_bars` count again, so the first ranking
after it rebalances.

## How it differs from the research

| | The research | Kestrel |
|---|---|---|
| Sides | long the top fifth, short the bottom fifth (or Bitcoin) | long only, as spot accounts cannot short: the top `top_n` (3 by default), and nothing short |
| Universe | every coin above $1 million of market capitalization, re-formed each week | the fixed `[general] symbols`, chosen by hand, so with hindsight; a handful of coins, so the top 3 is far more concentrated than a fifth of hundreds |
| Lookback and rebalance | one to four weeks' return, sorted every week | `lookback_bars` (28 days, the paper's four weeks) every `rebalance_bars` (7: weekly) |
| Weights | value-weighted within the fifth, set again every week | an equal share of `invest_fraction` of equity, set when a coin is bought and capped by the free cash, so a slot can be smaller; a coin that stays in the top is not resized, so the weights drift. A held coin that cannot be ranked keeps its slot, and with `positive_only` an unfilled slot stays in cash |
| Absolute momentum | none: the top fifth is held whatever the market does | `positive_only` (on by default) holds only coins that rose over the lookback, else cash: the absolute-momentum filter of Antonacci's dual momentum ("Risk Premia Harvesting Through Dual Momentum", 2012; [doi](https://doi.org/10.2139/ssrn.2042750)) |
| Costs and execution | before costs | market orders after the period's close, filled at the next price with fees, spread and slippage; sells first, buys once their fills are in |

The paper's returns are gross, long-short, from 2014–2018 and over hundreds
of coins. Kestrel's backtest on 11 coins in 2022–2026 beat holding Bitcoin,
but most of the profit came from one coin, ZEC (see
[strategy-tests.md](../strategy-tests.md#momentum-rotation)).

## Parameters

In the `[strategy]` table (see [configuration.md](../configuration.md#strategy)).

| Key | Meaning |
|---|---|
| `timeframe` | the bars it ranks on (default `1d`); not a calendar timeframe (a week or a month) |
| `lookback_bars` | momentum: the return since the bar this many periods of `timeframe` earlier, found by time (default 28) |
| `rebalance_bars` | rebalance every this many bars, starting with the first (default 7) |
| `top_n` | how many symbols to hold (default 3); at most the number of symbols |
| `positive_only` | hold only symbols whose momentum is above zero, else cash (default `true`) |
| `invest_fraction` | share of equity invested, split over the `top_n` slots; the rest covers fees and the move to the fill (default 0.95; above 0, at most 1) |
| `adopt_holdings` | count the coins of the universe the account holds at startup as held, to keep or sell (default `false`); see [What it holds](#how-it-works) |
