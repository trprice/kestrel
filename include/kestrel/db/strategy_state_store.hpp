#pragma once

#include <memory>
#include <optional>
#include <string>

#include "kestrel/db/database.hpp"
#include "kestrel/strategy/state_store.hpp"

namespace kestrel::db {

// A strategy::StateStore in the `strategy_state` table: one row per key,
// with when it was last saved. Every key is prefixed with `scope` (the
// venue and mode, say "binanceus/live/"), so a paper run never reads a live
// run's state.
class StrategyStateStore : public strategy::StateStore {
   public:
    StrategyStateStore(std::shared_ptr<Database> database, std::string scope)
        : mDatabase(std::move(database)), mScope(std::move(scope)) {}

    types::Result<std::optional<std::string>> Load(
        const std::string& key) override;
    types::Result<void> Save(const std::string& key,
                             const std::string& value) override;
    types::Result<void> Erase(const std::string& key) override;

   private:
    std::shared_ptr<Database> mDatabase;
    std::string mScope;
};

}  // namespace kestrel::db
