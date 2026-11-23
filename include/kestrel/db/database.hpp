#pragma once

#include <chrono>
#include <cstdint>
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
    // A statement running longer is cancelled by the server and fails; zero
    // means no limit. Long enough for a backtest's large quote loads.
    std::chrono::milliseconds statement_timeout{300'000};
    // A path on the database's disk, to warn at startup when that disk is
    // nearly full (LowDiskMessage); empty for no check. Not a connection
    // setting.
    std::string disk_path;
};

// The warning for a database disk with `available` of `capacity` bytes
// free at `path`, when that is under 10% or 5 GB; nullopt otherwise.
std::optional<std::string> LowDiskMessage(const std::string& path,
                                          std::uintmax_t available,
                                          std::uintmax_t capacity);

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
    // that has not been applied yet, then turns on compression for the
    // quote, market trade and bar hypertables if it is off and TimescaleDB
    // offers it (its community edition; the Apache-only build does not).
    // Safe to call on every startup and from several processes at once.
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
    // Migrate's compression step: logs a failure rather than returning it,
    // since the tables work the same uncompressed.
    void EnableCompression();

    ConnectionOptions mOptions;
    std::mutex mMutex;
    pg_conn* mConn;
};

}  // namespace kestrel::db
