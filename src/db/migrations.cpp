#include "migrations.hpp"

namespace kestrel::db {

const std::vector<Migration>& Migrations() {
    static const std::vector<Migration> kMigrations = {
        {1, "create bars hypertable", R"sql(
CREATE TABLE bars (
    venue     TEXT        NOT NULL,
    symbol    TEXT        NOT NULL,
    timeframe TEXT        NOT NULL,
    open_time TIMESTAMPTZ NOT NULL,
    open      NUMERIC     NOT NULL,
    high      NUMERIC     NOT NULL,
    low       NUMERIC     NOT NULL,
    close     NUMERIC     NOT NULL,
    volume    NUMERIC     NOT NULL,
    PRIMARY KEY (venue, symbol, timeframe, open_time)
);
SELECT create_hypertable('bars', 'open_time',
                         chunk_time_interval => INTERVAL '30 days');
)sql"},
        {2, "create quotes hypertable", R"sql(
CREATE TABLE quotes (
    venue         TEXT        NOT NULL,
    symbol        TEXT        NOT NULL,
    received_time TIMESTAMPTZ NOT NULL,
    exchange_time TIMESTAMPTZ,
    bid           NUMERIC,
    bid_size      NUMERIC,
    ask           NUMERIC,
    ask_size      NUMERIC,
    last          NUMERIC,
    last_size     NUMERIC
);
SELECT create_hypertable('quotes', 'received_time',
                         chunk_time_interval => INTERVAL '1 day');
CREATE INDEX quotes_venue_symbol_time
    ON quotes (venue, symbol, received_time DESC);
)sql"},
    };
    return kMigrations;
}

}  // namespace kestrel::db
