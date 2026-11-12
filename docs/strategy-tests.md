# Strategy tests

The backtests and paper runs done so far (Oct to Nov 2026), and what each
found. These are research notes, not tuned results: most tests tried
several settings and report the best, so treat each edge as an upper bound.

Unless a test says otherwise:
- **Crypto** is Binance.US, at its 2 bps taker and 0 maker fee, with 5 bps
  of slippage a side. That makes a round trip with market orders about
  14 bps. "The seven pairs" are BTC/USDT, SOL/USDT, ZEC/USDT, ETH/USD,
  ADA/USD, XRP/USD and HYPE/USD.
- **Stocks** use Schwab bars, with no commission, and 5 bps of slippage or
  each stock's estimated spread. Schwab keeps about 48 days of 1-minute
  bars and about 260 of 30-minute ones, so intraday stock tests cover
  weeks, not years.
- **"Hold"** is buying the same amount at the start and keeping it.

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
  (−0.29%).
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