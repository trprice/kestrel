# Writing a strategy

A strategy is a class derived from `strategy::Strategy`
(`include/kestrel/strategy/strategy.hpp`). The runner calls its hooks; the
strategy reacts by placing orders. `MaCrossover` is a small worked example,
`MarketMaker` a larger quote-driven one, `Breakout` a bar strategy with
resting stops and session handling, and `MomentumRotation` one that trades
several symbols at once. See [design.md](design.md) for where the runner,
the risk gate and the venue sit.

## 1. Declare what you need

`Needs()` returns a `Requirements`, which the runner reads once before it
subscribes to anything:

| Field | Meaning |
|---|---|
| `symbols` | the symbols the strategy trades |
| `bar_timeframes` | closed-bar streams to subscribe to for every symbol (none by default) |
| `quotes` | whether to receive `OnQuote` |
| `warmup_bars` | closed bars kept per series and loaded before `OnStart`, so indicators start with a full window |
| `timer_interval` | how often to call `OnTimer` (zero, the default: never) |
| `state` | whether to keep state across restarts: `Context::State()` then returns a store (`run` with a `[database]`; null in a backtest or without one) |

A backtest replays stored quotes only if `quotes` is true, and fetches the
warmup bars plus the range from the database or the provider.

## 2. Implement the hooks

All hooks are optional except `Name()` and `Needs()`.

| Hook | Called |
|---|---|
| `OnStart` | once, after the warmup bars are loaded and before the first event; every order that was open on the venue's symbols has been cancelled by then, so start from your balances, not from resting orders |
| `OnBar` | for each closed bar, in order; bars missed in a disconnect are backfilled |
| `OnQuote` | for each quote, if `quotes` is set; quotes are conflated, so you may skip intermediate ones |
| `OnFill` | once per fill of an order you placed, partial fills included |
| `OnOrderUpdate` | when an order you placed changes state or fills further, ending with a closed state (filled, cancelled, rejected, expired) |
| `OnSessionOpen` | on a market that closes (US stocks on Schwab), when a session opens; not for a session already open when the runner starts |
| `OnSessionClosing` | `RunnerOptions::session_closing_lead` (5 minutes) before a session closes, with the close time, while orders are still accepted: the time to flatten; right after `OnStart` if the runner starts in that window |
| `OnTimer` | every `Requirements::timer_interval` of the runner's clock (simulated in a backtest), whether or not anything arrives; for acting on time passing, such as a feed gone quiet. A tick due when an event arrives runs just before it, and a late one is not made up |
| `OnStop` | once at shutdown, after the streams are closed; before it, `run` cancels open orders that would open or add to a position and keeps those that close it (a protective stop), so cancel any others you want gone |

Every hook gets a `Context&`, valid only during that call:

- `Now()`: the runner's clock (simulated in a backtest). Use it instead of
  the system clock.
- `Venue()`: the `TradingVenue` to trade through. Place and cancel orders
  here, never on a venue you obtained elsewhere: this one carries the risk gate
  and the order tracking that feeds `OnOrderUpdate`.
- `SymbolInfo(symbol)`: tick size, lot size, minimum size and minimum order
  value, for rounding prices and quantities.
- `Bars(symbol, timeframe)`: the last closed bars, oldest first, up to
  `warmup_bars`. The span is invalidated by the next bar for that series, so
  do not keep it. `strategy/patterns.hpp` computes indicators from it (`Sma`,
  `Ema`, `Rsi`, `Atr`, `Bollinger`, `Macd`) and finds bar patterns (narrow
  range, inside bar, ranges, swing points, double bottoms). The smoothed
  indicators settle over a few times their period, so ask for that many
  `warmup_bars`.
- `State()`: a `StateStore` (`Load`, `Save`, `Erase` of a text value by
  key) kept across restarts, when `Requirements::state` asked for one and
  the run has a database; null otherwise. The market maker keeps its halt
  and loss-cap P&L there.
