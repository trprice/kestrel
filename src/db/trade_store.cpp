#include "kestrel/db/trade_store.hpp"

#include <algorithm>

namespace kestrel::db {

using types::Result;

namespace {

constexpr std::size_t kFillColumns = 12;
constexpr std::size_t kPnlColumns = 8;
// PostgreSQL allows 65535 bind parameters per statement.
constexpr std::size_t kRowsPerInsert = 1000;

std::string Values(std::size_t rows, std::size_t columns) {
    std::string sql;
    for (std::size_t r = 0; r < rows; ++r) {
        sql += r ? ", (" : "(";
        for (std::size_t c = 0; c < columns; ++c) {
            sql += (c ? ", $" : "$") + std::to_string(r * columns + c + 1);
        }
        sql += ")";
    }
    return sql;
}

std::optional<std::string> Text(const std::optional<types::Decimal>& d) {
    return d ? std::optional<std::string>(d->ToString()) : std::nullopt;
}

std::optional<types::Decimal> ParseOptional(
    const std::optional<std::string>& text) {
    if (!text) {
        return std::nullopt;
    }
    auto d = types::Decimal::Parse(*text);
    return d ? std::optional<types::Decimal>(*d) : std::nullopt;
}

types::Decimal ParseDecimal(const std::optional<std::string>& text) {
    return ParseOptional(text).value_or(types::Decimal());
}

constexpr const char* kMillis = "(extract(epoch FROM ";

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
            params.emplace_back(f.fill_id);
            params.emplace_back(f.order_id);
            params.emplace_back(f.client_order_id);
            params.emplace_back(f.symbol);
            params.emplace_back(std::string(types::ToString(f.side)));
            params.emplace_back(f.quantity.ToString());
            params.emplace_back(f.price.ToString());
            params.emplace_back(f.fee.ToString());
            params.emplace_back(f.fee_asset);
            params.emplace_back(
                f.liquidity == types::Liquidity::kMaker ? "maker" : "taker");
            params.emplace_back(types::FormatIso8601(f.time));
        }
        auto result = mDatabase->Query(
            "INSERT INTO fills (run_id, fill_id, order_id, client_order_id, "
            "symbol, side, quantity, price, fee, fee_asset, liquidity, "
            "fill_time) VALUES " +
                Values(batch.size(), kFillColumns) +
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
        std::string("SELECT fill_id, order_id, client_order_id, symbol, side, "
                    "quantity, price, fee, fee_asset, liquidity, ") +
            kMillis +
            "fill_time) * 1000)::bigint FROM fills WHERE run_id = $1 "
            "ORDER BY fill_time, fill_id",
        {std::to_string(mRunId)});
    if (!rows) {
        return std::unexpected(rows.error());
    }
    std::vector<types::Fill> fills;
    for (const auto& row : *rows) {
        types::Fill f;
        f.fill_id = *row[0];
        f.order_id = *row[1];
        f.client_order_id = *row[2];
        f.symbol = *row[3];
        f.side = *row[4] == "buy" ? types::Side::kBuy : types::Side::kSell;
        f.quantity = ParseDecimal(row[5]);
        f.price = ParseDecimal(row[6]);
        f.fee = ParseDecimal(row[7]);
        f.fee_asset = *row[8];
        f.liquidity = *row[9] == "maker" ? types::Liquidity::kMaker
                                         : types::Liquidity::kTaker;
        f.time = types::FromEpochMillis(std::stoll(*row[10]));
        fills.push_back(std::move(f));
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

}  // namespace kestrel::db
