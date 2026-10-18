#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

#include "kestrel/types/error.hpp"

namespace kestrel::types {

// All timestamps in Kestrel are UTC with millisecond precision.
using Timestamp = std::chrono::sys_time<std::chrono::milliseconds>;

Timestamp Now();

inline Timestamp FromEpochMillis(std::int64_t ms) {
    return Timestamp(std::chrono::milliseconds(ms));
}
inline Timestamp FromEpochSeconds(std::int64_t s) {
    return Timestamp(std::chrono::seconds(s));
}
inline std::int64_t ToEpochMillis(Timestamp t) {
    return t.time_since_epoch().count();
}
inline std::int64_t ToEpochSeconds(Timestamp t) {
    return std::chrono::floor<std::chrono::seconds>(t)
        .time_since_epoch()
        .count();
}

// Parses "YYYY-MM-DDTHH:MM:SS[.fraction]Z" (also "+00:00"). Fractions finer
// than a millisecond are truncated.
Result<Timestamp> ParseIso8601(std::string_view text);
// "YYYY-MM-DDTHH:MM:SS.mmmZ"
std::string FormatIso8601(Timestamp t);

}  // namespace kestrel::types
