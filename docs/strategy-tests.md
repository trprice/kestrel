# Strategy tests

The backtests and paper runs done so far (Oct to Nov 2026), and what each
found. These are research notes, not tuned results: most tests tried
several settings and report the best, so treat each edge as an upper bound.
[Upcoming tests](#upcoming-tests) come first: what is planned next,
with its settings and what counts as passing fixed before it is run.

Unless a test says otherwise:
- **Crypto** is Binance.US, at its 2 bps taker and 0 maker fee, with 5 bps
  of slippage a side. That makes a round trip with market orders about
  14 bps. "The seven pairs" are BTC/USDT, SOL/USDT, ZEC/USDT, ETH/USD,
  ADA/USD, XRP/USD and HYPE/USD.
- **Stocks** use Schwab bars, with no commission, and 5 bps of slippage or
  each stock's estimated spread. Schwab keeps about 48 days of 1-minute
  bars and about 260 of 30-minute ones, so intraday stock tests cover
  weeks, not years.
  Daily strategies fill at the open, when spreads are 2–3 times
  mid-day's; see [Stock spreads at the open](#stock-spreads-at-the-open).
- **"Hold"** is buying the same amount at the start and keeping it.

## Upcoming tests

### Crypto on Kraken, 2015–2021 (planned Nov 24 2026)

Every crypto result below was found on Binance.US's 2022–2026 bars, most
after trying several settings on them. Kraken's archive goes back to
2013, years none of these multi-coin tests has seen. The test is written
down here in full before its data is imported, so it can be run once and
not tuned on: no setting here changes after the results are seen. A bug
found while running is fixed, rerun and noted, as with XRP's gap in
[Trend ensemble in Kestrel](#trend-ensemble-in-kestrel).

**Already seen:** `trend_ensemble` on BTC alone over 2015–2025 and the
20/10 and 55/20 Turtles on BTC over 2016–2026 (see
[Trend ensemble in Kestrel](#trend-ensemble-in-kestrel) and
[Turtle breakout screen](#turtle-breakout-screen)). The
multi-coin runs, the 10-day breakout and `momentum_rotation` have not been
run before 2022.

**Data:**
- Kraken's downloadable OHLCVT archive, daily bars of USD pairs, imported
  as they are. Not the REST API, which serves only the newest 720 bars.
- Eleven pairs, the same count as the paper runs, chosen as the large
  Kraken USD pairs of 2015–2018, faded ones included, not today's winners:
  BTC (XBT), ETH, LTC, XRP, XMR, ETC, ZEC, DASH, REP, XLM and EOS. A pair
  the archive does not have is dropped and noted, not replaced. Each
  joins when its bars start; until then its share sits in cash.
- The archive is a downloaded file: imported with the untrusted-file
  handling (its own directory, `python -I`).
- **What it holds** (imported Nov 24 2026): all eleven, daily, to Sep 30
  2026 (EOS to Jun 2025, REP to Jun 2026). Bars start Oct 2013 for BTC and
  LTC, Aug 2015 ETH, Jul 2016 ETC, Oct 2016 REP and ZEC, Jan 2017 XMR and
  XLM, Apr 2017 DASH, May 2017 XRP and Jul 2017 EOS. Every bar checked out
  (low under open and close, high over them). EOS, REP and XLM have no
  bars from Aug 22 2017 to Feb 16 2018, the top of the 2017 bubble and its
  fall; LTC has gaps of up to 15 days in early 2015; the rest miss a day
  or six at most.

**Periods:** A, Jan 1 2015 – Dec 31 2017 (few coins, a bull market), and
B, Jan 1 2018 – Dec 31 2021 (the 2018 fall and the 2020–21 rise). Each
uses the bars before it as warm-up, as a live run would; a symbol with
less history runs with what it has, as on a new listing. 2013–2014 is
warm-up only. Period A is mostly BTC and LTC until mid-2016 (ETH from
Aug 2015), and has all eleven pairs only from mid-2017, so its results
are reported per pair too and say so, not as eleven coins' result.

**Account and costs:** 100,000 USD, so Kraken's minimum orders and lots
do not bind; the breakout runs with a 10,000,000 USD account, so its
fixed quantity is never refused as prices rise hundreds of times (ETH,
XRP). Bars come only from the database (`backtest --bars-from-database`),
as Kraken's API has no bars this old. A delisted pair without symbol
info (REP and EOS) gets 1e-8 lots and no minimum, with a warning. Market
orders filled at the next price after each daily close. Two cost levels,
each run in full:
- **10 bps a side:** 5 bps fee and 5 bps slippage, a modern low-fee venue.
- **35 bps a side:** about Kraken's retail taker fee (40 bps at this
  account's tier, 35 at the next; see running.md), spread included, for
  the wider spreads of those years.

**Strategies:**

| Run | Settings |
|---|---|
| `trend_ensemble`, default | `lookbacks = [5, 10, 20, 30, 60, 90, 150, 250, 360]`, `vol_bars = 90`, `target_vol = 0.25`, `days_per_year = 365`, `max_weight = 1`, `rebalance_threshold = 0.2`, `invest_fraction = 0.95` |
| `trend_ensemble`, `[20, 55]` | the same, `lookbacks = [20, 55]` |
| `breakout`, 10-day high, hold 1 day | one run a pair: `timeframe = "1d"`, `setup = "range"`, `range_bars = 10`, `confirm = "none"`, `entry = "stop"`, `arm_bars = 1`, `max_hold_bars = 1`, `target_multiple = 0`, `trend_sma = 0`, `stop_buffer_bps = 10`, and a fixed `quantity`, worth about 1,000 USD at the pair's first price in the period |
| `momentum_rotation` | `lookback_bars = 28`, `rebalance_bars = 7`, `top_n = 3`, `positive_only = true`, `invest_fraction = 0.95` |

The 10-day breakout's settings were not written down when it was found.
Before the Kraken runs, these are run on the 2022–2026 Binance.US bars of
the seven pairs with a build from before the hold fix (below); they should
give about +0.80% a trade over 697 trades. If they do not, the difference
is found and noted first, and the settings that reproduce it are the ones
run. They are then run again with the fix, and that result, not +0.80%,
is the one the Kraken runs are set against.

**Benchmarks:**
- `trend_ensemble` and `momentum_rotation`: BTC and cash in the mix that
  matches the strategy's realized volatility in that period (not a fixed
  20%), and the coins listed at the time, held equally, rebalanced
  monthly and scaled to the same volatility. A fixed mix would count the
  cash of the coins not yet listed as caution.
- The 10-day breakout: holding the same pair over the same bars (its
  average daily return times the days held, "less drift" as in Daily stock
  setups), and random entries.

**Gaps:** a breakout trade open when a gap starts is held across it, as
its exit waits for the next bar (the ADA trade held for 19 months in
[Crypto, 2022 – Oct 2026](#crypto-2022--oct-2026)). A trade held across a
gap of more than 7 days, in the backtest or the random-entry script, is
listed separately and left out of the averages. The portfolio strategies
need no rule: `trend_ensemble` leaves a symbol with no bar as it is and
starts its models again after a gap of more than 7 days, and
`momentum_rotation` does not rank a symbol without bars for the period
and its lookback. Neither can trade a coin during its gap, so one held
when the gap starts stays held, at its last price, until bars return;
that is reported if it happens. Both rebalance once a day late when a
symbol's bars stop (they wait for every symbol's bar, then go on without
it), and on the first day of a run: a delay, not a missed rebalance.

**The 2017 gap and the end of period A:** EOS, REP and XLM have no bars
from Aug 22 2017 into period B, so a coin of the three held on Aug 21 is
still valued at that day's price on Dec 31 2017, and its late-2017 rise
is not seen, nor its volatility. The benchmarks keep such a coin the same
way: held at its last price, not rebalanced, until its bars return. Each
portfolio run and benchmark reports its share of equity in the three on
Aug 21 2017. If any had more than 5% there, period A is also run on the
other eight pairs, and that run is the one judged; both are reported.
With eight pairs `momentum_rotation` starts when four can be ranked, from
late Aug 2016 (ETC's bars start Jul 27).

**Random entries:** a script outside Kestrel, 1,000 runs per pair and
period. Each run enters on random days, at the day's open, as many times
each calendar year as the strategy did, without overlap, and exits as the
breakout does, at the next price after the next day's close, at the same
costs. A random entry is held from the open, the breakout's from its stop
during the day, so each is measured over drift for its own time held,
entry to exit. The random entries' two whole days with no stop spread
wider, which raises their 95th percentile: a harder bar, not an easier
one. Before it is trusted, the script runs the real rule too and must
match Kestrel's backtest trade by trade (count within 2%, average within
0.05% a trade), as the Turtle simulation was checked. That is the rule as
Kestrel runs it: a day's bar walked open, the extreme nearer the open, the
other extreme, close (see running.md); a trade filled on day t, at its
stop or at an open above it, sold at the next price after day t+1's
close. The same check runs on Connors RSI(2) with the 200-day filter on
the 35 Schwab symbols (2016–2026, already seen; a check of the entry, not
a holdout): random entries on days closing above the 200-day average,
with the same exit at a close above the 5-day average.

**What counts as passing** (each at 10 bps a side, unless it says 35):
- `trend_ensemble` (each setting): a higher Sharpe than its
  volatility-matched BTC and cash in both periods, and still ahead at
  35 bps. `[20, 55]` replaces the default only if it is ahead in both
  periods and its Sharpe difference has p < 0.05 (paired 20-day block
  bootstrap); otherwise the default stays.
- The 10-day breakout: a positive average a trade over drift in both
  periods, on at least two thirds of the pairs, above the 95th percentile
  of the random entries pooled over the pairs; and still positive over
  drift at 35 bps.
- `momentum_rotation`: ahead of its volatility-matched BTC and cash in
  both periods, and still ahead without the coin that made it the most
  (the ZEC lesson).

**Changes before any run (Nov 24 2026):** written when the data was
imported, before any of these tests was run.
- The breakout's size was "a fixed 1,000 USD a trade", which `breakout`
  cannot do: it buys a fixed `quantity`, or sizes by risk. Now a fixed
  quantity worth about that at the period's first price, in a larger
  account. Results are per trade, so the size does not change them.
- The rule for trades held across a gap, above.
- Period A reported per pair, above.
- `momentum_rotation` skips a rebalance when under half of its symbols
  can be ranked, so in period A it holds cash until 6 of the 11 can be
  (REP and ZEC join in Oct 2016, ranked 28 days later). Its period A is
  in effect Nov 2016 – Dec 2017, and is reported as that.
- A bug in the breakout's hold, from Gemini's review: the backtest counted
  the entry's own day as held when it filled the stop at the day's open
  time (a gap through it, or the stop crossed before the day's low), and
  sold those trades a day early. Paper and live fills, inside the day,
  never were. Fixed, so the 2022–2026 result is run again, above.
- The random entries exit as the breakout does, and both are measured
  over their own drift, above.
- The rule for the 2017 gap at the end of period A, above.

A strategy that passes goes on to paper trading (the breakout) or stays in
it (`trend_ensemble`). One that fails is written up here and not re-tuned
on these years. With four settings, two periods and two cost levels, a
single pass is weak evidence; a pass in both periods and at both costs is
what would change a plan.

## Summary

| Strategy | Market | Verdict |
|---|---|---|
| `market_maker` | ZEC/USDT and 6 more crypto pairs, quotes | Doesn't beat holding once latency is modelled, on any pair |
| `breakout` range 1h, `trend_sma = 1000` | 7 crypto pairs, 2022–2026 | +0.11% a trade, +0.10% without ZEC, and 2025 lost without ZEC: thin; falls of 50–90% of a trade's size are normal; paper-trading now |
| `breakout` 10-day high, hold 1 day | 7 crypto pairs, daily, 2022–2026 | +0.80% a trade, 7/7 pairs up: the strongest single-symbol result; beats holding on 5 of 7 pairs, not on ZEC or HYPE |
| `momentum_rotation` | 11 USDT pairs, daily, 2022–2026 | +973% (+128% without ZEC) against +63% for BTC; deep drawdowns |
| Other `breakout` setups (nr, inside, rectangle, pipe, throwback) | crypto, 1h and 15m | Lose, or profit only from the ZEC rally |
| `breakout` double bottom | stocks daily; crypto 1h | Modest edge on daily stocks; none on crypto over the long history |
| `breakout` inverse head-and-shoulders | 33 stocks daily; crypto 1h | +1.35% a trade on daily stocks, positive in both periods, but rare (62 trades in ten years); too rare on crypto 1h |
| `breakout` ascending and symmetrical triangles | 33 stocks daily; crypto 1h | +1.67% and +1.36% a trade on daily stocks (390 and 559 trades), positive in both periods, though most trades lose; mixed on crypto 1h |
| `breakout` cup-and-handle | 33 stocks daily; crypto 1h | +2.35% a trade on daily stocks with a year's window (150 trades), positive in both periods, but only 32% won; lost on crypto 1h |
| `breakout` failed breakdown | 33 stocks, daily | +0.9% a trade, but gone since 2022 except on broad ETFs |
| `mean_reversion` Connors RSI(2) | SPY and QQQ, daily, 2016–2026 | +0.25% to +0.36% a trade, 63–70% won, on both and with or without the 200-day filter; the filter cut SPY's worst fall from −14.7% to −5.8%. In the market 11–16% of days, so 15 shares made +18% to +26% against +85% to +88% for holding them |
| Short-bar stock patterns, opening range | 33 stocks, 1m to 15m | Lose after costs |
| Intraday momentum | crypto 30m; SPY and QQQ | Too small for crypto costs; not significant on 181 days of stocks |
| Fair value gaps (FVG), limit entries and `breakout` `fvg` | 11 crypto pairs 1h/4h/1d; 35 stocks 1d and 30m | Lose on crypto and 30m stocks. On daily stocks the gains are the stocks' drift: none beats holding them. The gap neither fills nor bounces differently from an ordinary 3-bar move of the same size |
| `trend_ensemble` on 13 ETFs (`days_per_year = 252`) | Schwab daily, Sep 2019 – Nov 2026 | +8.5% and +18.9% at 5% volatility; level with SPY and cash at the same risk in 2019–2022, behind it in the holdout (Sharpe 0.90 against 1.30, p = 0.12). `[20, 55]` no better |
| Published ideas screened: crypto trend ensemble, volatility-adjusted rotation, turn of the month, dual momentum, GTAA, IBS | 11 crypto pairs and ETFs, daily, development and holdout periods | None worth building: each loses to holding in the holdout. The crypto ensemble halves the drawdown, but without ZEC did no better than 60% BTC and 40% cash |
| `trend_ensemble` (Zarattini, Pagani and Barbon's rules) | 11 USDT pairs, daily, 2022–2026 | +33% in 2022–2024 and +3% in the 2025–2026 holdout, at 7–8% volatility; beat 20% BTC with 80% cash in both, without ZEC too, by a few points. Paper-trading |
| Daily stock setups against holding | 35 stocks, daily, 2016–2026 | None beats holding the same stock over the same bars with confidence: most of each trade is drift. Closest: Connors RSI(2) with the 200-day filter, +0.14% a trade over drift (t = 1.9) |
| Turtle breakout (20/10 and 55/20, 2 ATR stop) | 10 macro ETFs + BTC (2016–2026), and 11 USDT pairs (2022–2026) | No timing edge on ETFs (+1.90 to −1.91 bps/day against exposure-matched holding, \|t\| < 2); the published 5-asset result comes from up to 5.8× CFD leverage and BTC. Likely edge on BTC (t = 2.33, `breakout` PF 4.32). On 11 coins at the same ~7.8% volatility, Turtle led `trend_ensemble` in the holdout but not significantly (p ≥ 0.085), and `trend_ensemble` with `lookbacks = [20, 55]` covers it |

## Market maker (ZEC/USDT)

Replays of recorded quotes and trades: Oct 19 (8 h), Oct 30 (24 h) and
Nov 1–3, 2026. The figures are "vs hold" in USDT.

- **Fixed spread, no latency:** 4–6 bps of half-spread did best on both
  days (+5.9 and +10.0 on Oct 30). At 1–2 bps it filled often and lost to
  better-informed traders (2 bps: −10.6 on Oct 30). Most of each result
  came from how much ZEC was held, not from the spread earned.
- **Volatility-scaled spread:** never beat a fixed spread of the same
  average width. Off by default.
- **Order and report latency (130 / 600 ms):** this changed every result.
  Cancels still in flight get filled, and fills are down 3–5 bps a second
  later. Oct 30 at 4 bps went from +5.9 to −2.0.
- **Live check (run 12, 30 min, 43 fills):** with latency, the backtest's
  1 s markout matched live (−2.99 against −3.00 bps), but it filled about
  28% too often.
- **Asymmetric requoting** (`requote_bps = 6`, `requote_toward_bps = 0`):
  removed the loss from a wide requote. At 6 bps it was the best setting:
  +5.4 on Oct 30 and +2.0 on Oct 19.
- **Microprice centring and the order-flow pull:** no improvement at the
  best setting. Both are off by default. Touch size and recent trades
  predict a fraction of a bp, against 5 bps lost a second after a fill.
- **Queue position** (Nov 2026 replays of Oct 19, Oct 30–31, Nov 1–2 and
  Nov 7, post-only): joining the touch instead of quoting inside the
  spread cut the loss at 2 bps (−100 to −72 against holding, summed;
  improving it by a tick, −88), and made no real difference at 4 or 6 bps
  (within a dollar either way); keeping a quote at the touch changed nothing,
  as ZEC's tick is too fine for a quote to sit exactly there. Every
  setting lost to holding over the four windows (6 bps: −19), and without
  post-only quotes the same replays lose several times as much.
- **Stronger inventory skew** (same replays): squaring the lean
  (`skew_power = 2`) lost more at 4 bps and about the same at 6; growing
  the skew with the position's age (`skew_age_ms`, 30 min) was a little
  ahead of the linear skew at both (−27.2 and −16.7 against −30.8 and
  −18.9), though doubling the linear skew did better at 6 bps (−15.2):
  all within the days' noise.
- **Several levels a side** (same replays): two or three levels a
  half-spread apart lost more at 4 bps (−38.6, −42.4 against −30.8) and
  a little more at 6; twice as far apart they were slightly ahead at 6 bps
  (−17.5, −16.6 against −18.9) and behind at 4. The deeper fills capture
  more, but on falling days the extra levels pile up inventory.
- **Pulling quotes when the feed goes quiet** (`quiet_pull_ms`, same
  replays): neutral at 30 or 60 s; at 2 or 5 s it pulled about 42,000
  and 11,000 times in a quiet market and lost more. A safety guard, not
  an edge.
- **Out of sample (Nov 1–3, ZEC falling):** the best setting lost 19.7
  against holding, with a 1 s markout of −6.2 bps.
- **Seven pairs, Nov 7–9 2026** (the best setting: post-only, latency,
  asymmetric requoting; about $60 a quote, $600 of inventory room and
  $2,400 of the coin to start on each): every pair lost to holding at 4
  and 6 bps of half-spread, together −200 and −137. Totals at 6 bps:
  ZEC −37.5, ETH −29.5, XRP −27.7, HYPE −16.5, ADA −10.1, SOL −9.3,
  BTC −6.5.
  - The 1 s markout was negative on every pair and day. On Nov 8 at 6
    bps it was −8 to −18 bps on SOL, ETH, XRP, ADA and HYPE, more than
    the half-spread earned (ZEC −5.8, BTC −2.6).
  - ZEC fell 4–5% on each of the three days and lost on each. The other
    pairs lost most of theirs on Nov 8, when they fell 1–5%; on the
    flatter days they were near even on few fills, and BTC, SOL, XRP and
    ADA made a few dollars on Nov 9 at 6 bps.
  - The other pairs fill far less than ZEC: BTC 114 fills at 6 bps over
    the three days against ZEC's 906. They lose less mainly because they
    trade less.

## Breakout on crypto, May–Oct 2026

The seven pairs on 1h bars, about $1,000 a trade.

- **Setups:** `nr` and `inside` lost on every pair that didn't rally: they
  trade 275–490 times, and costs add up. `range` made +2,102 in total
  (positive on 5 of 7 pairs), but ZEC and HYPE supplied most of it, and
  holding those did better.
- **Throwback entry:** saved fees, but missed the strong breakouts:
  `range` fell to +984.
- **Rectangle:** −52, against `range`'s +2,102. Requiring touches of both
  sides filters out the trending breakouts that paid.
- **Pipe bottom:** +451 on 1h bars, again mostly ZEC and HYPE. On two
  years of daily bars it made money on ETH, ADA and XRP while holding them
  lost, but on 4–8 trades a pair.
- **Failed breakdown:** lost overall (−546 on 1h).
- **24-hour hold cap:** 6 of 7 pairs positive on 1h bars (ZEC +3,048,
  BTC −212). On 15m bars only ZEC was positive: a third of trades hit the
  stop, against one in seven on 1h bars.
- **Filled on 1m bars:** the `range` 1h baseline made +4,677.
  - Volume confirmation (`volume_ratio = 2` on a close-confirmed entry)
    made +2,141, and all 7 pairs were up with `arm_bars = 3`.
  - A 10-bar trailing stop made +4,115, with a smaller drawdown (5.5%
    against 7.0%).
  - Breakeven at 0.5R made +3,743.
  - None beat the baseline.
- **Double bottom, 1h:** +1,386 (61% won, 6 of 7 pairs up, 1.9% worst
  drawdown). On 15m it lost, and triple bottoms almost never formed.
- **Inverse head-and-shoulders, 1h** (`range_bars = 40`): +101 on 20
  trades in six months across the 7 pairs; it hardly forms on hourly bars.
- **Triangles, 1h** (`range_bars = 40`): the symmetrical triangle made
  +1,187 on 236 trades, but +871 of it was ZEC; the ascending triangle
  lost 34 (116 trades). With `min_touches = 3` both traded 14 times.
- **Cup-and-handle, 1h** (`range_bars = 120`): −519 on 108 trades, 31%
  won.
- **Resting target** (`resting_target`, the target a sell limit paired
  with the stop): `range` fell from +4,677 to +3,855 and the double bottom
  from +1,386 to +832, though slightly more trades won. Selling on the
  close past the target catches the bars that close well beyond it; a
  limit stops at the target.
- **Shorts** (`direction = "short"`, with `[paper] allow_short`): `range`
  +937 (725 trades, ZEC +937 and HYPE −509), the double top +367, the
  descending triangle +724 and the falling symmetrical triangle +140,
  against +4,677, +1,386, −34 and +1,187 for the long side.
- **Stop-and-reverse** (`reverse`): `range` +5,541 on 895 trades against
  +4,677, 6 of 7 pairs better; the double bottom +1,183 against +1,386.
- **ATR stop** (`atr_stop`, 14-bar ATR under the broken level, instead of
  the setup's low): 1.5 ATRs +4,571, 2 ATRs +4,935, 3 ATRs +5,053, against
  +4,677 for the setup-low stop. 2 and 3 ATRs cut the worst drawdown from
  7.0% to 5.6%. Without ZEC the order flips: +1,822, +1,436 and +1,102
  against +1,304. No clear winner.
- **Moving-average crossover** (`ma_crossover` 10/30 on 1h): +2,301 (5 of
  7 pairs up, 14% worst drawdown) with simple averages, +1,067 (3 of 7)
  with exponential ones, which turn sooner and were whipsawed more. Both
  are almost all ZEC (+2,023 and +1,053).
- **Rectangle 15m and nr 15m with the new filters:** rectangle's +2,449 is
  all ZEC, and every filter made it worse. `nr` 15m lost on every pair.

## Crypto, 2022 – Oct 2026

The long history overturned the May–Oct results. Figures are the average
return per trade, net of costs.

- **No filter:** range 1h made −0.05% a trade (5,174 trades), and the
  double bottom −0.01%. ZEC was the worst pair over the long history:
  May–Oct 2026 was its best stretch, and also the window that was tuned on.
- **Trend filter (`trend_sma`, entries only above the 1000-hour
  average):**
  - Range 1h rose to +0.11% a trade (2,709 trades), and +0.10% without
    ZEC. Results improved steadily from a 200- to a 1000-bar filter.
  - By year: 2022 −0.04%, 2023 +0.12%, 2024 +0.02%, 2025 0.00% and 2026
    +0.32%. Without ZEC, 2025 averaged −0.30% a trade.
  - This was first reported as +0.17% a trade. That figure counted an ADA
    trade held for 19 months, July 2023 to February 2025, across the gap
    in Binance.US's USD data. The hold limit could not fire with no bars
    arriving, and the trade made +163%. Without it, ADA is −0.14% a trade,
    not +0.44%. The other long-history 1h results have not been rechecked
    for the same gap.
  - The double bottom rose to +0.20%, but still lost in 2022 and 2024.
- **Daily 10-day-high breakout, sold after one day:** +0.80% a trade (697
  trades, 51% won), 7 of 7 pairs up, +0.80% without ZEC. Only 2022 lost
  (−0.29%). Found before the hold fix of Nov 24 2026 (see
  [Upcoming tests](#upcoming-tests)): a trade whose stop filled at the
  day's open time was sold a day early. To be run again.
  - Against holding, per pair: the strategy's trades compounded (each
    trade's return on the whole stake, though the backtest bought a fixed
    quantity), against buying at the first bar and holding to the last.

    | Pair | Trades | Average | Compounded | Strategy drawdown | Hold |
    |---|---|---|---|---|---|
    | BTC | 156 | +0.49% | +94% | 18% | +66% |
    | SOL | 145 | +1.18% | +280% | 39% | −45% |
    | ZEC | 148 | +0.79% | +61% | 85% | +767% |
    | ETH | 96 | +0.99% | +128% | 17% | −40% |
    | ADA | 71 | +1.05% | +84% | 25% | −86% |
    | XRP | 35 | +0.33% | +5% | 33% | −43% |
    | HYPE | 46 | +0.19% | −5% | 53% | +155% |

    It beat holding on 5 of 7 pairs, and lost on the two that rose most
    (ZEC and HYPE). Split equally over the pairs it made about +93%
    against +111% for holding; without ZEC, about +98% against +1%. It is
    in the market only on the days it holds (35 to 156 of them), so most
    of the stake sits in cash.
  - Trailing-stop versions averaged more per trade but depend on ZEC.
  - Trend filters of 20–200 days didn't reliably improve it. The 200-day
    filter skips 2022 entirely, at a third of the total.
- **Losing streaks and worst periods** (per pair, as a single-pair run
  trades; in % of one trade's size):

  | | Range 1h, `trend_sma = 1000` | 10-day high, hold 1 day |
  |---|---|---|
  | Longest losing streak | 5–9 trades (ZEC 13) | 4–7 trades |
  | Most losses in one UTC day | 2 (ZEC 3, once) | 1 |
  | Worst trade | −7.6% (BTC) to −25% (ADA) | −12% to −25% |
  | Worst day | −8% to −25% | −12% to −25% |
  | Worst week | −9% to −31% | −12% to −25% |
  | Worst fall from peak | −50% to −87% (ZEC −345%) | −18% to −46% (HYPE −69%, ZEC −170%) |

  - Daily limits hardly bind on one pair. A daily loss limit of 5%, 10%
    or 20% of a trade refused 1–28 of 2,709 range trades and none of the
    daily strategy's, and moved the results by noise.
    `max_consecutive_losses = 2` changed nothing.
  - A drawdown limit, which does not reset, would have ended almost
    every range run early. At 25% or 50% of a trade it stopped all seven
    pairs, SOL in April 2022 before its later trades made +176%. At 100% it stopped only ZEC, in April 2023, before +143%.
  - The range strategy's stop is a median 2–6% below the entry, up to
    12–68% on a wide range. A `max_loss_per_position` inside that closes
    trades before their own stop.
  - With the gate on (BTC and SOL, 2025–26), it refused entries after the
    daily loss limit and after a position-loss close, and closed positions
    at market. The before-and-after P&L there means little: the limits
    were in dollars and the quantities fixed, so a SOL trade was about
    $3,200 at 2025 prices, not $1,000.
- **Time of day (hold BTC 21:00–23:00 UTC):** at best about 4 bps a day,
  against 14 bps of costs. Rejected.

## Momentum rotation

11 Binance.US USDT pairs, daily bars, 2022 – Oct 2026, filled on 1h bars.
The default is to rebalance weekly into the top 3 by 28-day return.

| Lookback, holdings | Return | Drawdown | Without ZEC | Drawdown |
|---|---|---|---|---|
| 28 days, top 3 | +973% | 68% | +128% | 56% |
| 28 days, top 5 | +440% | 65% | +120% | 49% |
| 14 days, top 3 | +404% | 54% | +81% | 64% |
| 7 days, top 1 | +2,277% | 59% | −24% | 85% |
| Hold BTC | +63% | 67% | | |

It beats holding BTC with or without ZEC, but ZEC made about 76k of the
default's 97k USDT profit. Short lookbacks with one holding were erratic.

## Intraday momentum

Does the early part of the day predict its last half-hour?

- **Crypto (11 pairs, 30m bars, 2022–2026):** yes, but weakly. The US
  session so far predicts its last half-hour by +2.35 bps, and its last
  hour by +3.84 bps (+8.5 bps on the strongest fifth of days). That's well
  short of 14 bps of costs.
- **SPY and QQQ (30m bars, Feb–Nov 2026, 181 days):** the paper's signal
  (prior close to 10:00) made +2.69 bps a day on QQQ (t = 1.5) and +0.91
  on SPY (t = 0.75). The session-so-far signal was negative on both. The
  sample is far too small to confirm or reject it.

## Breakout on stocks

- **SPY 30m, October 2026:** nr −12.87 (19 trades), inside −10.49 (18),
  range −14.84 (1). Too few trades to judge.
- **Pipe bottom (SPY, AAPL, QQQ, DKI):** on daily bars since 2016, SPY and
  QQQ made +0.2% a trade, about three trades a year, and AAPL −0.8%.
  Holding beat it everywhere except DKI, whose results rest on 1–8 trades.
- **Failed breakdown, 33 stocks and ETFs, daily since 2016:** +0.94% a
  trade (937 trades), but the median trade lost: a few big wins carry many
  small losses.
  - 2016–2021: +2.32% a trade. Since 2022: −0.36%.
  - Only broad ETFs stayed positive in both halves (+0.89% since 2022).
- **Double bottom, same 33 symbols:**
  - Daily: +0.71% a trade (321 trades, median +2.30%), positive in both
    periods (+1.03% since 2022). It trades about once a year per symbol,
    so it times entries rather than replacing holding.
  - 30m: +0.41% a trade (81 trades).
  - Triple bottoms were too rare to judge.
- **Inverse head-and-shoulders, same 33 symbols, daily** (`range_bars =
  60`): +1.35% a trade (62 trades, median +3.08%, 58% won); +0.98% in
  2016–2021 and +1.80% since 2022. On the same window the double bottom
  made +0.69% on 320 trades.
- **Triangles, same 33 symbols, daily** (`range_bars = 60`, 2 touches a
  side): ascending +1.67% a trade (390 trades, 43% won, median −2.25%):
  +1.48% in 2016–2021 and +1.91% since 2022. Symmetrical +1.36% (559
  trades, 38% won): +2.00% and +0.57%. A few large wins carry many small
  losses.
- **Cup-and-handle, same 33 symbols, daily:** with `range_bars = 250`
  and `swing_bars = 3`, +2.35% a trade (150 trades, 32% won, median
  −2.44%): +1.49% in 2016–2021 and +3.28% since 2022. With `range_bars =
  120` and the default swings, +1.06% (289 trades, 27% won). The stop
  under the handle is close, so most trades are stopped out and a few
  large ones carry the result.
- **Resting target, same 33 symbols, daily:** the double bottom went from
  +0.69% to +0.62% a trade, the ascending triangle from +1.67% to +1.27%,
  for the same reason as on crypto.
- **Shorts, same 33 symbols, daily** (each setup upside down; no borrow
  fee): lost on all but one. Double top −0.42% a trade (254 trades),
  descending triangle −1.32% (278), falling symmetrical triangle −1.02%
  (475), inverted cup −0.97% (152). The head-and-shoulders top made +0.77%
  (71), all of it since 2022 (−1.70% in 2016–2021, +3.96% since). Over a
  rising decade the shorts mostly paid for the market's drift.
- **Stop-and-reverse, same 33 symbols, daily:** it lowered every result:
  the double bottom from +0.69% to +0.17% a trade (the reversed shorts
  −0.94%), the ascending triangle from +1.67% to +0.85% (shorts −0.37%).
- **Double bottom on 1m, 5m and 15m (Sep 22 – Nov 6):** negative at every
  bar size: −0.05% to −0.21% a trade with each stock's estimated spread.
  The patterns are tiny, and the stop sits about twice as far as the
  target.
- **Opening range breakout (33 stocks, Sep 22 – Nov 6):** lost 7–19 bps a
  trade in every variant; the best was a 5-minute range on 1m bars. The
  published results trade only "stocks in play" (high volume or a gap),
  which this didn't screen for.

## Mean reversion on stocks

SPY and QQQ, daily bars from Mar 2016 to Nov 4, 2026 (2,687 bars, warmup
from Apr 2015), 15 shares a trade from $10,000, filled at the next open.
Connors RSI(2) buys at an RSI of 10 or under and sells at 90 or a close at
the 5-day average; RSI(14) buys at 35 or under below the lower 20-day
Bollinger band (2 deviations) and sells at 70 or the 20-day average. No
holding limit or stop.

| Symbol | Setup | Return | Worst fall | Trades | Won | A trade | Median held | Days in |
|---|---|---|---|---|---|---|---|---|
| SPY | Connors, 200-day filter | +23.26% | −5.77% | 93 | 70% | +0.33% | 4 days | 11% |
| SPY | Connors, no filter | +26.48% | −14.74% | 125 | 67% | +0.25% | 4 days | 16% |
| SPY | RSI(14) + band, 200-day filter | +4.42% | −13.02% | 16 | 75% | +0.61% | 19 days | 7% |
| QQQ | Connors, 200-day filter | +18.74% | −4.94% | 89 | 66% | +0.36% | 5 days | 12% |
| QQQ | Connors, no filter | +24.30% | −7.84% | 119 | 63% | +0.34% | 5 days | 16% |
| QQQ | RSI(14) + band, 200-day filter | +15.11% | −7.06% | 18 | 83% | +1.36% | 16 days | 7% |

- **Connors RSI(2) made money on both, with or without the filter.** The
  200-day filter cost SPY a little (+23% against +26%) and cut its worst
  fall from −14.7% (the 2020 crash) to −5.8%. On QQQ the filter's fall
  was −4.9% against −7.8%.
- **Holding the same 15 shares made more:** about +$8,530 on SPY (+85% of
  the account) and +$8,790 on QQQ (+88%). The strategy was in the market
  11–16% of days with a fixed 15 shares, so it put far less to work; per
  day invested it did well, and the cash between trades is free for
  something else.
- **RSI(14) under the band is rare:** 16–18 trades in ten years, too few
  to weigh its 75–83% won.
- **A second run agrees:** another model's backtest of the same six
  matched four exactly and was one trade off on the two filtered Connors
  runs (+23.19% and +18.37%, 94 and 90 trades). The cause wasn't found;
  several signals sat within 0.2% of the 200-day average, where a tiny
  difference in a price flips the filter.

## Daily stock setups against holding

Each daily stock setup above was rerun in Kestrel's backtester on the 35
Schwab stocks and ETFs, from Jan 2016 to Nov 2026. Each trade was then
compared with holding the same symbol over the same bars, its average daily
return times the bars held ("less drift"). The stocks rose about 1,355% over
the period, and these setups hold for 15–28 bars, so drift alone is worth
+0.9% to +1.7% a trade.

| Setup | Trades | A trade | Drift | Less drift | t | Symbols up | 2016–21 | 2022–26 | ETFs | Without the best 5 |
|---|---|---|---|---|---|---|---|---|---|---|
| Ascending triangle (`range_bars = 60`) | 407 | +1.90% | +1.24% | +0.67% | 1.3 | 20/35 | +0.35% | +1.05% | +0.09% | +0.06% |
| Cup-and-handle (`range_bars = 250`, `swing_bars = 3`) | 152 | +2.30% | +1.67% | +0.62% | 0.7 | 20/33 | −0.08% | +1.41% | +0.62% | −0.66% |
| Symmetrical triangle (`range_bars = 60`) | 574 | +1.27% | +1.09% | +0.17% | 0.5 | 17/35 | +0.76% | −0.55% | +0.12% | |
| Double bottom (`range_bars = 60`) | 327 | +0.90% | +0.89% | +0.01% | 0.0 | 18/35 | −0.39% | +0.52% | −0.05% | |
| Inverse head-and-shoulders (`range_bars = 60`) | 63 | +1.23% | +1.60% | −0.37% | −0.3 | 14/31 | −1.26% | +0.81% | −0.77% | |
| Failed breakdown | 967 | +0.95% | +1.38% | −0.43% | −1.4 | 15/35 | +0.81% | −1.61% | +0.33% | |
| Connors RSI(2), 200-day filter | 2,384 | +0.42% | +0.28% | +0.14% | 1.9 | 23/34 | +0.17% | +0.10% | +0.02% | |
| Connors RSI(2), no filter | 4,365 | +0.30% | +0.23% | +0.07% | 1.1 | 18/35 | −0.04% | +0.19% | −0.08% | |

- **No setup beats holding with any confidence.** The raw figures match
  the earlier runs (on 33 symbols then), but most of each trade's gain is
  the stock rising while it was held.
- **The two best are a few large trades in volatile stocks.**
  - The ascending triangle's +0.67% falls to +0.06% without its 5 best
    trades. One DKI trade made +90% over drift, and COIN, PLTR, TSLA and
    AMD together made +3.19% a trade.
  - The cup-and-handle falls to −0.66% without its 5 best trades.
  - Both setups were about even on ETFs and on AAPL, AMZN, GOOGL, META,
    MSFT and NVDA.
- **Connors RSI(2) with the 200-day filter comes closest**: +0.14% a trade
  over drift, positive in both periods and on 23 of 34 symbols. It holds
  about 4 bars, so per day in the market it earns about one and a half
  times holding's rate. On ETFs it is only +0.02%, and t = 1.9 is weak.

## Fair value gaps

Three bars in a row whose first high and third low do not overlap leave a
"fair value gap" in the middle bar. The published setup waits for a swing
low and a rally that breaks the last swing high before it, a "change of
character" when the market had been making lower highs and lower lows. It
then rests a buy limit at the midpoint (50%) of a gap the rally left inside
its own 50%–61.8% Fibonacci pullback, with a stop below the gap's middle bar
and a target at 2R.

Gemini 4 Carbon ran the tests, and Claude Opus 5.5 reran them with its own
code. The data:
- 11 continuous Binance.US USDT pairs: `1h` and `1d` from Jan 2022 to Nov
  2026, `4h` from Oct 2024 to Oct 2026.
- 35 Schwab stocks and ETFs: `1d` from Jan 2016 to Nov 2026, `30m` from Feb
  to Nov 2026.

Costs are 0 on resting limit entries and targets, and 7 bps (crypto) or 5 bps
(stocks) on stop and time exits. A limit fills only when price trades
strictly through it. Swings are 3-bar fractals, known 3 bars after they
print.

On daily stocks, a trade's return is also compared with holding the same
symbol over the same bars, its average daily return times the bars held
("less drift"). These 35 stocks rose about 1,355% over the period, so on daily
bars any long position makes money.

- **Does the gap pull price back, or bounce it better?** No.
  - The test compared gaps of at least 0.2 ATR with 3-bar up moves of the
    same ATR-scaled size whose wicks overlapped. A buy rested below each one
    for 20 bars, as deep below the close as the gap's midpoint.
  - Gaps filled about as often as the overlaps: 71–76% against 68–79%.
  - Once filled, both groups hit the middle-bar stop first 62–74% of the
    time.
  - Neither group did consistently better (gaps against overlaps): −0.24R
    against −0.21R on `1h` crypto, +0.04R against −0.14R on `1d` crypto,
    −0.13R against −0.05R on `1d` stocks, and −0.23R against −0.30R on `30m`
    stocks.
  - Gemini first found that gaps filled *less* often (71–76% against
    79–84%), but its control's buy rested at the middle of the 3-bar move,
    which is shallower. At equal depth the difference is between −4 and +3
    points.
- **Resting limits at the gap made nothing on any market or timeframe.**
  Each order rested up to 24 bars, with a `2R` resting target, and trades
  closed after 48 bars. `30m` trades closed at the end of the session. The
  table gives the average return per trade, net of costs, with the number of
  trades in brackets:

  | Variant | Crypto 1h (2022–26) | Crypto 1d (2022–26) | Stocks 1d (2016–26) | Stocks 1d, less drift | Stocks 30m (2026) |
  |---|---|---|---|---|---|
  | Plain 50%–61.8% Fib pullback (no gap) | −0.06% (8,212) | −0.86% (330) | +0.07% (1,563) | −0.13% | −0.05% (838) |
  | Raw gap midpoint | −0.09% (15,382) | +0.05% (518) | +0.16% (2,279) | −0.03% | −0.05% (1,291) |
  | Gap + Fib + change of character (the published setup) | −0.08% (1,562) | −0.31% (66) | +0.03% (184) | −0.11% | +0.01% (33) |
  | Gap + Fib + any break + trend SMA (1000h / 200d) | 0.00% (2,800) | −0.92% (114) | +0.11% (635) | −0.08% | −0.14% (86) |
  | Same, buying the top of the gap | 0.00% (3,138) | −0.95% (127) | +0.56% (790) | +0.12% (t = 0.7) | −0.10% (113) |

  - Crypto `4h` (14 USDT pairs) lost too: −0.13% a trade at the raw gap
    midpoint and −0.23% for the published setup.
  - Removing ZEC left the `1h` results unchanged.
  - Every stock variant that made money did so by holding a rising stock.
    Less drift, all of them landed within ±0.13% a trade, with |t| of 1.5
    or less.
  - The top of the gap, with no other rule, made +0.39% a trade on daily
    stocks (2,714 trades) but only +0.04% less drift. The same buy at an
    overlapping 3-bar move's third low made +0.02% (900 trades).
- **Gemini's simulator was too pessimistic on gapped opens.** When a bar
  opened below a resting buy limit, it filled the buy at the limit, not at
  the lower open. When that open was also below the stop, it booked the whole
  drop from the limit as a loss.
  - A real resting limit fills at the open, and so does Kestrel's backtester.
  - With Gemini's rule, the code here reproduces its figures: −0.19% for the
    raw gap midpoint on `1h` crypto (15,381 and 15,382 trades), and −0.15%
    against Gemini's −0.16% on daily stocks.
  - The rule mattered most where bars open away from the last close: ZEC
    (−0.51% against −0.01% a trade) and daily stocks.
  - Gemini's replay of the `1h` setups on `1m` bars (seven pairs, May–Oct
    2026) used the same rule. It confirms that stop-first ordering within a
    bar changes little (2–3 bps), not that the fills are right.
- **Kestrel's own `breakout` `fvg` setup did no better.** It buys on a stop
  above the gap's third bar (`entry = "stop"`), or with a post-only limit at
  the gap's top after a close above it (`entry = "throwback"`). Results from
  Kestrel's backtester, fees included:
  - Crypto `1h` (`max_hold_bars = 24`): −0.07% a trade with the stop entry
    (8,314 trades) and −0.15% with throwback (9,972). Without ZEC: −0.03% and
    −0.12%.
  - Daily stocks (`min_gap_bps = 50`, `max_hold_bars = 10`): +0.42% a trade
    with the stop entry (3,390 trades, 48% won) and +0.22% with throwback
    (2,753). But holding the same symbols over the same bars made +0.52% and
    +0.28%.
  - Less drift, that is −0.10% and −0.06% (t = −1.1 and −0.8). Since 2022 it
    is −0.32% and −0.19%. The ETFs alone fared no better.
- **What didn't rescue it:**
  - Gemini also tried wider gaps (`0.5` or `1.0` ATR), uncapped bar-close
    targets and swing-low stops. All stayed negative on `1h` crypto and were
    noise on daily bars; these ran with its fill rule and were not rerun.
  - Waiting for a bullish close inside the gap, then buying at that close
    and holding 10 bars, made +0.34% a trade on daily stocks (1,528 trades).
    Gemini's run of this gave +0.35%. Less drift, it is −0.13%. It lost
    0.14% a trade on `1h` crypto.
- Rejected: no variant beats holding the symbol, and the gap adds nothing
  over a plain 3-bar move of the same size.

## Published strategies, screened (Nov 2026)

Five ideas from the literature, screened before building any of them in
Kestrel, in a daily-bar simulation on the same database bars. Prices only,
so neither side earns dividends. Settings were chosen on a development
period, then fixed and run once on a holdout, as so many ideas have now
been tried on the same years:

- **Crypto:** develop on Jan 2022 – Dec 2024, hold out Jan 2025 – Nov
  2026. Universe: `momentum_rotation`'s 11 USDT pairs, filled at the
  daily close, at 7 bps a side.
- **ETFs:** develop on 2016–2022, hold out Jan 2023 – Nov 2026, at 1 bps
  a side. Median spreads in the recorded Schwab quotes are 0.40 bps on
  SPY, 0.46 on QQQ, 0.74 on IWM and 1.6–2.3 on sector ETFs, so the usual
  5 bps would be about ten times too much here. Idle cash earns nothing.
  BIL's price is no proxy for T-bills: it gains about 1.6 bps a day, then
  drops about 32 bps on the first trading day of each month when it pays
  out, and it ended 2023–2026 where it began. T-bills (4–5% a year in
  2023–2025) would add about that rate times the time spent idle. SHY, BIL
  and IEF were fetched for this.

| Idea | Development | Holdout | Benchmark (holdout) |
|---|---|---|---|
| Crypto trend ensemble: Donchian breakouts over 5–80 days, volatility-sized | +112%, 36% fall (BTC +130%, 67%) | +0%, 28% fall; −18%, 35% without ZEC | BTC −28%, 53% fall; 60% BTC and 40% cash −17%, 35%; the 10 coins without ZEC −51%, 65%; all 11 +89% (ZEC) |
| `momentum_rotation` ranked by return / volatility | +204% (current: +246%) | +159% (current: +139%) | |
| ... and sized by inverse volatility | +188% | +17% | |
| Turn of the month, SPY, day −2 to +3 | +23% (19% invested) | +11% | SPY +92%, 19% fall |
| Dual momentum, SPY / EFA, else IEF | +3% (from Oct 2019) | +68%, 19% fall | SPY +92%, 19% fall |
| GTAA-5 (SPY, EFA, EEM, IEF, GLD over the 10-month average) | +8%, 8% fall | +36%, 10% fall | the five held: +50%, 10% fall |
| IBS < 0.2 over the 200-day average, 9 ETFs, 3 slots, next open | +98%, 16% fall (SPY +90%, 34%) | +8% | SPY +92% |
| ... filled at the close | +69% | +17% | |
| ... SPY alone, filled at the close | +16% | +36%, 6% fall | |

- **None is worth building on this evidence.**
- **The crypto trend ensemble cut the drawdown, but showed no timing skill
  in the holdout.**
  - It ran at about 30% volatility, against 50% for BTC, so the fair
    comparison is BTC at that risk: 60% BTC and 40% cash.
  - Without ZEC it did the same as that (−18% against −17%), so its smaller
    loss than the coins' (−51%) came from being half invested, not from
    when it was. With ZEC it made 0%, 17 points ahead, from one coin again.
  - In development it beat 60% BTC (+112% against +78%), but those are the
    years it was set on.
  - Volatility sizing gave it little of ZEC's rally, as ZEC was the most
    volatile coin. Other lookbacks and volatility targets made −17% to +17%
    in the holdout.
  - Still the best candidate for a lower-risk crypto allocation, judged
    against 60% BTC and 40% cash. Built as `trend_ensemble` on the
    paper's own rules, which did better; see
    [Trend ensemble in Kestrel](#trend-ensemble-in-kestrel).
- **Volatility-adjusted rotation:** the risk-adjusted ranking was worse in
  development and better in the holdout, which is noise. Inverse-volatility
  sizing lost in both periods. Rotation's results are still mostly ZEC:
  without it, today's rotation made +245% in development and lost 37% in
  the holdout.
- **Turn of the month:** SPY earned 6.4 bps a day in the window against
  3.8 bps on other days in development, but 6.2 against 7.4 in the
  holdout.
- **Dual momentum and GTAA lost to holding:** by 24 points and by 14,
  with the same worst fall. Dual momentum missed 2020's rebound and still
  fell with SPY in 2022.
- **IBS beat SPY in development at realistic costs, then lost in the
  holdout.** Low-IBS closes are followed by about +8 bps overnight,
  against 3–4 bps on an average night.
  - Filled at the next open, as specified, the basket made +98% against
    SPY's +90% in development with half the fall, then +8% in the holdout.
  - Filled at the close it made +17%.
  - SPY alone at the close made +36% with a 6% fall, but that variant was
    the weakest in development (+16%).
  - It is invested about a quarter to half the time, so it lags holding
    unless the idle cash earns elsewhere.
- **Intraday ideas (opening range on "stocks in play", SPY noise area)**
  were not tested. They need years of 1-minute bars and, for the first, a
  scanner across thousands of stocks.
- **Corrections:** a first version of this section charged 5 bps a side
  on ETFs and counted BIL's price as the cash yield. That cost the IBS
  basket its development lead, and gave turn of the month +21% in the
  holdout, which was BIL's mid-month rise without its monthly drops.
  Another model's review found both.

## Trend ensemble in Kestrel

`trend_ensemble` ([trend-ensemble.md](strategies/trend-ensemble.md)) follows
Zarattini, Pagani and Barbon's rules, which differ from the variant screened
above: an entry on the highest close (not a break of the highest high), a
trailing stop at the channel's midpoint that only rises (not the lowest
low of half the lookback), nine lookbacks from 5 to 360 days, and each model
sized to 25% volatility over 90 days (not a portfolio-wide reference).
These are the paper's settings, not tuned here. Same 11 USDT pairs and
periods as the screen, 1,000 USDT, 2 bps taker fee and 5 bps slippage a
side, filled at the next price after each daily close. Binance.US has
about 850 daily bars before Jan 2022 for most of these pairs, so the
models start with that much history, not the 1,081 bars a live run asks
for:

| 1,000 USDT | Jan 2022 – Dec 2024 | Holdout, Jan 2025 – Nov 2026 |
|---|---|---|
| `trend_ensemble` | +32.7%, 6.9% fall, Sharpe 1.26 | +2.8%, 5.9% fall, Sharpe 0.26 |
| ... without ZEC | +36.4%, 7.6% fall, Sharpe 1.33 | −1.6%, 8.0% fall |
| The same rules in the daily screen | +35% | +4%; −2% without ZEC |
| 20% BTC, 80% cash (the same risk) | +26%, 15% fall | −6%, 13% fall |
| Hold BTC | +130%, 67% fall | −28%, 53% fall |

- **The backtest matches the screen** within 2.5 points in each period,
  so the strategy does what the screen did.
- **A first run of it was wrong in 2022–2023:** with XRP/USDT delisted
  (no bars from Jan 2021 to Jul 2023), every day waited for XRP's bar and
  completed only on the next day's first bar, ADA's, so ADA was never
  traded and the rest traded a day late. Its models also joined XRP's 2021
  and 2023 prices. Both are fixed (a symbol that misses a period is no
  longer waited for, and a gap of more than 7 bars and a week restarts
  its models); the totals moved by under half a point. Another model's
  review found it.
- **It runs at about 7–8% volatility**, a quarter of the variant above, so
  it is compared with 20% BTC and 80% cash. It beat that in both periods,
  without ZEC too (−1.6% against −6% in the holdout), with about half the
  fall. That is the first holdout in this file to beat a benchmark of the
  same risk without ZEC, though only by about 4 points over two years of
  falling prices.
- **It made about 10% a year in 2022–2024 and 1.5% a year since,**
  against the paper's 18% over 2015–2025. Its weight per coin is small at
  25% volatility: on average 10% of equity was in coins, at most 40%.
- **Turnover was about five times equity a year:** 5,251 fills in under five
  years, most of them a few USDT as one model of nine changed, which is
  why the account needs to be large enough for Binance.US's 1 USDT minimum.
- Paper-trading on 1,000 USDT from Nov 19 2026 (run 45; see Paper runs).

### On 13 ETFs (Schwab daily bars, 2019–2026)

The same rules, untuned, with `days_per_year = 252`, on SPY, QQQ, IWM, DIA,
EEM, EFA, GLD, TLT and the XLE, XLF, XLK, XLU and XLV sectors: 100,000 USD
(whole shares need it; on 1,000 USD most ETFs are a share or less of a
symbol's share), no commission, 5 bps slippage a side, filled at the next
price after each close. The full 1,081-bar warm-up of the nine lookbacks
starts the test in Sep 2019. Schwab's bars are split-adjusted but carry no
dividends, and cash earns nothing; both understate holding more than the
strategy, which was about half in cash.

| 100,000 USD | Sep 2019 – Dec 2022 | Holdout, Jan 2023 – Nov 5 2026 |
|---|---|---|
| Default 9 lookbacks | +8.5%, 5.2% vol, Sharpe 0.50, 6.3% fall; 41% invested | +18.9%, 5.2% vol, Sharpe 0.90, 5.2% fall; 48% invested |
| `lookbacks = [20, 55]` | +9.1%, Sharpe 0.51, 5.5% fall | +12.8%, Sharpe 0.65, 6.8% fall |
| SPY and cash at the default's volatility (22%; 35% in the holdout) | +7.9%, Sharpe 0.46, 8.2% fall | +28.9%, Sharpe 1.30, 7.0% fall |
| Hold SPY | +31.5%, Sharpe 0.46, 34.1% fall | +101.4%, Sharpe 1.30, 19.0% fall |
| Hold the 13, equal weight | +24.3%, Sharpe 0.43, 30.3% fall | +75.4%, Sharpe 1.22, 14.9% fall |

- **No edge over SPY at the same risk.** Level in 2019–2022 (ΔSharpe +0.03,
  95% CI [−0.74, +0.71]), behind in the holdout (−0.41, [−1.01, +0.10],
  p = 0.12; paired 20-day block bootstrap, correlation 0.6–0.7). Its worst
  fall was smaller than the SPY mix's in both periods (6.3% against 8.2%,
  and 5.2% against 7.0%), not enough to make up the return. This agrees
  with the Turtle screen's ETF result (no timing edge over holding).
- **`[20, 55]` did no better here**: level in 2019–2022 and 6 points behind
  the default in the holdout, at a quarter of the fills (728 and 877
  against 3,261 and 3,698).
- **Turnover** was about 10 times equity a year (7.1 million USD traded in
  seven years), most of it small resizes; free at Schwab, but each fill
  pays the spread.

### Checked against the paper: BTC on Kraken, 2015–2025

To check that the rules are the paper's, they were run on Bitcoin alone
over its period (Jan 1 2015 to Mar 19 2025), on Kraken's daily XBT/USD
bars from Oct 2013 (Kraken's downloadable OHLCVT archive; the paper used
coinmarketcap's prices). A Python replication of the paper's formulas
gave:

| BTC, Jan 2015 – Mar 2025 | CAGR | Vol. | Sharpe | Worst fall |
|---|---|---|---|---|
| Paper, Table 1 Combo (no costs) | 30% | 17% | 1.58 | 19% |
| Replication, no costs | 27.4% | 16.3% | 1.57 | 18.9% |
| Paper, Table 3 BTC (10 bps, 20% threshold) | 30% | 17% | 1.56 | 19% |
| Replication, 10 bps, 20% threshold | 27.3% | 16.6% | 1.53 | 19.5% |
| ... at 50 bps | 24.3% | 16.7% | 1.39 | 23.2% |
| Hold BTC | 73.1% | 70.5% | 1.13 | 83.6% |

- **The replication matches the paper's risk-adjusted results:** Sharpe
  within 0.03 and worst fall within a point for the ensemble, and within
  about 0.1 of Sharpe for each of the nine lookbacks alone. Returns are 2–3
  points a year lower throughout, which looks like the different price
  source (Kraken's thin 2013–2015 market against an index of exchanges).
- **Leverage never mattered on BTC:** its 90-day volatility never fell
  under 26% in the period, so 25% / volatility never reached 100%, and the
  paper's 200% cap and Kestrel's 100% give the same results.
- **Costs matter little:** the 20% threshold keeps turnover to about six
  times equity a year, so even 50 bps a trade leaves a Sharpe of 1.39.
- **Kestrel's `trend_ensemble` gives the same result as the replication.**
  It was run as a Kestrel backtest on the same bars (BTC/USD alone,
  100,000 USD, 10 bps a trade, filled at the close, `invest_fraction` 1).
  It starts on Sep 23 2016, the first day with Kestrel's 1,081 bars of
  warm-up in Kraken's data. From then to Mar 19 2025 both made 28.6% a
  year at 16.4% volatility, a Sharpe of 1.61 and a 19.5% worst fall
  (holding BTC: 79.5% a year, an 83.6% fall). It averaged 20% of equity in
  BTC, and at most 83%.
- **So the lower results above are the period, not the implementation.**
  The same rules on BTC alone, spot, at 10 bps made 34.4% a year (Sharpe
  1.78) in 2015–2021, 16.4% (Sharpe 1.09) in Jan 2022 – Dec 2024, and
  −3.6% (Sharpe −0.28) from Jan 2025 to Sep 2026, when holding BTC made
  −6.2%. The paper's decade includes three crypto bull markets, and our
  test periods include none.

## Turtle breakout screen

Tested from a [HowToTrade guide](https://howtotrade.com/wp-content/uploads/2024/10/The-Turtle-Trading-strategy.pdf)
and a modified long-only Turtle video: buy on a break of the 20-day high
(or 55-day for System 2), exit at whichever comes first of a fixed
`2 · ATR(20)` stop below entry or a trailing 10-day low (20-day for System
2), no profit target, no pyramiding. Single-asset runs use Kestrel's
`breakout` (`setup = "range"`, `atr_stop = 2`, `atr_bars = 20`, `trail_bars =
10` or `20`, `target_multiple = 0`); multi-asset runs simulate a shared
account with Kestrel's intra-bar OHLC walk at 5 bps a side for ETFs and 7 bps
for crypto:

| 5-asset portfolio (SPY, QQQ, GLD, SLV, BTC/USD), Jan 2016 – Sep 2026 | Return | CAGR | Vol. | Sharpe | Worst fall | Avg / peak exposure |
|---|---|---|---|---|---|---|
| Video rules on CFDs: 2% of high-water mark, uncapped (5%/yr swap) | +2,957% | +37.5% | 31.9% | 1.16 | −40.9% | 181% / 584% |
| 1× spot: 2% of high-water mark, first-come to 100% cash | +306% | +13.9% | 15.5% | 0.92 | −22.5% | 67% / 100% |
| 1× spot: 2% of equity, capped at 25% per asset | +578% | +19.5% | 14.2% | 1.32 | −18.2% | 52% / 100% |
| Hold the same 5 assets equally (rebalanced yearly, 1×) | +2,991% | +37.6% | 25.5% | 1.38 | −40.5% | 100% |
| Hold 45% in the 5 assets + 55% cash (same volatility) | +564% | +19.3% | 13.8% | 1.34 | −27.1% | 45% |

- **The video's returns come from hidden CFD leverage and Bitcoin, not ETF
  timing:**
  - A `2 · ATR(20)` stop on SPY or GLD is ~2% wide, so risking 2% of the
    high-water mark puts ~100% of equity into a single ETF and peaks at
    5.8× notional exposure across the five assets (paying ~$759k in swap
    fees on 100,000 USD, with 2018, 2021 and 2022 down and BTC at a 2.98 profit
    factor, matching the video's 3.01). The video's 16:30–18:00 ET flatten
    merely dodges the 17:00 ET CFD rollover fee; spot ETFs and spot BTC pay
    no swap fee.
  - At 1× spot, holding the same five assets equally matched the 5.8× CFD
    return (+37.6% CAGR, Sharpe 1.38), and a 45/55 asset/cash mix matched
    1× spot Turtle (+19.3% against +19.5% CAGR, Sharpe 1.34 against 1.32),
    with Turtle only trimming the worst fall (−18.2% against −27.1%).
- **Zero timing edge on ETFs; likely edge on BTC (`kestrel_app backtest
  breakout`, 2016–2026):**
  - SPY (67 trades, PF 2.97), QQQ (76, PF 1.86), GLD (63, PF 2.07) and SLV
    (70, PF 1.28) look profitable only because sitting long 34–53% of days
    captures upward drift. Tested individually against `exposure × hold`,
    none of 10 macro ETFs (SPY, QQQ, GLD, SLV, TLT, DBC, USO, EFA, EEM, UUP)
    beat drift with confidence (+1.90 bps/day on USO, t = +0.83, down to
    −1.91 bps/day on EFA, t = −1.91; |t| ≤ 0.86 on the other eight; a
    20-day block bootstrap gives the same t-values within 0.15). Filling
    daily levels on `30m` bars (`--fill-timeframe 30m`, Mar–Nov 2026) gave
    the same trades and within 1 bp of `1d` fills.
  - Without BTC, the 4-ETF 1× spot portfolio trailed `50% 4-ETFs + 50% cash`
    on Sharpe in both 2016–2022 (0.54 against 0.72) and the 2023–2026
    holdout (1.22 against 1.38). A 9-ETF macro basket (SPY, QQQ, EFA, EEM,
    TLT, GLD, SLV, DBC, UUP) also trailed passive holding on Sharpe in both
    periods across System 1 (`20/10`), System 2 (`55/20`) and 4-unit
    pyramiding (which fell to +7.9% total in 2016–2022).
  - **BTC/USD** (Kraken, 2016–Sep 2026) was the one asset to beat
    exposure-matched holding (+12.46 against +5.66 bps/day, +6.80 bps/day;
    t = 2.37 on daily returns and 2.33 with a 20-day block bootstrap, so the
    trades' runs of days do not inflate it; but it is one of 11 assets
    tested, so treat it as likely rather than proven, as the crypto trend
    literature also suggests): PF 4.32 on `20/10`
    (79 trades) and 7.82 on `55/20` (42 trades), already runnable as
    single-symbol `breakout`.
- **11 Binance.US USDT pairs (Jan 2022 – Dec 2024 vs Jan 2025 – Nov 14 2026
  holdout, risk-matched against `trend_ensemble`):**
  - The Python Turtle simulation was checked coin by coin against
    `kestrel_app backtest breakout` over 2022–2026 (10,000 USDT, fixed 100
    USDT risk): 10 of 11 coins matched exact trade counts and first-entry
    dates, with returns within 0.7 percentage points (BNB: 43 trades against
    42, and 1.0 point apart). This checks the entry, stop and exit rules; the
    shared account's sizing and per-coin caps are the simulation's own.
  - **Inside Kestrel (`kestrel_app backtest trend_ensemble`, 1,000 USDT, 7
    bps a side):** `lookbacks = [20]`, `[55]` and `[20, 55]` beat the
    default nine lookbacks on Sharpe in the holdout, with and without ZEC,
    and `[20]` and `[20, 55]` in 2022–2024 too (`[55]` trailed there), at
    4–10× fewer fills. Fewer lookbacks diversify less, so they ran at about
    9–10% volatility against 7–8%, and which single lookback did best
    flipped between the periods:

    | `trend_ensemble` in C++ | 2022–2024 (11 coins) | Holdout 2025–2026 | Holdout without ZEC | Fills, both periods |
    |---|---|---|---|---|
    | Default 9 lookbacks | +32.0%, 7.0% fall, Sharpe 1.23 (7.8% vol) | +2.1%, 5.7% fall, Sharpe 0.20 | −2.6%, 8.1% fall, Sharpe −0.17 | 5,274 |
    | `lookbacks = [20]` | +51.0%, 8.9% fall, Sharpe 1.44 (9.9% vol) | +8.4%, 8.4% fall, Sharpe 0.54 | +1.6%, 13.1% fall, Sharpe 0.14 | 1,000 |
    | `lookbacks = [55]` | +37.9%, 11.0% fall, Sharpe 1.14 (9.9% vol) | +11.1%, 5.3% fall, Sharpe 0.70 | +4.1%, 8.9% fall, Sharpe 0.29 | 504 |
    | `lookbacks = [20, 55]` | +43.4%, 9.9% fall, Sharpe 1.31 (9.5% vol) | +10.1%, 5.1% fall, Sharpe 0.67 | +3.1%, 10.8% fall, Sharpe 0.24 | 1,241 |

  - **Volatility-matched Turtle vs `trend_ensemble` (paired 20-day circular
    block bootstrap, 10,000 replicates):** calibrating Turtle's `risk_pct` on
    2022–2024 to match `trend_ensemble`'s 7.8% volatility (`0.153%` for
    `20/10`, `0.154%` for `55/20`, `max_weight` 9%, locked for 2025–2026;
    same period benchmarks: BTC −26.9% / 53% fall, 20% BTC + 80% cash −5.4% /
    13.3% fall) gives the same drawdowns as `trend_ensemble` (−6.3% to −7.1%
    in 2022–2024; −3.6% to −7.2% in the holdout). In the holdout `20/10` made
    +15.9% (−7.0% fall, Sharpe 1.09) with ZEC and +3.5% (−7.2% fall, Sharpe
    0.39) without ZEC (`55/20`: +14.4% and +2.3%, −3.6% fall), ahead on point
    estimates, but no Sharpe gap is significant at 5% (holdout ΔSharpe for
    `20/10` +0.89, 95% CI [−0.08, +1.94], p = 0.085 with ZEC; +0.56, 95% CI
    [−0.28, +1.27], p = 0.15 without ZEC; `55/20` p = 0.20 and 0.24;
    2022–2024 p = 0.22–0.87). The bootstrap pairs each day's returns of
    the two strategies (correlation 0.7–0.9); a first run paired each
    `trend_ensemble` day with the next day's Turtle return (the equity
    CSV stamps a daily bar at its close) and gave p = 0.11–0.91.
  - **Parameter grid (8 lookback pairs from `10/5` to `80/40` × 3 risk
    levels = 24 cells):** without ZEC, 2022–2024 Sharpe ranged from +0.84 to
    +1.43 (median +1.17) and 2025–2026 holdout Sharpe from −0.13 to +0.43
    (median +0.26, 19/24 positive); with ZEC the holdout ranged from +0.27
    to +1.16 (median +0.97, all 24 positive). Which cells did best did not
    persist between periods (Spearman ρ = −0.04 with ZEC, +0.07 without).
  - **Why not build a separate multi-coin Turtle:** at the same risk its
    drawdowns match `trend_ensemble`'s and no Sharpe gap is significant,
    ZEC drives most of its holdout gain, which lookbacks do best does not
    persist between periods (ρ ≈ 0), and Kestrel already runs the Turtle on
    one coin with `breakout` and 20- and 55-day channels on many with
    `trend_ensemble` (`lookbacks = [20, 55]`; its entry on the highest
    close, midpoint stop and volatility sizing differ from the Turtle's).
  - **What it does suggest:** every check here (risk-matched Turtle, the
    24-cell grid, and `trend_ensemble` with fewer lookbacks) did better
    than the default nine lookbacks in the 2025–2026 holdout, on point
    estimates. Choosing lookbacks after seeing the holdout would be fitting
    it, so the paper's defaults stay in the paper run; a test on Kraken's
    longer history (2016 on) would show whether the shortest and longest
    channels are a drag outside this sample.

## Stock spreads at the open

Kestrel's daily stock strategies send their orders at the session's open
(`OnSessionOpen`), so a fill pays the spread at 09:30 ET, not the mid-day
spreads quoted above. Median bid-ask spreads in the recorded Schwab
quotes, in bps of the mid, by time of day (ET). Five sessions have the
open recorded (Nov 18–24 2026), six the rest of the day, so these are a
first look:

| Symbol | 09:30:00–09:30:15 | 09:30–09:31 | 09:31–09:35 | 09:35–10:00 | 11:00–14:00 | 15:50–16:00 |
|---|---|---|---|---|---|---|
| SPY | 0.68 | 0.68 | 0.56 | 0.55 | 0.42 | 0.27 |
| QQQ | 1.09 | 0.94 | 0.91 | 0.78 | 0.63 | 0.46 |
| IWM | 1.13 | 1.13 | 0.76 | 0.76 | 0.75 | 0.38 |
| GLD | 1.47 | 1.48 | 1.21 | 0.92 | 0.62 | 0.57 |
| XLK | 8.21 | 6.07 | 3.18 | 1.85 | 1.23 | 0.62 |
| XLV | 8.85 | 7.80 | 3.90 | 2.23 | 1.11 | 0.56 |
| AAPL | 4.81 | 4.24 | 3.71 | 2.53 | 1.44 | 0.86 |
| NVDA | 4.20 | 3.78 | 2.83 | 1.93 | 1.32 | 0.78 |
| MSFT | 9.60 | 7.92 | 6.13 | 3.85 | 2.61 | 1.25 |
| TSLA | 11.71 | 10.09 | 7.36 | 5.98 | 3.93 | 2.08 |
| BA | 28.62 | 27.85 | 15.94 | 11.21 | 7.43 | 2.47 |
| MMM | 103.26 | 69.28 | 44.07 | 16.66 | 8.16 | 3.87 |

- **The first minute's spread is a median 1.9 times mid-day's on the 13
  ETFs** (1.0 to 7.0 times) **and 2.9 times on the stocks** (1.5 to 8.5).
  It narrows over the first half hour and is narrowest at the close.
- **A market order pays half the spread.** At the open that is under
  1 bps on SPY, QQQ and IWM and 2–5 bps on the sector ETFs and the
  largest stocks (TSLA about 6), about the usual 5 bps of slippage. Not
  on mid-caps: BA about 14 bps, DIS about 15 and MMM about 35–50 at the
  open (their whole mid-day spreads are 3–8 bps), and the small caps
  (AKAN, CLRO, FCUV) pay 60–290.
- **For the daily stock tests,** 5 bps a side stays fair for the ETFs and
  the largest stocks. A strategy that trades mid-caps at the open should
  use each symbol's open spread, or fill a few minutes later
  (09:35–10:00 spreads are a quarter to four fifths of the first
  minute's, about half on the stocks). Pre-market
  quotes (09:28–09:30) are wider again on most of them (XLV 47 bps).

## Paper runs

- **Market maker, Binance.US:** an 8-hour paper session (traded, recorded
  and stopped cleanly) and live runs on ZEC/USDT in Oct–Nov 2026. Run 12
  is the one with stored quotes; see the live check above.
- **DKI, `breakout` range 5m (Schwab, one session):** no trades.
- **ZEC/USDT and SOL/USDT, `breakout` range 1h with `trend_sma = 1000`
  (runs 17 and 16, Nov 7–11 2026):**
  - SOL was below its average throughout, so it made no entries.
  - ZEC entered 0.07 ZEC at 1289.23 on Nov 8, its only trade, and lost
    3.82 USDT on it. It was below its average from then on.
- **Restarted with risk limits (runs 19 ZEC, 20 SOL and 21 BTC, from Nov
  11 2026):** the same settings, plus `max_loss_per_position` 15, 15 and
  10 USDT, `max_daily_loss` 15 and `max_consecutive_losses` 3, from the
  losing-streak test above. Each trade is 70–76 USDT of a 100 USDT
  account.
  - BTC/USDT has no trend filter. It is there to exercise live orders,
    fills, stops and exits now, while the market is below its averages.
    Expect a small loss: unfiltered, range 1h lost 0.05% a trade.
- **`trend_ensemble`, the 11 USDT pairs (run 45, from Nov 19 2026):** the
  paper's defaults on a 1,000 USDT paper account, large enough for its
  small orders to clear Binance.US's 1 USDT minimum. Judge it against 20%
  BTC and 80% cash over the same days, not against holding BTC; months of
  it will mostly show whether the orders and restarts work. Run 44 (from
  23:35 UTC Nov 18) held cash until it was restarted as run 45 at 02:06
  UTC Nov 19 on the build with the second Gemini review's fixes.