- `MarketOpen()`: whether the market can be traded now, and
  `CurrentOrNextSession()`: the session open now, or the next one. Always open
  (and no session) on a crypto venue.
- `Log()`: the strategy's `LogContext` (`Debug`, `Info`, `Warn`, `Error`, plus
  `With(key, value)` for scoped child contexts). It automatically prepends the
  strategy name and `symbol=<symbol>` (for single-symbol strategies) plus any
  fields set on `RunnerOptions::log`. Use `context.Log()` instead of calling
  `spdlog` directly. A single-symbol strategy can log through
  `context.Log().WithDefault("symbol", symbol)` and leave the symbol out of
  its messages: the prefix carries it, in a runner or not. Field values with
  a space, `=`, a quote, a backslash or a control character are quoted.

```cpp
class MyStrategy : public strategy::Strategy {
   public:
    std::string_view Name() const override { return "my_strategy"; }

    strategy::Requirements Needs() const override {
        strategy::Requirements needs;
        needs.symbols = {mSymbol};
        needs.bar_timeframes = {types::Timeframe::kH1};
        needs.warmup_bars = 50;
        return needs;
    }

    void OnBar(strategy::Context& context, const types::Symbol& symbol,
               types::Timeframe, const types::Bar&) override {
        const auto bars = context.Bars(symbol, types::Timeframe::kH1);
        if (!ShouldBuy(bars) || mPending) {
            return;
        }
        types::OrderRequest request;
        request.symbol = symbol;
        request.side = types::Side::kBuy;
        request.type = types::OrderType::kMarket;
        request.quantity = mQuantity;
        auto ack = context.Venue().PlaceOrder(request);
        mPending = ack.has_value();
    }

    void OnFill(strategy::Context&, const types::Fill&) override {
        mPending = false;  // update your own position here
    }
};
```

A strategy that holds one long position in one symbol, entered and left by
market orders, can leave the order handling to `SingleOrderPosition`
(`include/kestrel/strategy/single_order_position.hpp`), as `ma_crossover` and
`mean_reversion` do. It sends one order at a time, counts the position from
the fills (an order filled in parts is one entry or exit), takes a fee in the
coin off what is held and sells in whole lots. Pass it every `OnFill` and
`OnOrderUpdate`; it reports when the position was entered or exited. Smaller
helpers (`Submit`, `RoundDownToLot`, `ReceivedOnBuy`, `ClosedUnfilled`) are in
`include/kestrel/strategy/orders.hpp`.

## 3. Rules to follow

- **Threading:** every hook runs on the runner's one strategy thread, one at a
  time, so the strategy needs no locks. Do not start your own threads that call
  the venue or the context.
- **Be quick:** events queue up behind a slow hook. Quotes are conflated;
  bars are not, and past `RunnerOptions::max_queued_bars` (1000) a bar is
  dropped and backfilled later, so a hook that keeps falling behind sees
  bars late. On a live venue every `PlaceOrder` and `CancelOrder` is an
  HTTP round trip (about 0.13 s on Binance.US once the connection is open)
  on this thread. Polling for
  fills does not use this thread.
- **Exceptions:** a hook that throws is logged and the runner moves on to the
  next event. Do not rely on that; handle errors where they happen.
