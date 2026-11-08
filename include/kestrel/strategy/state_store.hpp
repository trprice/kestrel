#pragma once

#include <optional>
#include <string>

#include "kestrel/types/error.hpp"

namespace kestrel::strategy {

// Somewhere a strategy can keep a little state across restarts: a value by
// key, replaced whole on each save. The runner hands one to a strategy whose
// Requirements::state asks for it (Context::State); a backtest has none.
// Values are opaque text (a strategy writes JSON, say). Called from the
// strategy thread only.
class StateStore {
   public:
    virtual ~StateStore() = default;
    StateStore() = default;
    // An interface: implementations are held by pointer, never copied.
    StateStore(const StateStore&) = delete;
    StateStore& operator=(const StateStore&) = delete;

    // The value saved under `key`, or nullopt if there is none.
    virtual types::Result<std::optional<std::string>> Load(
        const std::string& key) = 0;
    // Replaces the value under `key`.
    virtual types::Result<void> Save(const std::string& key,
                                     const std::string& value) = 0;
    // Forgets the value under `key` (nothing if there is none).
    virtual types::Result<void> Erase(const std::string& key) = 0;
};

}  // namespace kestrel::strategy
