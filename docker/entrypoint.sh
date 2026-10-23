#!/usr/bin/env bash
# Starts PostgreSQL, builds /src into /build and runs the tests against the
# database set up in the image. Arguments go to ctest.
set -euo pipefail

pg_ctlcluster "$PG_VERSION" main start
trap 'pg_ctlcluster "$PG_VERSION" main stop' EXIT

cmake -S /src -B /build -G Ninja
# Parallel C++23 compiles can exhaust a container's memory; override with KESTREL_JOBS.
cmake --build /build -j "${KESTREL_JOBS:-2}"

# The tests skip silently without the database; fail instead if it is not there.
PGPASSWORD=$KESTREL_TEST_DBPASSWORD psql -h "$KESTREL_TEST_DBHOST" \
    -U "$KESTREL_TEST_DBUSER" -d "$KESTREL_TEST_DBNAME" -qtAc \
    "SELECT extname FROM pg_extension WHERE extname = 'timescaledb'" | grep -q timescaledb

ctest --test-dir /build --output-on-failure "$@"
