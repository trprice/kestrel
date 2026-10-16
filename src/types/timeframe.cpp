#include "kestrel/types/timeframe.hpp"

#include <array>
#include <string>

namespace kestrel::types {
namespace {

struct Entry {
  Timeframe tf;
  std::string_view name;
  long seconds;  // 0 for calendar-based
};

constexpr std::array<Entry, 17> kTable{{
    {Timeframe::kM1, "1m", 60},          {Timeframe::kM3, "3m", 180},
    {Timeframe::kM5, "5m", 300},         {Timeframe::kM10, "10m", 600},
    {Timeframe::kM15, "15m", 900},       {Timeframe::kM30, "30m", 1800},
    {Timeframe::kH1, "1h", 3600},        {Timeframe::kH2, "2h", 7200},
    {Timeframe::kH4, "4h", 14400},       {Timeframe::kH6, "6h", 21600},
    {Timeframe::kH8, "8h", 28800},       {Timeframe::kH12, "12h", 43200},
    {Timeframe::kD1, "1d", 86400},       {Timeframe::kD3, "3d", 259200},
    {Timeframe::kW1, "1w", 604800},      {Timeframe::kD15, "15d", 1296000},
    {Timeframe::kMo1, "1M", 0},
}};

const Entry& Lookup(Timeframe tf) {
  for (const auto& e : kTable)
    if (e.tf == tf) return e;
  return kTable[0];
}

}  // namespace

std::vector<Timeframe> AllTimeframes() {
  std::vector<Timeframe> out;
  for (const auto& e : kTable) out.push_back(e.tf);
  return out;
}

std::string_view ToString(Timeframe tf) { return Lookup(tf).name; }

Result<Timeframe> ParseTimeframe(std::string_view text) {
  for (const auto& e : kTable)
    if (e.name == text) return e.tf;
  return MakeError(ErrorCategory::kInvalidArgument, "unknown timeframe '" + std::string(text) + "'");
}

std::optional<std::chrono::seconds> FixedDuration(Timeframe tf) {
  const long s = Lookup(tf).seconds;
  if (s == 0) return std::nullopt;
  return std::chrono::seconds(s);
}

}  // namespace kestrel::types
