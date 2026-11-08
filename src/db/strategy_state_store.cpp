#include "kestrel/db/strategy_state_store.hpp"

namespace kestrel::db {

using types::Result;

Result<std::optional<std::string>> StrategyStateStore::Load(
    const std::string& key) {
    auto rows = mDatabase->Query(
        "SELECT value FROM strategy_state WHERE key = $1", {mScope + key});
    if (!rows) {
        return std::unexpected(rows.error());
    }
    if (rows->empty() || rows->front().empty()) {
        return std::optional<std::string>();
    }
    return rows->front().front();
}

Result<void> StrategyStateStore::Save(const std::string& key,
                                      const std::string& value) {
    auto rows = mDatabase->Query(
        "INSERT INTO strategy_state (key, value, saved_at) "
        "VALUES ($1, $2, now()) ON CONFLICT (key) DO UPDATE "
        "SET value = EXCLUDED.value, saved_at = EXCLUDED.saved_at",
        {mScope + key, value});
    if (!rows) {
        return std::unexpected(rows.error());
    }
    return {};
}

Result<void> StrategyStateStore::Erase(const std::string& key) {
    auto rows = mDatabase->Query("DELETE FROM strategy_state WHERE key = $1",
                                 {mScope + key});
    if (!rows) {
        return std::unexpected(rows.error());
    }
    return {};
}

}  // namespace kestrel::db
