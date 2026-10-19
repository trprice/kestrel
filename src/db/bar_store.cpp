#include "kestrel/db/bar_store.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace kestrel::db {

using types::ErrorCategory;
using types::MakeError;
using types::Result;

namespace {

constexpr std::size_t kColumns = 9;
// PostgreSQL allows 65535 bind parameters per statement.
constexpr std::size_t kRowsPerInsert = 1000;

std::string InsertSql(std::size_t rows) {
    std::string sql =
        "INSERT INTO bars (venue, symbol, timeframe, open_time, open, high, "
        "low, close, volume) VALUES ";
    for (std::size_t r = 0; r < rows; ++r) {
        sql += r ? ", (" : "(";
        for (std::size_t c = 0; c < kColumns; ++c) {
            sql += (c ? ", $" : "$") + std::to_string(r * kColumns + c + 1);
        }
        sql += ")";
    }
    sql +=
        " ON CONFLICT (venue, symbol, timeframe, open_time) DO UPDATE SET "
        "open = EXCLUDED.open, high = EXCLUDED.high, low = EXCLUDED.low, "
        "close = EXCLUDED.close, volume = EXCLUDED.volume";
    return sql;
}

}  // namespace

Result<std::size_t> BarStore::Save(const std::string& venue,
                                   const types::Symbol& symbol,
                                   types::Timeframe timeframe,
                                   std::span<const types::Bar> bars) {
    const std::string tf(types::ToString(timeframe));
    std::size_t written = 0;
    while (written < bars.size()) {
        const auto batch = bars.subspan(
            written, std::min(kRowsPerInsert, bars.size() - written));
        std::vector<std::optional<std::string>> params;
        params.reserve(batch.size() * kColumns);
        for (const auto& b : batch) {
            params.emplace_back(venue);
            params.emplace_back(symbol);
            params.emplace_back(tf);
            params.emplace_back(types::FormatIso8601(b.open_time));
            params.emplace_back(b.open.ToString());
            params.emplace_back(b.high.ToString());
            params.emplace_back(b.low.ToString());
            params.emplace_back(b.close.ToString());
            params.emplace_back(b.volume.ToString());
        }
        auto result = mDatabase->Query(InsertSql(batch.size()), params);
        if (!result) {
            return std::unexpected(result.error());
        }
        written += batch.size();
    }
    return written;
}

Result<std::vector<types::Bar>> BarStore::Load(const std::string& venue,
                                               const types::Symbol& symbol,
                                               types::Timeframe timeframe,
                                               types::Timestamp start,
                                               types::Timestamp end) {
    auto rows = mDatabase->Query(
        "SELECT (extract(epoch FROM open_time) * 1000)::bigint, open, high, "
        "low, close, volume FROM bars WHERE venue = $1 AND symbol = $2 AND "
        "timeframe = $3 AND open_time >= $4 AND open_time < $5 "
        "ORDER BY open_time",
        {venue, symbol, std::string(types::ToString(timeframe)),
         types::FormatIso8601(start), types::FormatIso8601(end)});
    if (!rows) {
        return std::unexpected(rows.error());
    }
    std::vector<types::Bar> bars;
    bars.reserve(rows->size());
    for (const auto& row : *rows) {
        types::Bar bar;
        bar.open_time = types::FromEpochMillis(std::stoll(*row[0]));
        types::Decimal* fields[] = {&bar.open, &bar.high, &bar.low, &bar.close,
                                    &bar.volume};
        for (std::size_t i = 0; i < 5; ++i) {
            auto d = types::Decimal::Parse(*row[i + 1]);
            if (!d) {
                return std::unexpected(d.error());
            }
            *fields[i] = *d;
        }
        bars.push_back(bar);
    }
    return bars;
}

data::BarCallback BarStore::MakeStreamSink(std::string venue) {
    return [this, venue = std::move(venue)](const types::Symbol& symbol,
                                            types::Timeframe timeframe,
                                            const types::Bar& bar) {
        try {
            auto saved = Save(venue, symbol, timeframe, std::span(&bar, 1));
            if (!saved) {
                spdlog::error("store {} {} {}: {}", venue, symbol,
                              types::ToString(timeframe),
                              saved.error().ToString());
            }
        } catch (const std::exception& e) {
            spdlog::error("store {} {}: {}", venue, symbol, e.what());
        }
    };
}

}  // namespace kestrel::db
