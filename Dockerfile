# Build and test environment for Kestrel, including the PostgreSQL +
# TimescaleDB test database. Everything is fetched while the image is built;
# running it needs no network access:
#
#   docker build -t kestrel-test .
#   docker run --rm --network none kestrel-test
#
# The container starts PostgreSQL, builds the source in /src into /build and
# runs ctest with the database tests enabled. Mount a working tree over /src
# to test local changes without rebuilding the image:
#
#   docker run --rm --network none -v "$PWD":/src kestrel-test
#
# Compile parallelism defaults to 2 jobs (-e KESTREL_JOBS=N to change it).
# Extra arguments are passed to ctest, e.g. `... kestrel-test -R fixture`.
FROM ubuntu:26.04

ARG DEBIAN_FRONTEND=noninteractive
ARG PG_VERSION=18
ARG IXWEBSOCKET_TAG=v11.4.5

# Every dependency in cmake/KestrelDependencies.cmake except IXWebSocket is
# packaged, so find_package() succeeds and FetchContent is never reached.
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build pkg-config git ca-certificates \
        libssl-dev zlib1g-dev libcurl4-openssl-dev libpq-dev libcpr-dev \
        nlohmann-json3-dev libspdlog-dev libtomlplusplus-dev libcli11-dev \
        libgtest-dev linux-perf \
        postgresql-${PG_VERSION} postgresql-${PG_VERSION}-timescaledb \
    && rm -rf /var/lib/apt/lists/*

# IXWebSocket has no Ubuntu package: build it once here and install it where
# find_package(ixwebsocket CONFIG) will see it.
RUN git clone --depth 1 --branch ${IXWEBSOCKET_TAG} \
        https://github.com/machinezone/IXWebSocket.git /tmp/ixwebsocket \
    && cmake -S /tmp/ixwebsocket -B /tmp/ixwebsocket/build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        -DUSE_TLS=ON -DUSE_OPEN_SSL=ON -DUSE_ZLIB=ON \
    && cmake --build /tmp/ixwebsocket/build \
    && cmake --install /tmp/ixwebsocket/build \
    && rm -rf /tmp/ixwebsocket

# Preload TimescaleDB, and keep it from phoning home.
RUN printf '%s\n' \
        "shared_preload_libraries = 'timescaledb'" \
        "timescaledb.telemetry_level = off" \
        > /etc/postgresql/${PG_VERSION}/main/conf.d/kestrel.conf

COPY scripts/setup_database.sh scripts/setup_test_database.sh /usr/local/lib/kestrel/scripts/

# Create the kestrel_test role and database with the timescaledb extension.
# The tests create the tables and load tests/data/market themselves.
RUN pg_ctlcluster ${PG_VERSION} main start \
    && /usr/local/lib/kestrel/scripts/setup_test_database.sh \
        --admin-cmd "runuser -u postgres -- psql" \
    && pg_ctlcluster ${PG_VERSION} main stop

ENV PG_VERSION=${PG_VERSION} \
    KESTREL_TEST_DBNAME=kestrel_test \
    KESTREL_TEST_DBHOST=localhost \
    KESTREL_TEST_DBUSER=kestrel_test \
    KESTREL_TEST_DBPASSWORD=kestrel_test

COPY docker/entrypoint.sh /usr/local/bin/kestrel-entrypoint
COPY . /src
WORKDIR /src
ENTRYPOINT ["kestrel-entrypoint"]
