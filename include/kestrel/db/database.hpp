#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "kestrel/types/error.hpp"

struct pg_conn;

namespace kestrel::db {

// Empty fields fall back to libpq's own defaults (PGHOST, PGUSER, ~/.pgpass,
// the local socket, ...).
struct ConnectionOptions {
    std::string host;
    std::optional<int> port;
    std::string dbname = "kestrel";
    std::string user;
    std::string password;  // never logged
    std::string sslmode;
};

// One PostgreSQL connection, serialised by a mutex so the stream thread and
// the main thread can share it. Reconnects once if the connection has dropped.
// SQL values always travel as bind parameters, never spliced into the text.
class Database {
   public:
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    static types::Result<std::unique_ptr<Database>> Connect(
        const ConnectionOptions& options);

    // Creates the timescaledb extension and applies every schema migration
    // that has not been applied yet. Safe to call on every startup and from
    // several processes at once.
    types::Result<void> Migrate();

    // Rows come back as text; a SQL NULL is nullopt.
    using Row = std::vector<std::optional<std::string>>;

    // Runs one statement with `$1..$n` parameters (nullopt binds NULL).
    types::Result<std::vector<Row>> Query(
        const std::string& sql,
        const std::vector<std::optional<std::string>>& params = {});

   private:
    Database(ConnectionOptions options, pg_conn* conn);
    types::Result<void> EnsureConnected();
    // Runs a script of several statements in a single transaction.
    types::Result<void> ExecScript(const std::string& sql);

    ConnectionOptions mOptions;
    std::mutex mMutex;
    pg_conn* mConn;
};

}  // namespace kestrel::db
