# Kestrel

A C++23 library for gathering stock and cryptocurrency prices and trading on
them. Providers: **Schwab**, **Kraken**, **Binance.US**, and **Databento**
for historical US equity data to backtest on.

Market data and order execution are separate interfaces (`data::DataSource`,
`trading::TradingVenue`). Only the execution side changes between paper and
live trading; a factory picks the right one from the config. Strategies run
the same way live, on paper and in a backtest.

Status, known limitations, work in progress and planned projects are tracked in
[TODO.md](TODO.md).

## Quick start

```sh
cmake -S . -B build && cmake --build build -j
cp config/kestrel.example.toml config/kestrel.toml
./build/kestrel_app -c config/kestrel.toml bars 1h 24
./build/kestrel_app -c config/kestrel.toml run ma_crossover   # paper mode
```

Requires CMake 3.25+ and GCC 12+ or Clang 16+. Paper mode on Kraken or
Binance.US needs no credentials; Schwab's market data needs its app key and
tokens even in paper mode.

## Documentation

| Document | Covers |
|---|---|
| [Building](docs/building.md) | requirements, dependencies, CMake options, sanitizer builds |
| [Configuration](docs/configuration.md) | the config file table by table, credentials and the secrets file, risk limits |
| [Venues](docs/venues/README.md) | how Kestrel connects to each provider for market data and orders, paper and live; [Schwab](docs/venues/schwab.md) in detail |
| [Strategies](docs/strategies/README.md) | each included strategy: how it works, what Kestrel does for it, and its parameters |
| [Running](docs/running.md) | the packages a machine needs to run it, the `kestrel_app` subcommands, backtesting, running strategies, live trading commands on Binance.US |
| [Database](docs/database.md) | PostgreSQL / TimescaleDB setup, storing market data, recording runs, the account's trade history, upgrading |
| [Writing a strategy](docs/writing-strategies.md) | the strategy interface, its hooks, the rules to follow, registering and testing one |
| [Design](docs/design.md) | how information flows, the major classes, threads, design notes, source layout |
| [Testing](docs/testing.md) | running the tests, database integration tests, running the suite in Docker |
| [Strategy tests](docs/strategy-tests.md) | the backtests and paper runs done so far, and a short summary of each result |

## Blog posts

Posts about building Kestrel, on [blog.taylorbuiltsolutions.com](https://blog.taylorbuiltsolutions.com):

| Date | Post | Covers |
|---|---|---|
| Nov 15 2026 | [Introducing Kestrel – A C++23 Stock and Crypto Trading Tool](https://blog.taylorbuiltsolutions.com/introducing-kestrel-stock-crypto-trading-tool/) | what Kestrel is and why it was built |
| Nov 17 2026 | [Building a C++23 Trading Tool With AI Coding Agents (Without Making Spaghetti)](https://blog.taylorbuiltsolutions.com/building-with-ai-coding-agents/) | building it with AI coding agents, and the house rules that keep the code clean: narrow interfaces, clang-format, clang-tidy, sanitizers |
| Nov 21 2026 | [C++ Under the Hood: Money, Latency, and Safety](https://blog.taylorbuiltsolutions.com/cpp-under-the-hood-trading-bot/) | why safety and correctness matter more than latency here, and exact fixed-point math for prices, sizes and P&L |
| Nov 23 2026 | [Integrating Crypto Exchanges and Charles Schwab](https://blog.taylorbuiltsolutions.com/integrating-crypto-exchanges-charles-schwab-api/) | Binance.US, Kraken, Schwab and Databento behind one interface, with each venue's symbol quirks handled at the edge |
