#pragma once

#include <vector>

namespace kestrel::db {

struct Migration {
    int version;
    const char* name;
    const char* sql;  // may hold several statements
};

// Append-only: never edit a migration that has shipped, add a new one.
const std::vector<Migration>& Migrations();

}  // namespace kestrel::db
