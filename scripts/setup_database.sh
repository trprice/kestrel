#!/usr/bin/env bash
# Sets up PostgreSQL + TimescaleDB for a new Kestrel installation:
#   1. creates the login role and the database (skipped if they exist)
#   2. enables the timescaledb extension in that database (needs a superuser)
#   3. optionally creates the tables by running `kestrel_app init-db`
#
# Safe to re-run. Run it as a user that can administer PostgreSQL; by default
# it runs psql as the `postgres` OS user through sudo.
#
#   scripts/setup_database.sh [options]
#     --db-name NAME      database to create          (default: kestrel)
#     --db-user NAME      role that owns it           (default: kestrel)
#     --admin-cmd CMD     command that opens an admin psql session
#                         (default: "sudo -u postgres psql"; env
#                         KESTREL_ADMIN_PSQL), e.g. "psql -h db.example.com -U admin"
#     --config FILE       after the extension is in, create the tables with
#                         kestrel_app using this config (needs --app)
#     --app PATH          kestrel_app binary          (default: build/kestrel_app)
#
# The role's password is read from KESTREL_DATABASE_PASSWORD, or generated if
# the role is new and the variable is unset (it is printed once). An existing
# role's password is only changed when KESTREL_DATABASE_PASSWORD is set.
set -euo pipefail

db_name=kestrel
db_user=kestrel
admin_cmd=${KESTREL_ADMIN_PSQL:-"sudo -u postgres psql"}
config=
app=build/kestrel_app

while [[ $# -gt 0 ]]; do
    case $1 in
        --db-name) db_name=$2; shift 2 ;;
        --db-user) db_user=$2; shift 2 ;;
        --admin-cmd) admin_cmd=$2; shift 2 ;;
        --config) config=$2; shift 2 ;;
        --app) app=$2; shift 2 ;;
        -h|--help) sed -n '2,/^set -e/p' "$0" | sed '$d;s/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

if [[ ! $db_name =~ ^[A-Za-z_][A-Za-z0-9_]*$ || ! $db_user =~ ^[A-Za-z_][A-Za-z0-9_]*$ ]]; then
    echo "database and role names may only contain letters, digits and _" >&2
    exit 2
fi

# shellcheck disable=SC2086  # admin_cmd is deliberately word-split
admin() { $admin_cmd -X -v ON_ERROR_STOP=1 "$@"; }

echo "Checking PostgreSQL connection..."
admin -d postgres -qtAc "SELECT 1" >/dev/null ||
    { echo "cannot connect with: $admin_cmd (see --admin-cmd)" >&2; exit 1; }

if ! admin -d postgres -qtAc \
    "SELECT 1 FROM pg_available_extensions WHERE name = 'timescaledb'" | grep -q 1; then
    echo "The timescaledb extension is not installed on the server." >&2
    echo "Install the TimescaleDB package for your PostgreSQL version first" >&2
    echo "(Ubuntu: postgresql-<version>-timescaledb)." >&2
    exit 1
fi

password=${KESTREL_DATABASE_PASSWORD:-}
generated=
role_exists=$(admin -d postgres -qtAc \
    "SELECT 1 FROM pg_roles WHERE rolname = '$db_user'")
if [[ -z $role_exists && -z $password ]]; then
    password=$(head -c 24 /dev/urandom | base64 | tr -d '/+=\n')
    generated=1
fi

if [[ -z $role_exists ]]; then
    echo "Creating role $db_user..."
    # The password goes to psql on stdin as a variable so it never appears in
    # a process list or gets spliced into SQL by the shell.
    printf '%s\n' "CREATE ROLE $db_user LOGIN PASSWORD :'pw';" |
        admin -d postgres -v "pw=$password" -q
else
    echo "Role $db_user already exists."
    if [[ -n $password ]]; then
        echo "Updating its password from KESTREL_DATABASE_PASSWORD..."
        printf '%s\n' "ALTER ROLE $db_user PASSWORD :'pw';" |
            admin -d postgres -v "pw=$password" -q
    fi
fi

if [[ -z $(admin -d postgres -qtAc \
    "SELECT 1 FROM pg_database WHERE datname = '$db_name'") ]]; then
    echo "Creating database $db_name..."
    admin -d postgres -qc "CREATE DATABASE $db_name OWNER $db_user"
else
    echo "Database $db_name already exists."
fi

echo "Enabling the timescaledb extension in $db_name..."
admin -d "$db_name" -qc "CREATE EXTENSION IF NOT EXISTS timescaledb" 2>&1 |
    grep -v '^NOTICE' || true
if [[ -z $(admin -d "$db_name" -qtAc \
    "SELECT 1 FROM pg_extension WHERE extname = 'timescaledb'") ]]; then
    echo "Could not enable timescaledb. Make sure postgresql.conf has" >&2
    echo "  shared_preload_libraries = 'timescaledb'" >&2
    echo "and restart PostgreSQL (timescaledb-tune can do this)." >&2
    exit 1
fi

if [[ -n $config ]]; then
    if [[ ! -x $app ]]; then
        echo "$app not found; build kestrel first or pass --app" >&2
        exit 1
    fi
    echo "Creating tables..."
    KESTREL_DATABASE_PASSWORD=$password "$app" --config "$config" init-db
else
    echo "Extension ready. Create the tables with:"
    echo "  kestrel_app --config <config> init-db"
    echo "(they are also created automatically the first time a --store command runs)."
fi

cat <<MSG

Add this to your kestrel config:

  [database]
  host   = "localhost"
  dbname = "$db_name"
  user   = "$db_user"

MSG
if [[ -n $generated ]]; then
    echo "Generated password for $db_user (shown once; put it in secrets.toml"
    echo "under [database] password, or export KESTREL_DATABASE_PASSWORD):"
    echo "  $password"
fi
