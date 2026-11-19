# Strategies

The strategies included with Kestrel, how each works, and its parameters.
The strategy is named on the command line (`kestrel_app run <name>` or
`backtest <name>`) and its parameters go in the config's `[strategy]` table
(see [configuration.md](../configuration.md#strategy)). Results of the
backtests and paper runs are in [strategy-tests.md](../strategy-tests.md);
writing a new strategy is covered in
[writing-strategies.md](../writing-strategies.md).

Each document describes the strategy as its source has it, with a link to
the source, and then how Kestrel's version differs from it.

| Strategy | Document | What it does |
|---|---|---|
| `breakout` | [breakout.md](breakout.md) | trades the break out of a bar pattern (a range, a double bottom, a triangle, a cup-and-handle and others), long or short |
| `market_maker` | [market-maker.md](market-maker.md) | quotes a bid and an ask around the mid, with inventory skew and opt-in drawdown protection |
| `momentum_rotation` | [momentum-rotation.md](momentum-rotation.md) | holds the strongest few of a universe of symbols by recent return, rebalanced on a schedule |
| `trend_ensemble` | [trend-ensemble.md](trend-ensemble.md) | holds each coin of a universe in proportion to how many of its Donchian trend models are long, sized to a target volatility |
| `mean_reversion` | [mean-reversion.md](mean-reversion.md) | buys an oversold RSI (optionally under the lower Bollinger band, above a trend average) and sells on the rebound, a stop or a time limit |
| `ma_crossover` | [ma-crossover.md](ma-crossover.md) | a moving-average crossover; a worked example of the strategy interface |
