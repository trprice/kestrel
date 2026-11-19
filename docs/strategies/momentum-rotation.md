# Momentum rotation

`momentum_rotation` is cross-sectional momentum, long only. It ranks a
universe of symbols by their recent return and holds the strongest, equally
weighted, rebalancing on a schedule. Momentum is the anomaly that holds up
best in crypto research (Yang, "Behavioral Anomalies in Cryptocurrency
Markets"; Quantpedia's crypto summaries).

Source: `include/kestrel/strategy/momentum_rotation.hpp`,
`src/strategy/momentum_rotation.cpp`. Results are in
[strategy-tests.md](../strategy-tests.md#momentum-rotation).

## How it works

The universe is every symbol in `[general] symbols`: two or more, none listed
twice, all with one quote asset (a symbol without one is refused), which is
the cash.

Every `rebalance_bars` bars it ranks the symbols by their return over
`lookback_bars` and holds the strongest `top_n`, each with an equal share of
`invest_fraction` of equity (cash plus its holdings at the last close). With
`positive_only` (absolute momentum), only symbols that rose are held, so in a
falling market it sits in cash. The rest of equity is room for fees and moves
between the sizing and the fill.

**Ranking.** Bars of the symbols arrive one at a time. A period is ranked once
every symbol's bar for it has closed, or, if one never comes, when a bar of
the next period arrives. The lookback is measured in time: a symbol without a
bar for the ranked period, or for `lookback_bars` before it, sits that ranking
out, and if held it is kept (with its slot) rather than sold. With fewer than
half the symbols ranked there is no rebalance.

**Rebalancing.** A rebalance sells what dropped out of the top at market
first. Once those sells have filled and their fills have been seen (or the
sells closed), it buys what entered the top at market into the slots left,
sized from equity then and capped by the cash available. Symbols still in the
top are kept as they are, not resized. An order counts as out until its
fills have been seen, even one the venue reports filled at once. Orders
still out at a rebalance are checked with the venue; if one is still open,
that rebalance is skipped, and the last one's buys still go once its sells
fill.
While the market is closed (daily stock bars), a rebalance waits for the next
session's open.

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

## Parameters

In the `[strategy]` table (see [configuration.md](../configuration.md#strategy)).

| Key | Meaning |
|---|---|
| `timeframe` | the bars it ranks on (default `1d`); not a calendar timeframe (a week or a month) |
| `lookback_bars` | momentum: the return over this many bars (default 28) |
| `rebalance_bars` | rebalance every this many bars, starting with the first (default 7) |
| `top_n` | how many symbols to hold (default 3); at most the number of symbols |
| `positive_only` | hold only symbols whose momentum is above zero, else cash (default `true`) |
| `invest_fraction` | share of equity invested, split over the `top_n` slots; the rest covers fees and the move to the fill (default 0.95; above 0, at most 1) |
| `adopt_holdings` | count the coins of the universe the account holds at startup as held, to keep or sell (default `false`); see [What it holds](#how-it-works) |
