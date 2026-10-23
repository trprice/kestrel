#!/usr/bin/env bash
# Writes tests/data/market/{bars,quotes}.csv from a database that has had real
# data stored into it, e.g. with
#
#   kestrel_app -c <config> bars 1h 168 --store
#   kestrel_app -c <config> stream 90 --store
#
# Use a scratch database, not one you care about. Connection settings come
# from the usual PG* variables (PGHOST, PGUSER, PGPASSWORD, PGDATABASE).
# The venue column is prefixed "fixture-" so loading the files into a test
# database can never collide with, or delete, real rows.
#
#   scripts/export_test_data.sh [venue ...]   (default: kraken binanceus)
set -euo pipefail

out=$(dirname "$0")/../tests/data/market
mkdir -p "$out"
venues=("$@")
[[ ${#venues[@]} -gt 0 ]] || venues=(kraken binanceus)
list=$(printf "'%s'," "${venues[@]}")
list=${list%,}

ms() { echo "(extract(epoch FROM $1) * 1000)::bigint"; }

psql -X -v ON_ERROR_STOP=1 -q <<SQL
\copy (SELECT 'fixture-' || venue AS venue, symbol, timeframe, $(ms open_time) AS open_time_ms, open, high, low, close, volume FROM bars WHERE venue IN ($list) ORDER BY venue, symbol, timeframe, open_time) TO '$out/bars.csv' WITH (FORMAT csv, HEADER)
\copy (SELECT 'fixture-' || venue AS venue, symbol, seq, $(ms received_time) AS received_time_ms, $(ms exchange_time) AS exchange_time_ms, bid, bid_size, ask, ask_size, last, last_size FROM quotes WHERE venue IN ($list) ORDER BY venue, symbol, seq) TO '$out/quotes.csv' WITH (FORMAT csv, HEADER)
SQL
wc -l "$out"/bars.csv "$out"/quotes.csv
