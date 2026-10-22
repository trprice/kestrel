#include "kestrel/db/database.hpp"

#include <libpq-fe.h>
#include <spdlog/spdlog.h>

#include <deque>
#include <string_view>

#include "migrations.hpp"

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
    add("options", "-c client_min_messages=warning");
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
    const int current = std::stoi(*applied->front().front());

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
    unlock();
    return {};
}

}  // namespace kestrel::db
