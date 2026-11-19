# Trend ensemble

`trend_ensemble` is time-series trend following on a universe of coins, long
only. Each coin runs several Donchian channel breakout models of different
lengths; the share of them in a trend sets how much of the coin is held, and
each is sized to a target volatility. It is Kestrel's version of the
ensemble in Zarattini, Pagani and Barbon, "Catching Crypto Trends: A
Tactical Approach for Bitcoin and Altcoins" (Swiss Finance Institute
Research Paper 25-80, 2025;
[SSRN 5209907](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=5209907),
[PDF](https://concretumgroup.com/wp-content/uploads/2026/02/Catching-Crypto-Trends.pdf)).

Source: `include/kestrel/strategy/trend_ensemble.hpp`,
`src/strategy/trend_ensemble.cpp`. Results are in
[strategy-tests.md](../strategy-tests.md#trend-ensemble-in-kestrel).

## The strategy in the paper

The paper builds on time-series momentum: an asset that has been rising tends
to keep rising for a while, and trend following holds it while it does and
steps aside when it stops (Moskowitz, Ooi and Pedersen, "Time Series
Momentum", Journal of Financial Economics, 2012). Its trend signal is the
Donchian channel, the highest and lowest close of the last n days.

**One model** (Section 4.1). For a lookback of n days:
- *Entry:* long when the day's close is the highest close of the last n
  days, today's included.
- *Exit:* a trailing stop, set at entry to the channel's midpoint (halfway
  between the highest and lowest close), and raised after each day's close
  to that day's midpoint if that is higher; it is never lowered. The model
  goes flat on a close at or below the stop.

**Sizing** (Section 4.2). A model that is long holds 25% / σ of its capital,
where σ is the asset's annualized volatility over the last 90 days, capped
at 200% (so up to 2x leverage). A volatile coin gets a small position, a
calm one a large one, so each position carries about 25% a year of risk.

**The ensemble** (Section 4.3). Nine models, of 5, 10, 20, 30, 60, 90, 150,
250 and 360 days, each sized as above. The "Combo" weight is their average,
so a coin with all nine long holds its full volatility-target weight, one
with three long a third of it. No single lookback has to be the right one.

**Costs** (Section 5.1). Weights are set at each day's close and held for the
next day. Trades cost 10 bps. A position is resized for a change in
volatility only when it is more than 20% off its target; a change of signal
(an entry or a stop) trades at once.

**The portfolio** (Section 7). Each month the universe is the top coins by
median daily volume (not stablecoins or wrapped tokens; at least $2 million
a day), each given an equal share of the capital, within which the ensemble
runs on its own; a coin whose volume falls under $1 million a day, or that
barely moves, is dropped. From January 2015 to March 2025, net of costs, the
20-coin portfolio returned about 18% a year with a Sharpe ratio of 1.57,
9% volatility and an 11% worst fall. On Bitcoin alone the ensemble had a
Sharpe ratio of 1.58 and a 19% worst fall, against more than 80% for holding
it. Those years include three crypto bull markets, and the paper notes its
coin lists are chosen with hindsight.

## How Kestrel runs it

The universe is every symbol in `[general] symbols`: one or more, none
listed twice, all with one quote asset (the cash).

**The models.** For each symbol and each of `lookbacks`, a model follows the
paper's rules on the closes of `timeframe` bars: long on a close at the
channel's highest close (a flat window's close counts, as in the paper),
flat on a close at or below its trailing stop. The new-high check comes
first, so a close that is both a new high and at the stop stays long.

**Sizing.** A symbol's target, as a fraction of equity (cash plus holdings at
the last close), is

    (models long / models) x min(target_vol / volatility, max_weight)
        x invest_fraction / symbols

where volatility is the sample standard deviation of the last `vol_bars`
close-to-close returns, annualized with the bars in 365 days (crypto trades
every day). Each symbol has an equal share of `invest_fraction` of equity.

**Rebalancing.** Once every symbol's bar for a period has closed (or a bar of
the next arrives), each symbol is compared with its target. A symbol that
missed a period, halted or not listed yet, is not waited for again until it
sends a bar, so it holds up only the first period it misses.
- *Signal change:* if the number of its models long has changed since an
  order last took it toward its target, it trades to its target. It is the
  number that counts: one model going long as another goes flat leaves the
  weight as it was. A signal that could not be traded (an order too small,
  or refused) is tried again at the next rebalance.
- *Drift:* otherwise it trades only if it is more than
  `rebalance_threshold` of the target away from it.
- *Out of the trend:* a symbol with no model long and something held is sold.

Sells go first, at market. Once their fills have been seen (or the sells
closed), the buys go, sized from the targets set at the rebalance and capped
by 99% of the free cash. Orders under the venue's minimum size or notional
are skipped; a position already within the threshold of its target then
counts as traded. An order counts as out until its fills have been seen,
even one the venue reports filled at once (a market order on the paper
venue or Binance.US), as the holdings are counted from the fills. Orders
still out at a rebalance are checked with the venue; if one is still open,
the rebalance waits and runs once they have closed, and buys queued by an
earlier rebalance are dropped. A symbol
without a bar for the period, or without `vol_bars` + 1 bars yet, is left
as it is.

