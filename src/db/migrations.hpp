#pragma once

#include <vector>

namespace kestrel::db {

struct Migration {
    int version;
    const char* name;
    const char* sql;  // may hold several statements
};

// Append-only: never edit a migration that has shipped, add a new one. Note
// that `quotes`, `market_trades` and `bars` may have compressed chunks (see
// Compressions()), which restrict column-type changes, new constraints and
// whole-table UPDATEs unless affected chunks are decompressed first.
const std::vector<Migration>& Migrations();

// A hypertable's TimescaleDB compression: chunks wholly older than
// `compress_after` are compressed by a background policy, in segments per
// `segment_by` value, each ordered by `order_by`. SQL fragments, constants.
struct Compression {
    const char* table;
    const char* segment_by;
    const char* order_by;
    const char* compress_after;  // a PostgreSQL interval
};

// The hypertables Migrate compresses, where TimescaleDB allows it.
const std::vector<Compression>& Compressions();

}  // namespace kestrel::db
