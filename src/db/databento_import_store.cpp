#include "kestrel/db/databento_import_store.hpp"

#include <algorithm>
#include <optional>

#include "sql_rows.hpp"

namespace kestrel::db {

using types::Result;

namespace {

constexpr std::size_t kColumns = 6;

}  // namespace

Result<std::vector<DatabentoImportStore::Interval>> DatabentoImportStore::Load(
    const std::string& venue, const std::string& dataset,
    const types::Symbol& symbol, const std::string& target,
    types::Timestamp from, types::Timestamp to) const {
    auto rows = mDatabase->Query(
        std::string("SELECT ") + sql::kMillis +
            "slice_start) * 1000)::bigint, " + sql::kMillis +
            "slice_end) * 1000)::bigint FROM databento_imports WHERE venue = "
            "$1 AND dataset = $2 AND symbol = $3 AND target = $4 AND "
            "slice_end > $5 AND slice_start < $6 ORDER BY slice_start, "
            "slice_end",
        {venue, dataset, symbol, target, types::FormatIso8601(from),
         types::FormatIso8601(to)});
    if (!rows) {
        return std::unexpected(rows.error());
    }
    std::vector<Interval> merged;
    merged.reserve(rows->size());
    for (const auto& row : *rows) {
        if (row.size() < 2) {
            return types::MakeError(types::ErrorCategory::kParse,
                                    "databento_imports: incomplete row");
        }
        auto s = sql::ParseMillis(row[0], "databento_imports: slice_start");
        auto e = sql::ParseMillis(row[1], "databento_imports: slice_end");
        if (!s || !e) {
            return std::unexpected(!s ? s.error() : e.error());
        }
        if (*e <= *s) {
            continue;
        }
        if (!merged.empty() && *s <= merged.back().second) {
            merged.back().second = std::max(merged.back().second, *e);
        } else {
            merged.emplace_back(*s, *e);
        }
    }
    return merged;
}

Result<std::optional<types::Timestamp>> DatabentoImportStore::FindOffGridBar(
    const std::string& venue, const types::Symbol& symbol,
    types::Timeframe timeframe, int anchor_minutes, int length_minutes) const {
    if (length_minutes <= 0) {
        return types::MakeError(types::ErrorCategory::kInvalidArgument,
                                "FindOffGridBar: length must be positive");
    }
    // The minute of the New York day less the anchor, which is on the grid
    // when the length divides it (mod is 0 for a negative multiple too).
    auto rows = mDatabase->Query(
        std::string("SELECT ") + sql::kMillis +
            "open_time) * 1000)::bigint FROM (SELECT open_time, open_time AT "
            "TIME ZONE 'America/New_York' AS ny FROM bars WHERE venue = $1 AND "
            "symbol = $2 AND timeframe = $3) b WHERE mod((extract(hour FROM "
            "ny) * 60 + extract(minute FROM ny))::int - $4::int, $5::int) <> 0 "
            "ORDER BY open_time LIMIT 1",
        {venue, symbol, std::string(types::ToString(timeframe)),
         std::to_string(anchor_minutes), std::to_string(length_minutes)});
    if (!rows) {
        return std::unexpected(rows.error());
    }
    if (rows->empty() || rows->front().empty()) {
        return std::optional<types::Timestamp>{};
    }
    auto t = sql::ParseMillis(rows->front()[0], "bars: open_time");
    if (!t) {
        return std::unexpected(t.error());
    }
    return std::optional<types::Timestamp>(*t);
}

Result<void> DatabentoImportStore::Record(const std::string& venue,
                                          const std::string& dataset,
                                          const types::Symbol& symbol,
                                          std::span<const std::string> targets,
                                          std::span<const Interval> slices) {
    if (targets.empty() || slices.empty()) {
        return {};
    }
    struct Entry {
        const std::string* target;
        Interval slice;
    };
    std::vector<Entry> entries;
    entries.reserve(targets.size() * slices.size());
    for (const std::string& target : targets) {
        for (const Interval& slice : slices) {
            if (slice.second > slice.first) {
                entries.push_back(Entry{&target, slice});
            }
        }
    }
    for (std::size_t done = 0; done < entries.size();) {
        const std::size_t count =
            std::min(sql::kRowsPerInsert, entries.size() - done);
        std::vector<std::optional<std::string>> params;
        params.reserve(count * kColumns);
        for (std::size_t i = 0; i < count; ++i) {
            const Entry& e = entries[done + i];
            params.emplace_back(venue);
            params.emplace_back(dataset);
            params.emplace_back(symbol);
            params.emplace_back(*e.target);
            params.emplace_back(types::FormatIso8601(e.slice.first));
            params.emplace_back(types::FormatIso8601(e.slice.second));
        }
        auto res = mDatabase->Query(
            "INSERT INTO databento_imports (venue, dataset, symbol, target, "
            "slice_start, slice_end) VALUES " +
                sql::Values(count, kColumns) + " ON CONFLICT DO NOTHING",
            params);
        if (!res) {
            return std::unexpected(res.error());
        }
        done += count;
    }
    return {};
}

}  // namespace kestrel::db
