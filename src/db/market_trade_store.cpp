#include "kestrel/db/market_trade_store.hpp"

#include <algorithm>

#include "sql_rows.hpp"

namespace kestrel::db {

using types::ErrorCategory;
using types::MakeError;
using types::Result;

namespace {

constexpr std::size_t kColumns = 7;

}  // namespace

Result<std::size_t> MarketTradeStore::Save(
    const std::string& venue, const types::Symbol& symbol,
    std::span<const types::MarketTrade> trades) {
    std::size_t stored = 0;
    for (std::size_t done = 0; done < trades.size();) {
        const auto batch = trades.subspan(
            done, std::min(sql::kRowsPerInsert, trades.size() - done));
        std::vector<std::optional<std::string>> params;
        params.reserve(batch.size() * kColumns);
        for (const auto& t : batch) {
            params.emplace_back(venue);
            params.emplace_back(symbol);
            params.emplace_back(std::to_string(t.trade_id));
            params.emplace_back(types::FormatIso8601(t.time));
            params.emplace_back(t.price.ToString());
            params.emplace_back(t.quantity.ToString());
            params.emplace_back(std::string(types::ToString(t.taker_side)));
        }
        auto rows = mDatabase->Query(
            "INSERT INTO market_trades (venue, symbol, trade_id, trade_time, "
            "price, quantity, taker_side) VALUES " +
                sql::Values(batch.size(), kColumns) +
                " ON CONFLICT DO NOTHING RETURNING 1",
            params);
        if (!rows) {
            return std::unexpected(rows.error());
        }
        stored += rows->size();
        done += batch.size();
    }
    return stored;
}

Result<std::optional<types::Timestamp>> MarketTradeStore::Newest(
    const std::string& venue, const types::Symbol& symbol,
    types::Timestamp from, types::Timestamp to) const {
    auto rows = mDatabase->Query(
        std::string("SELECT ") + sql::kMillis +
            "max(trade_time)) * 1000)::bigint FROM market_trades "
            "WHERE venue = $1 AND symbol = $2 AND trade_time >= $3 AND "
            "trade_time < $4",
        {venue, symbol, types::FormatIso8601(from), types::FormatIso8601(to)});
    if (!rows) {
        return std::unexpected(rows.error());
    }
    if (rows->empty() || !(*rows)[0][0]) {
        return std::optional<types::Timestamp>{};
    }
    return std::optional<types::Timestamp>(
        types::FromEpochMillis(std::stoll(*(*rows)[0][0])));
}

Result<std::vector<types::MarketTrade>> MarketTradeStore::Load(
    const std::string& venue, const types::Symbol& symbol,
    types::Timestamp from, types::Timestamp to) const {
    auto rows = mDatabase->Query(
        std::string("SELECT trade_id, ") + sql::kMillis +
            "trade_time) * 1000)::bigint, price, quantity, taker_side FROM "
            "market_trades WHERE venue = $1 AND symbol = $2 AND "
            "trade_time >= $3 AND trade_time < $4 ORDER BY trade_time, "
            "trade_id",
        {venue, symbol, types::FormatIso8601(from), types::FormatIso8601(to)});
    if (!rows) {
        return std::unexpected(rows.error());
    }
    std::vector<types::MarketTrade> trades;
    trades.reserve(rows->size());
    for (const auto& row : *rows) {
        if (row.size() < 5 || !row[0] || !row[1] || !row[2] || !row[3] ||
            !row[4]) {
            return MakeError(ErrorCategory::kParse,
                             "market_trades: incomplete row");
        }
        types::MarketTrade t;
        auto price = types::Decimal::Parse(*row[2]);
        auto quantity = types::Decimal::Parse(*row[3]);
        if (!price || !quantity) {
            return MakeError(ErrorCategory::kParse,
                             "market_trades: bad price or quantity");
        }
        if (*row[4] != "buy" && *row[4] != "sell") {
            return MakeError(ErrorCategory::kParse,
                             "market_trades: bad taker_side '" + *row[4] + "'");
        }
        t.trade_id = std::stoll(*row[0]);
        t.time = types::FromEpochMillis(std::stoll(*row[1]));
        t.price = *price;
        t.quantity = *quantity;
        t.taker_side = *row[4] == "buy" ? types::Side::kBuy : types::Side::kSell;
        trades.push_back(t);
    }
    return trades;
}

}  // namespace kestrel::db
