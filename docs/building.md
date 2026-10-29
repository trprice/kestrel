# Building Kestrel

Requires CMake 3.25+ and GCC 12+/Clang 16+ (`std::expected`). The `Decimal`
implementation uses `__int128`, so MSVC is not supported yet.

```sh
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure     # or ./build/kestrel_tests
```

See [testing.md](testing.md) for the database tests, which need a PostgreSQL
database, and for running the whole suite in Docker.

## Dependencies

See `cmake/KestrelDependencies.cmake`. Three must be installed, since they are
found with `find_package(... REQUIRED)` and never fetched:

- OpenSSL
- PostgreSQL's libpq (Debian/Ubuntu: `libpq-dev`; see [database.md](database.md))
- ZLIB (needed by IXWebSocket)

The rest are found with `find_package` if installed and fetched with
FetchContent otherwise: cpr (system libcurl preferred via
`CPR_USE_SYSTEM_CURL`), IXWebSocket (TLS via OpenSSL), nlohmann/json, toml++,
spdlog, CLI11 (for `kestrel_app`) and GoogleTest (only with tests on). The
first configure needs network access if any of these are missing. The cpr
FetchContent fallback is written but has not been exercised here (the system
cpr was found); see [TODO.md](../TODO.md).

## Targets and options

Targets: `kestrel` (library), `kestrel_app`, `kestrel_tests`.
Options: `-DKESTREL_BUILD_APP=OFF`, `-DKESTREL_BUILD_TESTS=OFF`.

## Sanitizers

`-DKESTREL_SANITIZE=` builds everything, the fetched dependencies included,
with AddressSanitizer, UndefinedBehaviorSanitizer or ThreadSanitizer. Use a
separate build directory for each; `build-*/` is git-ignored.

```sh
cmake -S . -B build-asan -DKESTREL_SANITIZE=address,undefined
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure

cmake -S . -B build-tsan -DKESTREL_SANITIZE=thread
cmake --build build-tsan -j
ctest --test-dir build-tsan --output-on-failure
```

Allowed values are `address`, `undefined`, `address,undefined` (or
`undefined,address`) and `thread` (thread cannot be combined with address). An undefined-behavior finding aborts
the test instead of printing and carrying on. A sanitized build is several
times slower to compile and run, and the first configure fetches any
dependency that is not installed. TSan is the one to run after touching the
runner, a venue, a stream or `QuoteWriter`, since they are multithreaded.

Run the database tests serially (plain `ctest`, not `-j`), sanitized or not:
they share the fixture venues' rows, so parallel runs collide.
