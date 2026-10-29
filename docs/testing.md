# Testing

```sh
ctest --test-dir build --output-on-failure     # or ./build/kestrel_tests
```

`ctest` runs the unit tests (`kestrel_tests`, GoogleTest) and the `cli_*`
tests, which run the built `kestrel_app` with bad arguments and check how it
refuses them (`tests/cli_check.cmake`, cases in `CMakeLists.txt`). Run it
serially, not with `-j`, when the database tests are enabled: they share the
fixture venues' rows, so parallel runs collide.

For sanitizer builds (ASan, UBSan, TSan) see [building.md](building.md#sanitizers).

## Database integration tests

The database tests run only when `KESTREL_TEST_DBNAME` is set (plus optional
`KESTREL_TEST_DB{HOST,PORT,USER,PASSWORD}`); otherwise they are skipped. **No
venue API keys are needed**: real Kraken and Binance.US bars and quotes are
checked in under `tests/data/market/`, and the tests load them into the
database themselves. What you need to do:

1. Install PostgreSQL and the TimescaleDB package for your PostgreSQL version
   (Ubuntu: `postgresql-<version>-timescaledb`), and enable it in
   `postgresql.conf` with `shared_preload_libraries = 'timescaledb'`
   (`timescaledb-tune` can do this), then restart PostgreSQL.
2. Run `scripts/setup_test_database.sh`. It must run as a PostgreSQL admin
   (by default through `sudo -u postgres psql`), because creating the
   database and enabling the extension need superuser rights. The tests
   cannot do this for you: their `kestrel_test` role is deliberately not a
   superuser and cannot create databases.
3. Export the variables it prints and run the tests:

```bash
export KESTREL_TEST_DBNAME=kestrel_test
export KESTREL_TEST_DBHOST=localhost
export KESTREL_TEST_DBUSER=kestrel_test
export KESTREL_TEST_DBPASSWORD=kestrel_test
ctest --test-dir build --output-on-failure
```

The script wraps `scripts/setup_database.sh` (see its `--help`, e.g.
`--admin-cmd` if `sudo -u postgres psql` doesn't work on your machine). The
tests run the migrations, then replace the rows of two venues named
`fixture-kraken` and `fixture-binanceus`; the other store tests only touch rows
under their own venue or strategy names. Use a separate database from your real
one anyway.

`tests/db_fixture.hpp` has the shared helpers (`ConnectTestDatabase`,
`ReadFixtureData`, `LoadFixtureData`); `tests/fixture_data_test.cpp` is the
suite built on them. To refresh the data, store a new pull into a scratch
database (`kestrel_app bars 1h 168 --store`, `kestrel_app stream 90 --store`)
and run `scripts/export_test_data.sh` with the `PG*` variables pointing at it.

## Fakes for strategy and venue tests

`tests/strategy_fakes.hpp` has a recording venue (`RecordingVenue`, which can
also push executions), and `FakeTradeLog`. `tests/binanceus_trading_test_support.hpp`
has `RoutedHttp`, a scripted HTTP client that answers by URL path and keeps
every request, for testing the Binance.US client and venue without the network.
See [writing-strategies.md](writing-strategies.md#5-test-it) for testing a
strategy.

## Running the tests in Docker

The `Dockerfile` (Ubuntu 26.04) installs the build dependencies, PostgreSQL 18
with TimescaleDB and the `kestrel_test` database, so the whole suite, database
tests included, runs without installing anything on the host. Only building the
image needs network access (apt and an IXWebSocket clone); running it does not.

```sh
docker build -t kestrel-test .
docker run --rm --network none kestrel-test
```

The container starts PostgreSQL, builds `/src` into `/build` and runs `ctest`.
Arguments after the image name go to `ctest`. Mount your working tree over
`/src` to test local changes without rebuilding the image:

```sh
docker run --rm --network none -v "$PWD":/src kestrel-test -R fixture
```

The build runs 2 compile jobs by default because parallel C++23 compiles can
run a small container out of memory (the compiler is killed); change it with
`-e KESTREL_JOBS=N`. The source is compiled on every run.

To skip the entrypoint and get a shell (to run a single test, use `perf`,
or poke at the database), override it. PostgreSQL is not started for you in
that case:

```sh
docker run --rm -it --network none -v "$PWD":/src --entrypoint bash kestrel-test
# inside the container:
pg_ctlcluster "$PG_VERSION" main start
cmake -S /src -B /build -G Ninja && cmake --build /build -j2
/build/kestrel_tests --gtest_filter='BarStoreTest.*'
```

The image includes `perf`. Docker's default seccomp profile blocks the
`perf_event_open` syscall, so profiling needs
`--cap-add SYS_ADMIN --security-opt seccomp=unconfined` on the `docker run`
(and a host `kernel.perf_event_paranoid` that permits it).
