#pragma once

// Shared setup for the database tests: connect, migrate, and load the market
// data checked in under tests/data/market, so they need no venue credentials
// or network.

#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "kestrel/db/bar_store.hpp"
#include "kestrel/db/database.hpp"
#include "kestrel/db/quote_store.hpp"
#include "kestrel/types/timeframe.hpp"
#include "test_helpers.hpp"

namespace kestrel::testing {

// Venues in the checked-in data are prefixed so loading them never touches
// real rows.
inline constexpr const char* kFixtureVenues[] = {"fixture-kraken",
                                                 "fixture-binanceus"};

inline std::string Env(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

inline bool DatabaseConfigured() { return !Env("KESTREL_TEST_DBNAME").empty(); }

inline db::ConnectionOptions TestConnectionOptions() {
    db::ConnectionOptions options;
    options.dbname = Env("KESTREL_TEST_DBNAME");
    options.host = Env("KESTREL_TEST_DBHOST");
    options.user = Env("KESTREL_TEST_DBUSER");
    options.password = Env("KESTREL_TEST_DBPASSWORD");
    if (const auto port = Env("KESTREL_TEST_DBPORT"); !port.empty()) {
        options.port = std::stoi(port);
    }
    return options;
}

// Connects to KESTREL_TEST_DBNAME, which must already exist with the
// timescaledb extension enabled (scripts/setup_test_database.sh does both),
// and applies the migrations.
inline types::Result<std::shared_ptr<db::Database>> ConnectTestDatabase() {
    auto database = db::Database::Connect(TestConnectionOptions());
    if (!database) {
        return std::unexpected(database.error());
    }
    std::shared_ptr<db::Database> shared = std::move(*database);
    if (auto migrated = shared->Migrate(); !migrated) {
        return std::unexpected(migrated.error());
    }
    return shared;
}

struct FixtureBars {
    std::string venue;
    types::Symbol symbol;
    types::Timeframe timeframe;
    std::vector<types::Bar> bars;
};

struct FixtureData {
    std::vector<FixtureBars> bars;
    std::vector<types::Quote> quotes;  // venue/symbol/seq already set
};

namespace detail {

inline std::vector<std::string> Split(const std::string& line) {
    std::vector<std::string> fields;
    std::stringstream ss(line);
    std::string field;
    while (std::getline(ss, field, ',')) {
        fields.push_back(field);
    }
    if (!line.empty() && line.back() == ',') {
        fields.emplace_back();
    }
    return fields;
}

inline std::optional<types::Decimal> OptDecimal(const std::string& text) {
    return text.empty() ? std::nullopt : std::optional(D(text.c_str()));
}

inline std::ifstream OpenData(const char* name) {
    std::ifstream in(std::string(KESTREL_TEST_DATA_DIR) + "/market/" + name);
    if (!in) {
        throw std::runtime_error(std::string("cannot open test data ") + name);
    }
    std::string header;
    std::getline(in, header);
    return in;
}

}  // namespace detail

// Parses tests/data/market/{bars,quotes}.csv. Throws if they are missing or
// malformed.
inline FixtureData ReadFixtureData() {
    FixtureData data;
    std::string line;

    auto bars = detail::OpenData("bars.csv");
    while (std::getline(bars, line)) {
        const auto f = detail::Split(line);
        if (f.size() != 9) {
            throw std::runtime_error("bad bars.csv row: " + line);
        }
        const auto timeframe = types::ParseTimeframe(f[2]);
        if (!timeframe) {
            throw std::runtime_error("bad timeframe: " + f[2]);
        }
        if (data.bars.empty() || data.bars.back().venue != f[0] ||
            data.bars.back().symbol != f[1] ||
            data.bars.back().timeframe != *timeframe) {
            data.bars.push_back({f[0], f[1], *timeframe, {}});
        }
        types::Bar b;
        b.open_time = types::FromEpochMillis(std::stoll(f[3]));
        b.open = D(f[4].c_str());
        b.high = D(f[5].c_str());
        b.low = D(f[6].c_str());
        b.close = D(f[7].c_str());
        b.volume = D(f[8].c_str());
        data.bars.back().bars.push_back(b);
    }

    auto quotes = detail::OpenData("quotes.csv");
    while (std::getline(quotes, line)) {
        const auto f = detail::Split(line);
        if (f.size() != 11) {
            throw std::runtime_error("bad quotes.csv row: " + line);
        }
        types::Quote q;
        q.venue = f[0];
        q.symbol = f[1];
        q.seq = std::stoll(f[2]);
        q.received_time = types::FromEpochMillis(std::stoll(f[3]));
        if (!f[4].empty()) {
            q.exchange_time = types::FromEpochMillis(std::stoll(f[4]));
        }
        q.bid = detail::OptDecimal(f[5]);
        q.bid_size = detail::OptDecimal(f[6]);
        q.ask = detail::OptDecimal(f[7]);
        q.ask_size = detail::OptDecimal(f[8]);
        q.last = detail::OptDecimal(f[9]);
        q.last_size = detail::OptDecimal(f[10]);
        data.quotes.push_back(std::move(q));
    }
    return data;
}

// Replaces the fixture venues' rows in `database` with the checked-in data.
inline types::Result<void> LoadFixtureData(
    const std::shared_ptr<db::Database>& database, const FixtureData& data) {
    for (const char* venue : kFixtureVenues) {
        for (const char* table : {"bars", "quotes"}) {
            if (auto r = database->Query(
                    std::string("DELETE FROM ") + table + " WHERE venue = $1",
                    {venue});
                !r) {
                return std::unexpected(r.error());
            }
        }
    }
    db::BarStore bar_store(database);
    for (const auto& set : data.bars) {
        if (auto r = bar_store.Save(set.venue, set.symbol, set.timeframe,
                                    set.bars);
            !r) {
            return std::unexpected(r.error());
        }
    }
    db::QuoteStore quote_store(database);
    if (auto r = quote_store.Save(data.quotes); !r) {
        return std::unexpected(r.error());
    }
    return {};
}

}  // namespace kestrel::testing