- **Check every result:** `PlaceOrder` returns a `Result`. It can fail (a risk
  limit, the venue, an order under the symbol's minimum), or succeed with an
  acknowledgement that is already closed (for example an unfillable
  fill-or-kill order). Neither produces a fill, so do not wait for one.
- **Market hours:** on a market that closes, orders placed while it is closed
  are refused (kRejected, "market closed: ..."), in paper, live and backtests
  alike; cancels always go through, and the paper venue fills nothing until
  the open. The session is the regular one (09:30 to 16:00 New York time for
  US stocks, 13:00 on an early close), or with `[schwab] extended_hours` also
  07:00 to 09:30 and 16:00 to 20:00. A bar that closes at the session's close
  (the 15:30 30-minute bar, at 16:00) arrives after it, so an order it
  prompts is refused: act on the bar before, or in `OnSessionClosing`.
- **Fills arrive late:** `OnFill` always comes after the fill, even from a
  venue that pushes executions (an order that fills inside `PlaceOrder` is
  reported on a later event), and a polled venue can lag by
  `fill_poll_interval` (1 s by default). Track your own pending orders and
  your own position from `OnFill` rather than assuming an order filled when
  you placed it, and count a fill the venue has already reported (an
  acknowledgement in `kFilled`, a closing `OnOrderUpdate`) against your limits
  until its `OnFill` arrives, as `MarketMaker` does.
- **Cancels race with fills:** an order can fill while your cancel is in
  flight, and a polled fill can be queued for an order you are replacing. `CancelOrder` reports `kAlreadyFilled` or `kAlreadyClosed` for that,
  and it is a normal outcome, not an error. `MarketMaker` tracks the race as
  `cancel_raced`.
- **Round to the symbol's rules:** prices to the tick, quantities to the lot
  size, using `SymbolInfo` and `Decimal::RoundToIncrement`. The venue rounds
  too, but your own sizing should agree with what is sent.
- **Use `Decimal`** for prices and quantities, never `double`.
- **Spot only:** the paper venue does not short, so do not sell more than you
  hold.
- **The risk gate sits between you and the venue.** If `[risk]` is set, an
  order that breaks a limit comes back as an error. After a loss, drawdown or
  order-count limit trips, only orders that reduce a position are allowed.
  Only `max_drawdown_close` and `max_loss_per_position` close a position for
  you (see [configuration.md](configuration.md)); otherwise do that in the
  strategy, as `MarketMaker`'s protection rules do.

## 4. Register it

1. Add `src/strategy/my_strategy.cpp` and `include/kestrel/strategy/my_strategy.hpp`
   (and the source file to `CMakeLists.txt`, with a test under `tests/`).
2. In `src/factory.cpp`, write a `Build` function that reads the strategy's
   parameters from `[strategy]` with the `ParamReader` (`Symbol()`,
   `Quantity(key)`, `Timeframe(key, default)`, `Count(key, default)`, and so
   on), calls `p.Finish()` so an unknown parameter is an error, and returns
   the new strategy. A constructor that throws `std::invalid_argument` becomes
   a config error. Copy `BuildMaCrossover`.
3. Add `{"my_strategy", BuildMyStrategy}` to the `kStrategies` table. That one
   entry makes `kestrel_app run my_strategy` and `backtest my_strategy` work,
   and allows a `[risk.my_strategy]` section.
4. Document it in a page of its own under [strategies/](strategies/README.md):
   how it works, what Kestrel does for it, and its parameters; add it to the
   index there and to the table in
   [configuration.md](configuration.md#strategy). Keep the comments in the
   code to why it does what it does. `config/kestrel.example.toml` shows
   `ma_crossover`'s parameters only.

## 5. Test it

`tests/strategy_fakes.hpp` has `RecordingVenue`, a venue that records the
orders placed through it and can push executions, and `FakeTradeLog`. Each
strategy test defines a small `FakeContext` around them (see
`tests/ma_crossover_test.cpp` and `tests/market_maker_test.cpp`). Feed the
strategy bars or quotes and assert on the orders it placed. Then run a
backtest over stored data (`kestrel_app backtest my_strategy --from ... --to
...`). Use `limit_fill_model = "conservative"` for anything that rests limit
orders; the default `cross` model flatters those. Set `[paper]
order_latency_ms` and `report_latency_ms` to the live venue's round trips
(see [configuration.md](configuration.md#paper)): a strategy that cancels
and replaces orders can be filled while a cancel is in flight, and handles
fills that arrive after the cancel was acknowledged. Paper-trade it with `run`
before trusting the result, and see [TODO.md](../TODO.md) for what the paper
venue and backtest do not model.
