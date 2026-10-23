#!/usr/bin/env bash
# Creates the database the database tests run against (kestrel_test, owned by
# the role kestrel_test) and prints the environment variables that point the
# tests at it. The tests create the tables and load the market data checked in
# under tests/data/market themselves; no venue API keys are needed.
#
#   scripts/setup_test_database.sh [setup_database.sh options]
#
# Extra options (--admin-cmd, --db-name, ...) are passed to setup_database.sh.
# The password comes from KESTREL_TEST_DBPASSWORD, default "kestrel_test"
# (a throwaway local database, so a well-known password is fine).
set -euo pipefail

here=$(dirname "$0")
password=${KESTREL_TEST_DBPASSWORD:-kestrel_test}

KESTREL_DATABASE_PASSWORD=$password "$here/setup_database.sh" \
    --db-name kestrel_test --db-user kestrel_test "$@"

cat <<MSG

Run the database tests with:

  export KESTREL_TEST_DBNAME=kestrel_test
  export KESTREL_TEST_DBHOST=localhost
  export KESTREL_TEST_DBUSER=kestrel_test
  export KESTREL_TEST_DBPASSWORD=$password
  ctest --test-dir build --output-on-failure
MSG
