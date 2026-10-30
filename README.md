# Kestrel

A C++23 library for gathering stock and cryptocurrency prices and trading on
them. Providers: **Schwab**, **Kraken**, **Binance.US**.

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
./build/kestrel_app -c config/kestrel.toml run sma_crossover   # paper mode
```

Requires CMake 3.25+ and GCC 12+ or Clang 16+. Paper mode needs no credentials.

## Documentation

| Document | Covers |
|---|---|
| [Building](docs/building.md) | requirements, dependencies, CMake options, sanitizer builds |
| [Configuration](docs/configuration.md) | the config file table by table, credentials and the secrets file, risk limits, strategy parameters |
| [Running](docs/running.md) | the packages a machine needs to run it, the `kestrel_app` subcommands, backtesting, running strategies, live trading commands on Binance.US |
| [Database](docs/database.md) | PostgreSQL / TimescaleDB setup, storing market data, recording runs, the account's trade history, upgrading |
| [Writing a strategy](docs/writing-strategies.md) | the strategy interface, its hooks, the rules to follow, registering and testing one |
| [Design](docs/design.md) | how information flows, the major classes, threads, design notes, source layout |
| [Testing](docs/testing.md) | running the tests, database integration tests, running the suite in Docker |
