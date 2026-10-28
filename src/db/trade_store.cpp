#include "kestrel/db/trade_store.hpp"

#include <algorithm>

#include "sql_rows.hpp"

namespace kestrel::db {

using types::Result;

namespace {

using sql::kMillis;
using sql::kRowsPerInsert;
using sql::ParseDecimal;
using sql::ParseOptional;
using sql::Text;
using sql::Values;

constexpr std::size_t kFillColumns = 1 + sql::kFillColumns;  // run_id first
constexpr std::size_t kPnlColumns = 8;
constexpr std::size_t kLatencyColumns = 12;

}  // namespace

Result<std::unique_ptr<TradeStore>> TradeStore::StartRun(
    std::shared_ptr<Database> database, const RunInfo& info) {
    auto rows = database->Query(
        "INSERT INTO runs (strategy, venue, mode, started_at) "
        "VALUES ($1, $2, $3, $4) RETURNING run_id",
        {info.strategy, info.venue, info.mode,
         types::FormatIso8601(info.started)});
    if (!rows) {
        return std::unexpected(rows.error());
    }
    return std::unique_ptr<TradeStore>(
        new TradeStore(std::move(database), std::stoll(*(*rows)[0][0])));
}

Result<void> TradeStore::WriteFills(std::span<const types::Fill> fills) {
    for (std::size_t done = 0; done < fills.size();) {
        const auto batch =
            fills.subspan(done, std::min(kRowsPerInsert, fills.size() - done));
        std::vector<std::optional<std::string>> params;
        params.reserve(batch.size() * kFillColumns);
        for (const auto& f : batch) {
            params.emplace_back(std::to_string(mRunId));
            sql::AppendFill(params, f);
        }
        auto result = mDatabase->Query(
            std::string("INSERT INTO fills (run_id, ") + sql::kFillColumnNames +
                ") VALUES " + Values(batch.size(), kFillColumns) +
                " ON CONFLICT (run_id, fill_id) DO NOTHING",
            params);
        if (!result) {
            return std::unexpected(result.error());
        }
        done += batch.size();
    }
    return {};
}

Result<void> TradeStore::WritePnl(types::Timestamp time,
                                  std::span<const trading::PnlRow> rows) {
    for (std::size_t done = 0; done < rows.size();) {
        const auto batch =
            rows.subspan(done, std::min(kRowsPerInsert, rows.size() - done));
        std::vector<std::optional<std::string>> params;
        params.reserve(batch.size() * kPnlColumns);
        for (const auto& r : batch) {
            params.emplace_back(std::to_string(mRunId));
            params.emplace_back(types::FormatIso8601(time));
            params.emplace_back(r.symbol);
            params.emplace_back(r.quantity.ToString());
            params.emplace_back(r.average_price.ToString());
            params.emplace_back(Text(r.mark_price));
            params.emplace_back(r.realized.ToString());
            params.emplace_back(Text(r.unrealized));
        }
        auto result = mDatabase->Query(
            "INSERT INTO pnl_snapshots (run_id, snapshot_time, symbol, "
            "quantity, average_price, mark_price, realized, unrealized) "
            "VALUES " +
                Values(batch.size(), kPnlColumns) +
                " ON CONFLICT (run_id, snapshot_time, symbol) DO NOTHING",
            params);
        if (!result) {
            return std::unexpected(result.error());
        }
        done += batch.size();
    }
    return {};
}

Result<void> TradeStore::WriteLatency(
    types::Timestamp start, types::Timestamp end,
    std::span<const types::EndpointLatency> rows) {
    for (std::size_t done = 0; done < rows.size();) {
        const auto batch =
            rows.subspan(done, std::min(kRowsPerInsert, rows.size() - done));
        std::vector<std::optional<std::string>> params;
        params.reserve(batch.size() * kLatencyColumns);
        for (const auto& r : batch) {
            params.emplace_back(std::to_string(mRunId));
            params.emplace_back(types::FormatIso8601(start));
            params.emplace_back(types::FormatIso8601(end));
            params.emplace_back(r.endpoint);
            params.emplace_back(std::to_string(r.count));
            params.emplace_back(std::to_string(r.errors));
            params.emplace_back(std::to_string(r.rate_limited));
            params.emplace_back(std::to_string(r.min_ms));
            params.emplace_back(std::to_string(r.p50_ms));
            params.emplace_back(std::to_string(r.p95_ms));
            params.emplace_back(std::to_string(r.p99_ms));
            params.emplace_back(std::to_string(r.max_ms));
        }
        auto result = mDatabase->Query(
            "INSERT INTO venue_latency (run_id, window_start, window_end, "
            "endpoint, calls, errors, rate_limited, min_ms, p50_ms, p95_ms, "
            "p99_ms, max_ms) VALUES " +
                Values(batch.size(), kLatencyColumns) +
                " ON CONFLICT (run_id, window_start, endpoint) DO NOTHING",
            params);
        if (!result) {
            return std::unexpected(result.error());
        }
        done += batch.size();
    }
    return {};
}

Result<void> TradeStore::Finish(types::Timestamp time) {
    auto result =
        mDatabase->Query("UPDATE runs SET ended_at = $2 WHERE run_id = $1",
                         {std::to_string(mRunId), types::FormatIso8601(time)});
    if (!result) {
        return std::unexpected(result.error());
    }
    return {};
}

Result<std::vector<types::Fill>> TradeStore::LoadFills() const {
    auto rows = mDatabase->Query(
        "SELECT " + sql::FillSelect() +
            " FROM fills WHERE run_id = $1 ORDER BY fill_time, fill_id",
        {std::to_string(mRunId)});
    if (!rows) {
        return std::unexpected(rows.error());
    }
    std::vector<types::Fill> fills;
    for (const auto& row : *rows) {
        auto fill = sql::ParseFill(row);
        if (!fill) {
            return std::unexpected(fill.error());
        }
        fills.push_back(std::move(*fill));
    }
    return fills;
}

Result<std::vector<TradeStore::StoredPnl>> TradeStore::LoadPnl() const {
    auto rows = mDatabase->Query(
        std::string("SELECT ") + kMillis +
            "snapshot_time) * 1000)::bigint, symbol, quantity, average_price, "
            "mark_price, realized, unrealized FROM pnl_snapshots "
            "WHERE run_id = $1 ORDER BY snapshot_time, symbol",
        {std::to_string(mRunId)});
    if (!rows) {
        return std::unexpected(rows.error());
    }
    std::vector<StoredPnl> out;
    for (const auto& row : *rows) {
        StoredPnl s;
        s.time = types::FromEpochMillis(std::stoll(*row[0]));
        s.row.symbol = *row[1];
        s.row.quantity = ParseDecimal(row[2]);
        s.row.average_price = ParseDecimal(row[3]);
        s.row.mark_price = ParseOptional(row[4]);
        s.row.realized = ParseDecimal(row[5]);
        s.row.unrealized = ParseOptional(row[6]);
        out.push_back(std::move(s));
    }
    return out;
}

Result<std::vector<TradeStore::StoredLatency>> TradeStore::LoadLatency() const {
    auto rows = mDatabase->Query(
        std::string("SELECT ") + kMillis + "window_start) * 1000)::bigint, " +
            kMillis +
            "window_end) * 1000)::bigint, endpoint, calls, errors, "
            "rate_limited, min_ms, p50_ms, p95_ms, p99_ms, max_ms "
            "FROM venue_latency WHERE run_id = $1 "
            "ORDER BY window_start, endpoint",
        {std::to_string(mRunId)});
    if (!rows) {
        return std::unexpected(rows.error());
    }
    std::vector<StoredLatency> out;
    for (const auto& row : *rows) {
        StoredLatency s;
        s.start = types::FromEpochMillis(std::stoll(*row[0]));
        s.end = types::FromEpochMillis(std::stoll(*row[1]));
        s.row.endpoint = *row[2];
        s.row.count = std::stoull(*row[3]);
        s.row.errors = std::stoull(*row[4]);
        s.row.rate_limited = std::stoull(*row[5]);
        s.row.min_ms = std::stod(*row[6]);
        s.row.p50_ms = std::stod(*row[7]);
        s.row.p95_ms = std::stod(*row[8]);
        s.row.p99_ms = std::stod(*row[9]);
        s.row.max_ms = std::stod(*row[10]);
        out.push_back(std::move(s));
    }
    return out;
}

}  // namespace kestrel::db
