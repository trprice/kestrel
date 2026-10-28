#pragma once

// Helpers shared by the stores in this directory: multi-row INSERT
// placeholders, Decimal and timestamp text, and the columns of a fill row,
// which the `fills` and `account_fills` tables have in common.

#include <cstddef>
#include <exception>
#include <optional>
#include <string>
#include <vector>

#include "kestrel/db/database.hpp"
#include "kestrel/types/decimal.hpp"
#include "kestrel/types/error.hpp"
#include "kestrel/types/orders.hpp"
#include "kestrel/types/time.hpp"

namespace kestrel::db::sql {

// PostgreSQL allows 65535 bind parameters per statement.
inline constexpr std::size_t kRowsPerInsert = 1000;

// "($1, $2), ($3, $4)" for `rows` rows of `columns` parameters each.
inline std::string Values(std::size_t rows, std::size_t columns) {
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

inline std::optional<std::string> Text(const std::optional<types::Decimal>& d) {
    return d ? std::optional<std::string>(d->ToString()) : std::nullopt;
}

inline std::optional<types::Decimal> ParseOptional(
    const std::optional<std::string>& text) {
    if (!text) {
        return std::nullopt;
    }
    auto d = types::Decimal::Parse(*text);
    return d ? std::optional<types::Decimal>(*d) : std::nullopt;
}

inline types::Decimal ParseDecimal(const std::optional<std::string>& text) {
    return ParseOptional(text).value_or(types::Decimal());
}

// Opens a timestamp column as epoch milliseconds:
// kMillis + "col) * 1000)::bigint".
inline constexpr const char* kMillis = "(extract(epoch FROM ";

// The fill columns, in the order AppendFill binds them and ParseFill reads
// them (with fill_time selected as epoch milliseconds).
inline constexpr std::size_t kFillColumns = 11;
inline constexpr const char* kFillColumnNames =
    "fill_id, order_id, client_order_id, symbol, side, quantity, price, fee, "
    "fee_asset, liquidity, fill_time";
inline std::string FillSelect() {
    return std::string(
               "fill_id, order_id, client_order_id, symbol, side, quantity, "
               "price, fee, fee_asset, liquidity, ") +
           kMillis + "fill_time) * 1000)::bigint";
}

inline void AppendFill(std::vector<std::optional<std::string>>& params,
                       const types::Fill& f) {
    params.emplace_back(f.fill_id);
    params.emplace_back(f.order_id);
    params.emplace_back(f.client_order_id);
    params.emplace_back(f.symbol);
    params.emplace_back(std::string(types::ToString(f.side)));
    params.emplace_back(f.quantity.ToString());
    params.emplace_back(f.price.ToString());
    params.emplace_back(f.fee.ToString());
    params.emplace_back(f.fee_asset);
    params.emplace_back(f.liquidity == types::Liquidity::kMaker ? "maker"
                                                                : "taker");
    params.emplace_back(types::FormatIso8601(f.time));
}

// Reads the kFillColumns columns of FillSelect() from `row`, starting at
// `first`. A row that is too short, a NULL, or a side, liquidity or time that
// is not what AppendFill writes is kParse rather than a guess, so a bad row
// (or a query that does not match FillSelect) is noticed.
inline types::Result<types::Fill> ParseFill(const Database::Row& row,
                                            std::size_t first = 0) {
    const auto bad = [](const std::string& why) {
        return types::MakeError(types::ErrorCategory::kParse,
                                "fill row: " + why);
    };
    if (row.size() < first + kFillColumns) {
        return bad("has " + std::to_string(row.size()) + " columns, needs " +
                   std::to_string(first + kFillColumns));
    }
    for (std::size_t i = 0; i < kFillColumns; ++i) {
        if (!row[first + i]) {
            return bad("column " + std::to_string(i) + " is NULL");
        }
    }
    const auto at = [&](std::size_t i) -> const std::string& {
        return *row[first + i];
    };
    types::Fill f;
    f.fill_id = at(0);
    f.order_id = at(1);
    f.client_order_id = at(2);
    f.symbol = at(3);
    if (at(4) == "buy") {
        f.side = types::Side::kBuy;
    } else if (at(4) == "sell") {
        f.side = types::Side::kSell;
    } else {
        return bad("unknown side '" + at(4) + "' for " + f.fill_id);
    }
    f.quantity = ParseDecimal(row[first + 5]);
    f.price = ParseDecimal(row[first + 6]);
    f.fee = ParseDecimal(row[first + 7]);
    f.fee_asset = at(8);
    if (at(9) == "maker") {
        f.liquidity = types::Liquidity::kMaker;
    } else if (at(9) == "taker") {
        f.liquidity = types::Liquidity::kTaker;
    } else {
        return bad("unknown liquidity '" + at(9) + "' for " + f.fill_id);
    }
    try {
        f.time = types::FromEpochMillis(std::stoll(at(10)));
    } catch (const std::exception&) {
        return bad("unreadable time '" + at(10) + "' for " + f.fill_id);
    }
    return f;
}

}  // namespace kestrel::db::sql