**Halts and relistings.** Bars more than 7 periods and more than a week
apart are a halt or a delisting: the symbol's models start again after the
gap, and its volatility counts only the bars after it. Binance.US had no
XRP/USDT from January 2021 to July 2023; without this, its first bar back
would have been measured against closes two and a half years old. A bar or
two missing is not a gap.

**Restarts.** The models replay the warm-up bars at startup: three times the
longest lookback (1,081 daily bars by default). A model of n bars can only
start once it has n closes, so it replays the last 1,082 − n of them (722
for the 360-day model, 1,077 for the 5-day one): its trailing stop agrees
with a run that never stopped unless it has been long for longer than that.
With `adopt_holdings`, the coins the account holds count as held and are
traded to their targets at the first rebalance; without it, they are
neither counted nor sold (see `momentum_rotation`'s
[What it holds](momentum-rotation.md#how-it-works), which works the same).

## How it differs from the paper

| | The paper | Kestrel |
|---|---|---|
| Leverage | up to 200% of a coin's share | none: `max_weight` is at most 1 (spot). It binds only while a coin's volatility is under `target_vol`, so it matters little at 25% |
| Universe | the top 20 by volume, re-chosen monthly, with liquidity exits | the fixed `[general] symbols`, chosen by hand. A symbol without bars leaves its share in cash: LINK, NEAR and ZEC before January 2022, and XRP/USDT, delisted from January 2021 to July 2023 |
| Capital per coin | an equal share at each month's start, drifting with each coin's results within the month | an equal share of current equity at every rebalance, resized past the threshold, less 5% (`invest_fraction`) kept for fees and moves before the fill |
| Threshold | "more than 20%" between current and target allocation, for volatility-driven changes | read as 20% of the target, and applied to any drift without a signal change (a price move as well as a volatility change) |
| Execution | at the daily close, 10 bps a trade | market orders after the 00:00 UTC daily close, filled at the next price: on Binance.US 2 bps taker fee, plus the spread and slippage (5 bps assumed in backtests) |
| Volatility | "90-day annualized volatility of returns" | the sample deviation of 90 daily simple returns, times the square root of 365 |
| History | from each coin's first day | the warm-up window at startup (1,081 days), and only since a gap of more than 7 bars and a week; a backtest starting soon after a listing has less |
| Equity | the strategy's own capital | the whole quote balance plus the coins it holds, as `momentum_rotation`: run it on an account of its own |
| Holdings at the start | none | without `adopt_holdings`, coins the account already holds are ignored, so the first rebalance buys full targets on top of them |
| Order sizes | exact weights | quantities are rounded down to the venue's lot, a buy is capped at 99% of the free cash, and orders under the venue's minimums (1 USDT notional on Binance.US) are skipped, so a small account trades less than the model asks |

The paper's results come from a decade with three crypto bull markets and a
universe chosen with hindsight; Kestrel's universe is chosen with hindsight
too. Expect less: in Kestrel's backtest of 2022–2026 it made 6.7% a year,
about a third of the paper's, at a Sharpe ratio near the paper's in
2022–2024 (1.26) and a far lower one in 2025–2026 (0.26; see
[strategy-tests.md](../strategy-tests.md#trend-ensemble-in-kestrel)).
On Bitcoin alone in the paper's period, on Kraken's prices, the rules
match the paper's risk-adjusted results (a Sharpe ratio of 1.53 at 10 bps
against the paper's 1.56, a 19.5% worst fall against 19%), and Kestrel's
backtest matches those rules exactly (from Sep 2016, where Kraken's data
first covers its warm-up). The shortfall is the later, weaker period, not
the implementation (see
[Checked against the paper](../strategy-tests.md#checked-against-the-paper-btc-on-kraken-20152025)).

## Parameters

In the `[strategy]` table (see [configuration.md](../configuration.md#strategy)).
The defaults are the paper's, except where spot trading rules them out.

| Key | Meaning |
|---|---|
| `timeframe` | the bars the models run on (default `1d`); not a calendar timeframe (a month) |
| `lookbacks` | the channel lengths in bars, one model each, as an array (default `[5, 10, 20, 30, 60, 90, 150, 250, 360]`); each at least 2 |
| `vol_bars` | the returns the volatility is measured over (default 90; at least 2) |
| `target_vol` | the annualized volatility each model is sized to (default 0.25) |
| `max_weight` | the most of a symbol's share a model may hold (default 1; above 0, at most 1: no leverage) |
| `rebalance_threshold` | resize a holding whose signal has not changed only when it is more than this fraction of its target away from it (default 0.2; 0 resizes at every rebalance) |
| `invest_fraction` | share of equity the symbols split equally; the rest covers fees and the move to the fill (default 0.95; above 0, at most 1) |
| `adopt_holdings` | count the coins of the universe the account holds at startup as held (default `false`); for an account of its own |

At the default `target_vol` the strategy holds little: a coin at 70%
volatility with every model long holds about a third of its share, so even
the whole universe in a trend is about a third of equity. In the 2022–2026
backtest it averaged 10% of equity in coins, and at most 40%. On a small
account that means small orders: with 1,000 USDT over 11 coins, one model
changing moves about 3–4 USDT of a coin, above Binance.US's 1 USDT minimum
but not by much.
