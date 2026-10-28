#include "kestrel/db/account_fill_store.hpp"

#include <algorithm>

#include "sql_rows.hpp"

namespace kestrel::db {

using types::Result;

namespace {

constexpr std::size_t kColumns = 1 + sql::kFillColumns;  // venue first

}  // namespace

Result<std::size_t> AccountFillStore::Save(const std::string& venue,
                                           std::span<const types::Fill> fills) {
    std::size_t stored = 0;
    for (std::size_t done = 0; done < fills.size();) {
        const auto batch = fills.subspan(
            done, std::min(sql::kRowsPerInsert, fills.size() - done));
        std::vector<std::optional<std::string>> params;
        params.reserve(batch.size() * kColumns);
        for (const auto& f : batch) {
            params.emplace_back(venue);
            sql::AppendFill(params, f);
        }
        auto rows = mDatabase->Query(
            std::string("INSERT INTO account_fills (venue, ") +
                sql::kFillColumnNames + ") VALUES " +
                sql::Values(batch.size(), kColumns) +
                " ON CONFLICT (venue, fill_id) DO NOTHING RETURNING 1",
            params);
        if (!rows) {
            return std::unexpected(rows.error());
        }
        stored += rows->size();
        done += batch.size();
    }
    return stored;
}

Result<std::optional<types::Timestamp>> AccountFillStore::Newest(
    const std::string& venue, const types::Symbol& symbol) const {
    auto rows = mDatabase->Query(
        std::string("SELECT ") + sql::kMillis +
            "max(fill_time)) * 1000)::bigint FROM account_fills "
            "WHERE venue = $1 AND symbol = $2",
        {venue, symbol});
    if (!rows) {
        return std::unexpected(rows.error());
    }
    if (rows->empty() || !(*rows)[0][0]) {
        return std::optional<types::Timestamp>{};
    }
    return std::optional<types::Timestamp>(
        types::FromEpochMillis(std::stoll(*(*rows)[0][0])));
}

Result<std::vector<types::Fill>> AccountFillStore::Load(
    const std::string& venue, types::Timestamp from, types::Timestamp to,
    const std::optional<types::Symbol>& symbol) const {
    std::vector<std::optional<std::string>> params = {
        venue, types::FormatIso8601(from), types::FormatIso8601(to)};
    std::string where = "venue = $1 AND fill_time >= $2 AND fill_time < $3";
    if (symbol) {
        where += " AND symbol = $4";
        params.emplace_back(*symbol);
    }
    auto rows = mDatabase->Query("SELECT " + sql::FillSelect() +
                                     " FROM account_fills WHERE " + where +
                                     " ORDER BY fill_time, fill_id",
                                 params);
    if (!rows) {
        return std::unexpected(rows.error());
    }
    std::vector<types::Fill> fills;
    fills.reserve(rows->size());
    for (const auto& row : *rows) {
        auto fill = sql::ParseFill(row);
        if (!fill) {
            return std::unexpected(fill.error());
        }
        fills.push_back(std::move(*fill));
    }
    return fills;
}

Result<types::Timestamp> SyncStart(const AccountFillStore& store,
                                   const std::string& venue,
                                   const std::vector<types::Symbol>& symbols,
                                   types::Timestamp first_since) {
    types::Timestamp start = types::Now();
    for (const types::Symbol& symbol : symbols) {
        auto newest = store.Newest(venue, symbol);
        if (!newest) {
            return std::unexpected(newest.error());
        }
        start = std::min(start, newest->value_or(first_since));
    }
    return start;
}

Result<FillSync> SyncAccountFills(trading::TradingVenue& venue,
                                  AccountFillStore& store,
                                  const std::vector<types::Symbol>& symbols,
                                  types::Timestamp first_since) {
    const std::string name(venue.Name());
    auto start = SyncStart(store, name, symbols, first_since);
    if (!start) {
        return std::unexpected(start.error());
    }
    FillSync sync;
    sync.since = *start;
    auto fills = venue.GetFills(sync.since);
    if (!fills) {
        return std::unexpected(fills.error());
    }
    sync.fetched = fills->size();
    auto stored = store.Save(name, *fills);
    if (!stored) {
        return std::unexpected(stored.error());
    }
    sync.stored = *stored;
    return sync;
}

}  // namespace kestrel::db
