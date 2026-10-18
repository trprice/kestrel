#pragma once

#include <chrono>
#include <optional>
#include <string_view>
#include <vector>

#include "kestrel/types/error.hpp"

namespace kestrel::types {

// Union of the bar intervals offered by Schwab, Kraken and Binance.US.
// Each DataSource maps these to its own format and returns kUnsupported for
// intervals it does not offer:
//
//   Schwab      1m 5m 10m 15m 30m, 1d 1w 1M
//   Kraken      1m 5m 15m 30m 1h 4h 1d 1w 15d
//   Binance.US  1m 3m 5m 15m 30m 1h 2h 4h 6h 8h 12h 1d 3d 1w 1M
enum class Timeframe {
    kM1,
    kM3,
    kM5,
    kM10,
    kM15,
    kM30,
    kH1,
    kH2,
    kH4,
    kH6,
    kH8,
    kH12,
    kD1,
    kD3,
    kW1,
    kD15,
    kMo1,
};

std::vector<Timeframe> AllTimeframes();
std::string_view ToString(Timeframe tf);  // "1m", "4h", "1d", "1M", ...
Result<Timeframe> ParseTimeframe(std::string_view text);

// Length of a bar; nullopt for calendar-based intervals (kMo1).
std::optional<std::chrono::seconds> FixedDuration(Timeframe tf);

}  // namespace kestrel::types
