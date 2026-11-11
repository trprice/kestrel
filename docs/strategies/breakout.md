# Breakout

`breakout` trades the break out of a bar pattern, after Fidelity's
"Identifying Chart Patterns" deck (Kirkpatrick). It waits for a setup, enters
when the price breaks above the setup's high, protects the trade with a stop
below the setup's low, and takes profit at the setup's height above the
entry. With `direction = "short"` it does all of this upside down.

Source: `include/kestrel/strategy/breakout.hpp`, `src/strategy/breakout.cpp`;
the pattern detectors are in `include/kestrel/strategy/patterns.hpp`. Results
are in [strategy-tests.md](../strategy-tests.md): [crypto](../strategy-tests.md#breakout-on-crypto-mayoct-2026) and [stocks](../strategy-tests.md#breakout-on-stocks).

## How a trade goes

The description below is of the long side; for a short, read it with every
price mirrored (see [Shorts](#shorts)).

**Flat.** On each closed bar the strategy looks for its setup. A setup arms an
entry, provided that with `trend_sma` the close is above its average. The
entry is a buy-stop one tick above the setup's high, raised by
`confirm_percent` with `confirm = "percent"`. With `confirm = "closes"` it is
a watch instead, which buys at market once `confirm_closes` closes in a row
are above the trigger. An entry that has not triggered within `arm_bars` bars
is cancelled, and the next setup can arm.

**Throwback entries.** With `entry = "throwback"` the breakout is not bought
but waited for. First comes a close above the trigger within `arm_bars`
(`confirm_closes` of them with `closes`, above the raised trigger with
`percent`). Then a post-only limit buy rests at the level broken, the setup's
high, for `throwback_bars` bars. It fills only if the price comes back to
that level, and then as a maker: no slippage, and the maker fee (zero on many
Binance.US pairs) instead of the taker fee. The stop and target sit where they
would for a stop entry, so the target is the same height above the lower
fill. A breakout that runs without looking back is missed, which is the
deck's caveat that throwbacks do not always occur.

**Long.** When the entry fills, a sell-stop rests at the setup's low less
`stop_buffer_bps` (with `atr_stop`, that many ATRs under the level broken).
The target is checked on closes: a bar closing at or above it sells at market,
cancelling the stop first, because a spot account cannot hold the same shares
for two sell orders. The `max_hold_bars`-th whole bar held does the same, and
so does the session's closing hook with `flatten_at_close`.

**Resting target.** With `resting_target`, the target rests at the venue as a
sell limit paired with the stop, one-cancels-other
(`TradingVenue::PlaceOco`). Whichever fills first, the venue closes the
other. If the target fills only in part, the pair is taken down and the rest
sold at market. The paper venue and Binance.US have such pairs. On a venue
without them, or when the venue refuses the pair (the price already past the
target, or a risk limit the pair's two orders break), the stop rests alone and
the target is left to the close for the rest of the trade. A pair whose
placement got no answer may be resting, so nothing else is placed until the
next bar; a pair that came back half closed is taken down and tried again
then.

**Moving the stop.** With `breakeven_r` or `trail_bars`, a close that is not
an exit can raise the stop. The resting stop is cancelled and placed again at
the new level. A raised stop is never lowered, and never placed at or above
the close. While the market is closed (daily bars, or a bar that closes the
session) the new stop is placed at the next open. If the venue refuses it
there (an open that gapped below it), the position is sold at market, as it
is when a stop to be placed again has been closed through.

**Volume.** With `volume_ratio`, the first close above the trigger (of
`confirm_closes`, or the throwback's) must come on a bar that traded at least
`volume_ratio` times the average volume of the `volume_bars` bars before it.
A close on less does not count, as if it had closed below. Bars without volume
never confirm. A buy-stop fills before its bar's volume is known, so this
needs `confirm = "closes"` or `entry = "throwback"`.

**Orders that go wrong.** Orders are tracked through `OnFill` and
`OnOrderUpdate`:

- An entry that closes unfilled returns to flat; one that closes partly
  filled (a live venue) is held as a smaller long, with its stop.
- A protective stop that closes unfilled, or could not be placed, is placed
  again at the next bar.
- An entry whose cancel fails stays armed, and the cancel is retried each bar.
- When the stop or the exit sells out the position, the other is cancelled.

## Markets that close

On a market that closes (Schwab), with intraday bars:

- Nothing is armed while the market is closed, and an armed entry is
  cancelled in the closing hook, so a setup never carries into the next
  session's open.
- A target or time exit due on a bar that closes with the market (the 15:30
  bar, at 16:00) waits for the next bar, the stop resting meanwhile.

Daily (and longer) bars all close after the market:

- A setup is pending until the next open, where the entry is placed; a
  buy-stop the open gaps through fills at the open.
- An untriggered entry is cancelled at each close and placed again at the next
  open while `arm_bars` remain.
- A target or time exit is sold at the next open.

## Shorts

With `direction = "short"`, every setup is turned upside down: a range's low
broken, a double top, a head-and-shoulders top, a descending triangle, an
inverted cup. The trade is sold short below the setup's low, with the stop
above its high and the target below the entry. The strategy does this by
negating prices on the way in (bars) and out (orders) and back on fills, so
the same setup code serves both sides.

A short needs a venue that can sell short: the paper venue with
`[paper] allow_short` (see [configuration.md](../configuration.md#paper)).
Binance.US, the live venue here, refuses a sale of what the account does not
hold.

**Stop-and-reverse.** With `reverse = true`, a failed breakout is traded the
other way. When the protective stop, never moved, closes the whole trade, the
strategy enters the other direction at once at market. The new stop is
`stop_buffer_bps` beyond the level the failed trade broke, and the target is
the setup's height times `target_multiple` away. A reversed trade does not
reverse again, and a raised or trailed stop, a target or a time exit never
reverses. A late fill of the failed trade's orders after a reversal is logged
and ignored.

## The setups

Each setup arms a buy-stop one tick above the level shown, with the protective
stop `stop_buffer_bps` below the low shown and the target the height shown
above the fill.

| Setup | Pattern | Level | Low | Height |
|---|---|---|---|---|
| `nr` | the newest bar narrower than each of the `nr_bars - 1` before it | its high | its low | its range |
| `inside` | the newest bar inside the bar before | its high | its low | its range |
| `range` | the high and low of the last `range_bars` bars | the high | the low | the range |
| `rectangle` | that range, once the price has touched its high and its low `min_touches` times each | the high | the low | the range |
| `pipe` | a pipe bottom: after a `decline_bars` low, a wide bar closing in the lower quarter of its range, then a wide bar closing in the upper half of its own | the second bar's high | the lower low | both bars |
| `failed_breakdown` | the long side of the deck's false breakout: a close back above the low of the `range_bars` range, the bar after a close below it | that bar's high | the false low | the range |
| `double_bottom`, `triple_bottom` | in the last `range_bars` bars, the newest 2 (or 3) swing lows, each pair separated by a swing high and all within `touch_zone` of the pattern's height of each other, not yet broken out above the neckline (the highest high between the first and last low; for a triple, the higher peak) nor failed below the lows; no prior decline is required, and a formed triple also counts as a `double_bottom` of its newest two lows | the neckline | the lowest low | neckline to lowest low |
| `inverse_head_shoulders` | in the last `range_bars` bars, the newest 3 swing lows, a swing high between each pair, the middle (the head) below each shoulder by more than `touch_zone` of the height, and each shoulder at least half the height below the neckline. The neckline runs through the highest high between each shoulder and the head, so it may slope; the level is where it will be at the next bar (or the newest bar's high, if higher), rounded up to the tick, and stays there while the entry is armed. No bar since the neckline's second point above it yet, and none since the right shoulder below it by more than the zone | the neckline | the head | the neckline at the head to the head |
| `ascending_triangle` | in the last `range_bars` bars, the newest `min_touches` swing highs and swing lows, overlapping in time: the highs within `touch_zone` of the height of the highest, the lows each above the one before, and the lows' least-squares line still under the highs at the next bar. Not yet broken out above the highest high nor failed below the last low by more than the zone | the highest high | the newest swing low | lowest low to highest high |
| `symmetrical_triangle` | the same, but the highs each below the one before, and the resistance the least-squares line through them; the level is that line at the next bar (or the newest bar's high, if higher), rounded up to the tick | the line | the newest swing low | lowest low to highest high |
| `cup_handle` | in the last `range_bars` bars, two swing highs (the rims) within `touch_zone` of the depth of each other, with a rounded cup between: no high above the lower rim, and at least two-fifths of its bars with lows in the bottom third of its depth (a V is not a cup). Every bar since the right rim is the handle: at most half as many bars as the cup, pulled back from the rim by more than the zone but staying in the upper half of the depth, and not yet above the rim. If several rims make one, the deepest cup is used. Mainly a daily-bar pattern; give `range_bars` room for the whole cup | the right rim | the handle's low | the cup's depth |
| `opening_range` | the session's first `opening_bars` bars, on the close of the last of them (once a session) | the range's high | the range's low | the range |

A sloping neckline or triangle line moves on while the entry is armed, so with
`arm_bars` above 1 the trigger stays the one worked out at the setup.

**The opening range.** The session is the data source's
(`Context::CurrentOrNextSession`; with Schwab's `extended_hours`, from the
pre-market open), and on a market that never closes it is the UTC day. The
range is the first `opening_bars` bars to open at or after the open, counted
rather than timed:

- A bar straddling the open is skipped, so `1h` bars on a 09:30 open make the
  range from 10:00.
- A missing bar (a minute with no trades) delays the setup by a bar. Bars
  missing at the open itself start the range at the first bar that does
  open: with `1m` bars and no trade until 09:35, the range runs from 09:35.
- A run started after a session's range has formed waits for the next
  session.

Its entry lasts `arm_bars` bars like any other. For the usual opening range
breakout, set `arm_bars` to the session's remaining bars and
`flatten_at_close = true`.

**Departures from the deck.** Every setup, the inside bar included, triggers
one tick above its high; the deck buys an inside bar on an open above the
inside bar's close. The target is measured from the entry's fill price rather
than the breakout level, so an open gapping through the trigger raises the
target with it.

**Backtests.** A bar-only backtest fills the entry, the stop and a resting
target inside the bar at their own prices (see `RunBacktest`).

## Parameters

In the `[strategy]` table (see [configuration.md](../configuration.md#strategy)).

| Key | Meaning |
|---|---|
| `symbol`, `quantity` | required; `quantity` is base units (shares) per entry, or the most an entry buys with `risk_per_trade` |
| `risk_per_trade` | size each entry so that a fill at the entry and a sale at the protective stop lose this much, in the quote asset, before fees and slippage (default 0: every entry buys `quantity`). The size is this over the entry's distance to the stop, rounded down to the lot and capped at `quantity`; an entry under the venue's minimum size or value is skipped. A stop-entry is sized from its trigger, a throwback from its limit, a market entry from the last close, so a gap past the trigger or the stop loses more |
| `timeframe` | the bars it trades (default `1h`; Schwab streams `30m` or shorter for `run`, and backtests also take `1d`) |
| `setup` | `nr`, `inside`, `range`, `rectangle`, `pipe`, `failed_breakdown`, `double_bottom`, `triple_bottom`, `inverse_head_shoulders`, `ascending_triangle`, `symmetrical_triangle`, `cup_handle` or `opening_range` (default `nr`); see [the setups](#the-setups) |
| `direction` | `long` (default) or `short`; see [Shorts](#shorts) |
| `reverse` | stop-and-reverse on a failed breakout (default `false`); see [Shorts](#shorts) |
| `nr_bars`, `range_bars` | 4 (NR4, at least 2) and 20 |
| `touch_zone` | a fraction of the pattern's height (default 0.1). `rectangle`: a bar touches a bound when it comes within it of the bound (consecutive touching bars count once). `double_bottom`, `triple_bottom`: how far apart the lows may be, and how far below them the price may go before the pattern has failed. `inverse_head_shoulders`: how far the head must be below each shoulder, and how far below the right shoulder the price may go. Triangles: how flat an ascending triangle's highs must be, and how far below the last swing low the price may go. `cup_handle`: how far apart the rims may be, and the least the handle must pull back, as a fraction of the cup's depth. Must be above 0 and below 0.5 |
| `min_touches` | `rectangle`: touches needed of each bound; triangles: the swing highs and the swing lows used, at least 2 (default 2) |
| `decline_bars` | `pipe`: the first bar's low must be below all of these bars before it, and both bars wider than their average range (default 10) |
| `swing_bars` | `double_bottom`, `triple_bottom`, `inverse_head_shoulders`, triangles, `cup_handle`: a swing point is beyond this many bars before it and no less extreme than this many after (default 2), so it is seen that many bars late. `range_bars` must be at least `2·swing_bars + 1 + (bottoms − 1)·(swing_bars + 1)` (8 for a double, 11 for a triple or a head-and-shoulders at the default); for a triangle, `2·swing_bars + 2 + (min_touches − 1)·(swing_bars + 1)` (9 at the defaults); for `cup_handle`, `4·swing_bars + 2` (10) |
| `opening_bars` | `opening_range`: the bars of the session's opening range (default 6: the first half hour of 5-minute bars). Intraday bars only |
| `reclaim_bars` | `failed_breakdown`: the range is the `range_bars` bars before the newest `reclaim_bars + 1`, so the break below it must start within these bars (default 3) |
| `shortfall` | `rectangle`: also require the deck's shortfall: since the last touch of the top, a fall below the middle that turned before reaching the bottom (default `false`) |
| `confirm` | `none` (a buy-stop one tick above the high), `closes` (buy at market after `confirm_closes` closes above it) or `percent` (a buy-stop `confirm_percent` % above the high) (default `none`) |
| `confirm_closes`, `confirm_percent` | 2 and 0.5 |
| `entry` | `stop` (enter as `confirm` says) or `throwback` (after a close above the trigger, rest a post-only limit buy at the level broken (the setup's high; for `pipe` and `failed_breakdown`, the high of the bar the entry triggers above) for `throwback_bars` bars) (default `stop`). With `throwback`, `confirm` decides the close: with `closes`, `confirm_closes` of them in a row; with `percent`, above the raised trigger. The first must come within `arm_bars` bars of the setup, so with the default of 1 a breakout that closes above it only two bars later is missed: raise `arm_bars` to wait longer |
| `throwback_bars` | bars the throwback's limit rests before it is cancelled (default 3) |
| `stop_buffer_bps` | how far below the setup's low the protective stop sits (default 10) |
| `atr_stop`, `atr_bars` | instead, put the protective stop `atr_stop` average true ranges (Wilder's, over `atr_bars` bars, measured at the setup) below the level broken, so its distance follows the market's volatility rather than the setup's shape (defaults 0: off, and 14). The ATR is smoothed over the bars loaded, so it asks for `6 · atr_bars + 1` of warmup. A setup whose ATR stop would fall to zero or below arms nothing. `breakeven_r` measures the risk from this stop, `trail_bars` can still raise it, and the target is still the setup's height. A tight stop can sit above a `confirm = "closes"` market entry that gaps down: the venue then refuses the stop or fills it at once; without a stop in place, the next close at or below it sells at market, and a close above it places the stop again |
| `target_multiple` | the target is the entry plus this many times the setup's height (default 1; 0: none) |
| `resting_target` | rest the target at the venue, paired with the stop (default `false`); see [Resting target](#how-a-trade-goes) |
| `arm_bars` | bars an untriggered entry stays (default 1); with `closes`, bars for the closes to start |
| `max_hold_bars` | exit after this many whole bars held, not counting the bar the entry filled inside (default 0: never) |
| `trend_sma` | enter only while the close is above this many bars' average close (default 0: off) |
| `flatten_at_close` | sell in the closing hook, before the session closes (default `false`) |
| `volume_ratio`, `volume_bars` | volume confirmation (defaults 0: off, and 20); see [Volume](#how-a-trade-goes) |
| `breakeven_r` | once a bar closes this many times the trade's risk (the entry less the first stop) above the entry, the stop is raised to the entry (rounded down to the tick), so the trade loses little more than fees, unless the price gaps through it (default 0: off). With the default `target_multiple` of 1 the target usually comes first, as the risk is about the setup's height |
| `trail_bars` | on each close, raise the stop to the lowest low of the last `trail_bars` bars less `stop_buffer_bps` (default 0: off) |

`stop_buffer_bps`, `confirm_percent`, `target_multiple`, `volume_ratio`,
`breakeven_r`, `atr_stop` and `risk_per_trade` cannot be negative, and the bar counts must be
positive.
