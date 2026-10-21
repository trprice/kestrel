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
        {3, "number quotes per venue and symbol", R"sql(
ALTER TABLE quotes ADD COLUMN seq BIGINT;
UPDATE quotes q SET seq = r.rn
FROM (SELECT tableoid AS t, ctid AS c,
             row_number() OVER (PARTITION BY venue, symbol
                                ORDER BY received_time) AS rn
      FROM quotes) r
WHERE q.tableoid = r.t AND q.ctid = r.c;
ALTER TABLE quotes ALTER COLUMN seq SET NOT NULL;
ALTER TABLE quotes ADD CONSTRAINT quotes_seq_unique
    UNIQUE (venue, symbol, received_time, seq);
CREATE INDEX quotes_venue_symbol_seq
    ON quotes (venue, symbol, seq DESC);
)sql"},
    };
    return kMigrations;
}

}  // namespace kestrel::db
