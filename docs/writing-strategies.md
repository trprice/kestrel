# Writing a strategy

A strategy is a class derived from `strategy::Strategy`
(`include/kestrel/strategy/strategy.hpp`). The runner calls its hooks; the
strategy reacts by placing orders. `SmaCrossover` is a small worked example and
`MarketMaker` a larger one. See [design.md](design.md) for where the runner,
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
| `OnStop` | once at shutdown, after the streams are closed; `run` does not cancel open orders itself, so cancel the ones you want gone |

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
  do not keep it.

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

## 3. Rules to follow

- **Threading:** every hook runs on the runner's one strategy thread, one at a
  time, so the strategy needs no locks. Do not start your own threads that call
  the venue or the context.
- **Be quick:** events queue up behind a slow hook. Quotes are conflated, but
  bars never are. On a live venue every `PlaceOrder` and `CancelOrder` is an
  HTTP round trip (about 0.4 s on Binance.US) on this thread. Polling for
  fills does not use this thread.
- **Exceptions:** a hook that throws is logged and the runner moves on to the
  next event. Do not rely on that; handle errors where they happen.
- **Check every result:** `PlaceOrder` returns a `Result`. It can fail (a risk
  limit, the venue, an order under the symbol's minimum), or succeed with an
  acknowledgement that is already closed (for example an unfillable
  fill-or-kill order). Neither produces a fill, so do not wait for one.
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
  The gate never closes your position for you; do that in the strategy, as
  `MarketMaker`'s protection rules do.

## 4. Register it

1. Add `src/strategy/my_strategy.cpp` and `include/kestrel/strategy/my_strategy.hpp`
   (and the source file to `CMakeLists.txt`, with a test under `tests/`).
2. In `src/factory.cpp`, write a `Build` function that reads the strategy's
   parameters from `[strategy]` with the `ParamReader` (`Symbol()`,
   `Quantity(key)`, `Timeframe(key, default)`, `Count(key, default)`, and so
   on), calls `p.Finish()` so an unknown parameter is an error, and returns
   the new strategy. A constructor that throws `std::invalid_argument` becomes
   a config error. Copy `BuildSmaCrossover`.
3. Add `{"my_strategy", BuildMyStrategy}` to the `kStrategies` table. That one
   entry makes `kestrel_app run my_strategy` and `backtest my_strategy` work,
   and allows a `[risk.my_strategy]` section.
4. Document the parameters in `config/kestrel.example.toml` and in
   [configuration.md](configuration.md#strategy).

## 5. Test it

`tests/strategy_fakes.hpp` has `RecordingVenue`, a venue that records the
orders placed through it and can push executions, and `FakeTradeLog`. Each
strategy test defines a small `FakeContext` around them (see
`tests/sma_crossover_test.cpp` and `tests/market_maker_test.cpp`). Feed the
strategy bars or quotes and assert on the orders it placed. Then run a
backtest over stored data (`kestrel_app backtest my_strategy --from ... --to
...`). Use `limit_fill_model = "conservative"` for anything that rests limit
orders; the default `cross` model flatters those. Paper-trade it with `run`
before trusting the result, and see [TODO.md](../TODO.md) for what the paper
venue and backtest do not model.
