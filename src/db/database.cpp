#include "kestrel/db/database.hpp"

#include <libpq-fe.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <charconv>
#include <deque>
#include <format>
#include <string_view>

#include "migrations.hpp"
#include "sql_rows.hpp"

namespace kestrel::db {

using types::ErrorCategory;
using types::MakeError;
using types::Result;

namespace {

// Arbitrary application-wide key serialising concurrent migrators.
constexpr const char* kMigrationLockKey = "7452135901";

struct ResultDeleter {
    void operator()(PGresult* r) const { PQclear(r); }
};
using ResultPtr = std::unique_ptr<PGresult, ResultDeleter>;

types::Error ConnectionError(const PGconn* conn, std::string_view what) {
    std::string message(PQerrorMessage(conn));
    while (!message.empty() &&
           (message.back() == '\n' || message.back() == '\r')) {
        message.pop_back();
    }
    return MakeError(ErrorCategory::kNetwork,
                     std::string(what) + ": " + message)
        .error();
}

// Maps a failed statement to an error category by SQLSTATE class.
types::Error StatementError(const PGresult* res, const PGconn* conn) {
    const char* state = PQresultErrorField(res, PG_DIAG_SQLSTATE);
    std::string message(PQresultErrorMessage(res));
    while (!message.empty() &&
           (message.back() == '\n' || message.back() == '\r')) {
        message.pop_back();
    }
    if (!state) {
        // No SQLSTATE: the server was never reached (connection lost, OOM).
        return ConnectionError(conn, "database");
    }
    ErrorCategory category = ErrorCategory::kRejected;
    const std::string_view code(state);
    if (code.starts_with("08") || code.starts_with("57P") ||
        code.starts_with("53")) {
        category =
            ErrorCategory::kNetwork;  // connection / shutdown / resources
    } else if (code.starts_with("28")) {
        category = ErrorCategory::kAuth;
    } else if (code.starts_with("40")) {
        category = ErrorCategory::kServer;  // serialization / deadlock
    }
    auto err = MakeError(category, "database: " + message, std::nullopt,
                         std::string(code))
                   .error();
    return err;
}

PGconn* Open(const ConnectionOptions& o) {
    std::vector<const char*> keys;
    std::vector<const char*> values;
    std::deque<std::string> storage;  // keeps every value alive for the call
    auto add = [&](const char* key, std::string value) {
        if (!value.empty()) {
            keys.push_back(key);
            values.push_back(storage.emplace_back(std::move(value)).c_str());
        }
    };
    add("host", o.host);
    if (o.port) {
        add("port", std::to_string(*o.port));
    }
    add("dbname", o.dbname);
    add("user", o.user);
    add("password", o.password);
    add("sslmode", o.sslmode);
    add("application_name", "kestrel");
    // A server or network that stops answering must not hang the caller (a
    // stream thread recording bars, say): give up connecting after 10 s,
    // notice a dead peer with keepalives (idle 30 s, then every 10 s, 3
    // tries), and drop a connection whose sent data goes unacknowledged
    // for 60 s.
    add("connect_timeout", "10");
    add("keepalives", "1");
    add("keepalives_idle", "30");
    add("keepalives_interval", "10");
    add("keepalives_count", "3");
    add("tcp_user_timeout", "60000");
    add("options", "-c client_min_messages=warning -c statement_timeout=" +
                       std::to_string(o.statement_timeout.count()));
    keys.push_back(nullptr);
    values.push_back(nullptr);
    return PQconnectdbParams(keys.data(), values.data(), 0);
}

}  // namespace

Database::Database(ConnectionOptions options, PGconn* conn)
    : mOptions(std::move(options)), mConn(conn) {}

Database::~Database() {
    if (mConn) {
        PQfinish(mConn);
    }
}

std::optional<std::string> LowDiskMessage(const std::string& path,
                                          std::uintmax_t available,
                                          std::uintmax_t capacity) {
    constexpr std::uintmax_t kGb = 1'000'000'000;
    if (capacity == 0 || (available >= 5 * kGb && available * 10 >= capacity)) {
        return std::nullopt;
    }
    const auto tenths = [](std::uintmax_t bytes) {
        return std::format("{}.{}", bytes / kGb, bytes % kGb / (kGb / 10));
    };
    return std::format(
        "database disk nearly full: {} GB free of {} GB ({}%) at {}",
        tenths(available), tenths(capacity), available * 100 / capacity, path);
}

Result<std::unique_ptr<Database>> Database::Connect(
    const ConnectionOptions& options) {
    PGconn* conn = Open(options);
    if (!conn) {
        return MakeError(ErrorCategory::kInternal,
                         "database: could not allocate a connection");
    }
    if (PQstatus(conn) != CONNECTION_OK) {
        auto err = ConnectionError(conn, "database connect");
        PQfinish(conn);
        return std::unexpected(std::move(err));
    }
    return std::unique_ptr<Database>(new Database(options, conn));
}

Result<void> Database::EnsureConnected() {
    if (PQstatus(mConn) == CONNECTION_OK) {
        return {};
    }
    spdlog::warn("database: connection lost, reconnecting");
    PQfinish(mConn);
    mConn = Open(mOptions);
    if (!mConn) {
        return MakeError(ErrorCategory::kInternal,
                         "database: could not allocate a connection");
    }
    if (PQstatus(mConn) != CONNECTION_OK) {
        return std::unexpected(ConnectionError(mConn, "database reconnect"));
    }
    return {};
}

Result<std::vector<Database::Row>> Database::Query(
    const std::string& sql,
    const std::vector<std::optional<std::string>>& params) {
    std::lock_guard lock(mMutex);
    if (auto ok = EnsureConnected(); !ok) {
        return std::unexpected(ok.error());
    }
    std::vector<const char*> values;
    values.reserve(params.size());
    for (const auto& p : params) {
        values.push_back(p ? p->c_str() : nullptr);
    }
    // Not sent again after a failure: the server may have run it before the
    // connection dropped (an INSERT would be made twice). EnsureConnected
    // above reconnects for the next call.
    ResultPtr res(PQexecParams(mConn, sql.c_str(),
                               static_cast<int>(params.size()), nullptr,
                               values.data(), nullptr, nullptr, 0));
    if (!res) {
        return std::unexpected(ConnectionError(mConn, "database query"));
    }
    const auto status = PQresultStatus(res.get());
    if (status != PGRES_TUPLES_OK && status != PGRES_COMMAND_OK) {
        return std::unexpected(StatementError(res.get(), mConn));
    }
    std::vector<Row> rows;
    const int n_rows = PQntuples(res.get());
    const int n_cols = PQnfields(res.get());
    rows.reserve(static_cast<std::size_t>(n_rows));
    for (int r = 0; r < n_rows; ++r) {
        Row row;
        row.reserve(static_cast<std::size_t>(n_cols));
        for (int c = 0; c < n_cols; ++c) {
            if (PQgetisnull(res.get(), r, c)) {
                row.emplace_back(std::nullopt);
            } else {
                row.emplace_back(PQgetvalue(res.get(), r, c));
            }
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

Result<void> Database::ExecScript(const std::string& sql) {
    std::lock_guard lock(mMutex);
    if (auto ok = EnsureConnected(); !ok) {
        return std::unexpected(ok.error());
    }
    // One simple-protocol string with several statements runs in a single
    // implicit transaction, so a failed migration leaves nothing behind.
    ResultPtr res(PQexec(mConn, sql.c_str()));
    if (!res) {
        return std::unexpected(ConnectionError(mConn, "database script"));
    }
    if (PQresultStatus(res.get()) != PGRES_COMMAND_OK &&
        PQresultStatus(res.get()) != PGRES_TUPLES_OK) {
        return std::unexpected(StatementError(res.get(), mConn));
    }
    return {};
}

Result<void> Database::Migrate() {
    // Session-level advisory lock: held on this connection until released.
    auto locked = Query(std::string("SELECT pg_advisory_lock(") +
                        kMigrationLockKey + ")");
    if (!locked) {
        return std::unexpected(locked.error());
    }
    auto unlock = [&] {
        (void)Query(std::string("SELECT pg_advisory_unlock(") +
                    kMigrationLockKey + ")");
    };

    auto fail = [&](types::Error err) -> Result<void> {
        unlock();
        return std::unexpected(std::move(err));
    };

    // CREATE EXTENSION needs a superuser the first time for timescaledb, so
    // only attempt it when the extension is missing.
    auto ext =
        Query("SELECT 1 FROM pg_extension WHERE extname = 'timescaledb'");
    if (!ext) {
        return fail(ext.error());
    }
    if (ext->empty()) {
        if (auto ok = ExecScript("CREATE EXTENSION IF NOT EXISTS timescaledb");
            !ok) {
            auto err = ok.error();
            err.message +=
                " (a superuser must run CREATE EXTENSION timescaledb in this "
                "database once)";
            return fail(std::move(err));
        }
    }
    if (auto ok = ExecScript("CREATE TABLE IF NOT EXISTS schema_migrations ("
                             "version INTEGER PRIMARY KEY, name TEXT NOT NULL, "
                             "applied_at TIMESTAMPTZ NOT NULL DEFAULT now())");
        !ok) {
        return fail(ok.error());
    }
    auto applied =
        Query("SELECT COALESCE(MAX(version), 0) FROM schema_migrations");
    if (!applied) {
        return fail(applied.error());
    }
    if (applied->empty() || applied->front().empty()) {
        return fail(types::Error{ErrorCategory::kParse,
                                 "schema_migrations: empty version query",
                                 std::nullopt, std::nullopt, std::nullopt});
    }
    const auto version =
        sql::ParseInt(applied->front().front(), "schema_migrations: version");
    if (!version) {
        return fail(version.error());
    }
    const auto current = *version;

    for (const auto& m : Migrations()) {
        if (m.version <= current) {
            continue;
        }
        spdlog::info("database: applying migration {} ({})", m.version, m.name);
        // Migration names are string literals from migrations.cpp.
        const std::string script =
            std::string(m.sql) +
            "\nINSERT INTO schema_migrations (version, name) VALUES (" +
            std::to_string(m.version) + ", '" + m.name + "');";
        if (auto ok = ExecScript(script); !ok) {
            auto err = ok.error();
            err.message =
                "migration " + std::to_string(m.version) + ": " + err.message;
            return fail(std::move(err));
        }
    }
    EnableCompression();
    unlock();
    return {};
}

namespace {

// DML (inserts with ON CONFLICT and upserts) on compressed chunks requires
// TimescaleDB 2.11+.
bool CompressionSupportedVersion(std::string_view v) {
    const auto dot = v.find('.');
    if (dot == std::string_view::npos) {
        return false;
    }
    int major = 0;
    int minor = 0;
    const auto [p1, e1] = std::from_chars(v.data(), v.data() + dot, major);
    if (e1 != std::errc{} || p1 != v.data() + dot) {
        return false;
    }
    const auto rest = v.substr(dot + 1);
    const auto end =
        std::min(rest.find_first_not_of("0123456789"), rest.size());
    const auto [p2, e2] =
        std::from_chars(rest.data(), rest.data() + end, minor);
    if (e2 != std::errc{} || end == 0 || p2 != rest.data() + end) {
        return false;
    }
    return major > 2 || (major == 2 && minor >= 11);
}

}  // namespace

void Database::EnableCompression() {
    // Compression is in TimescaleDB's community edition (license
    // "timescale"); the Apache-only build has none.
    auto license = Query("SELECT current_setting('timescaledb.license', true)");
    if (!license) {
        spdlog::warn(
            "database: cannot read the TimescaleDB license: {}; "
            "compression not checked",
            license.error().ToString());
        return;
    }
    if (license->empty() || license->front().empty() ||
        license->front().front() != "timescale") {
        spdlog::debug(
            "database: TimescaleDB's Apache build has no "
            "compression; the hypertables stay uncompressed");
        return;
    }
    auto ext_ver = Query(
        "SELECT extversion FROM pg_extension WHERE extname = "
        "'timescaledb'");
    if (!ext_ver || ext_ver->empty() || ext_ver->front().empty() ||
        !ext_ver->front().front()) {
        spdlog::warn(
            "database: cannot read the TimescaleDB version; compression not "
            "checked");
        return;
    }
    if (!CompressionSupportedVersion(*ext_ver->front().front())) {
        spdlog::warn(
            "database: TimescaleDB {} is older than 2.11 (needed for upserts "
            "into compressed chunks); the hypertables stay uncompressed",
            *ext_ver->front().front());
        return;
    }
    auto enabled = Query(
        "SELECT hypertable_name FROM timescaledb_information.hypertables "
        "WHERE hypertable_schema = current_schema() AND "
        "compression_enabled");
    if (!enabled) {
        spdlog::warn(
            "database: cannot read which hypertables are "
            "compressed: {}",
            enabled.error().ToString());
        return;
    }
    for (const auto& c : Compressions()) {
        const bool on = std::ranges::any_of(*enabled, [&](const Row& row) {
            return !row.empty() && row.front() == c.table;
        });
        if (on) {
            continue;
        }
        // Constants from migrations.cpp, not input. Bound the lock wait so a
        // startup never blocks writers behind a long backtest read.
        const std::string table = c.table;
        const std::string script =
            "SET LOCAL lock_timeout = '5s';\nALTER TABLE " + table +
            " SET (timescaledb.compress, timescaledb.compress_segmentby = '" +
            c.segment_by + "', timescaledb.compress_orderby = '" + c.order_by +
            "');\nSELECT add_compression_policy('" + table + "', INTERVAL '" +
            c.compress_after + "', if_not_exists => true);";
        if (auto ok = ExecScript(script); !ok) {
            spdlog::warn("database: cannot turn on compression for {}: {}",
                         table, ok.error().ToString());
            continue;
        }
        spdlog::info("database: compressing {} chunks older than {}", table,
                     c.compress_after);
    }
}

}  // namespace kestrel::db
