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
        {4, "create run, fill and P&L tables", R"sql(
CREATE TABLE runs (
    run_id     BIGSERIAL   PRIMARY KEY,
    strategy   TEXT        NOT NULL,
    venue      TEXT        NOT NULL,
    mode       TEXT        NOT NULL,
    started_at TIMESTAMPTZ NOT NULL,
    ended_at   TIMESTAMPTZ
);
CREATE TABLE fills (
    run_id          BIGINT      NOT NULL REFERENCES runs (run_id),
    fill_id         TEXT        NOT NULL,
    order_id        TEXT        NOT NULL,
    client_order_id TEXT        NOT NULL,
    symbol          TEXT        NOT NULL,
    side            TEXT        NOT NULL,
    quantity        NUMERIC     NOT NULL,
    price           NUMERIC     NOT NULL,
    fee             NUMERIC     NOT NULL,
    fee_asset       TEXT        NOT NULL,
    liquidity       TEXT        NOT NULL,
    fill_time       TIMESTAMPTZ NOT NULL,
    PRIMARY KEY (run_id, fill_id)
);
CREATE INDEX fills_run_time ON fills (run_id, fill_time);
CREATE TABLE pnl_snapshots (
    run_id        BIGINT      NOT NULL REFERENCES runs (run_id),
    snapshot_time TIMESTAMPTZ NOT NULL,
    symbol        TEXT        NOT NULL,
    quantity      NUMERIC     NOT NULL,
    average_price NUMERIC     NOT NULL,
    mark_price    NUMERIC,
    realized      NUMERIC     NOT NULL,
    unrealized    NUMERIC,
    PRIMARY KEY (run_id, snapshot_time, symbol)
);
CREATE VIEW pnl_totals AS
SELECT run_id, snapshot_time,
       sum(realized)                       AS realized,
       sum(COALESCE(unrealized, 0))        AS unrealized,
       sum(realized + COALESCE(unrealized, 0)) AS total
FROM pnl_snapshots
GROUP BY run_id, snapshot_time;
)sql"},
        {5, "create venue latency table", R"sql(
CREATE TABLE venue_latency (
    run_id       BIGINT           NOT NULL REFERENCES runs (run_id),
    window_start TIMESTAMPTZ      NOT NULL,
    window_end   TIMESTAMPTZ      NOT NULL,
    endpoint     TEXT             NOT NULL,
    calls        BIGINT           NOT NULL,
    errors       BIGINT           NOT NULL,
    rate_limited BIGINT           NOT NULL,
    min_ms       DOUBLE PRECISION NOT NULL,
    p50_ms       DOUBLE PRECISION NOT NULL,
    p95_ms       DOUBLE PRECISION NOT NULL,
    p99_ms       DOUBLE PRECISION NOT NULL,
    max_ms       DOUBLE PRECISION NOT NULL,
    PRIMARY KEY (run_id, window_start, endpoint)
);
)sql"},
        {6, "create account fill table", R"sql(
CREATE TABLE account_fills (
    venue           TEXT        NOT NULL,
    fill_id         TEXT        NOT NULL,
    order_id        TEXT        NOT NULL,
    client_order_id TEXT        NOT NULL,
    symbol          TEXT        NOT NULL,
    side            TEXT        NOT NULL,
    quantity        NUMERIC     NOT NULL,
    price           NUMERIC     NOT NULL,
    fee             NUMERIC     NOT NULL,
    fee_asset       TEXT        NOT NULL,
    liquidity       TEXT        NOT NULL,
    fill_time       TIMESTAMPTZ NOT NULL,
    synced_at       TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (venue, fill_id)
);
CREATE INDEX account_fills_venue_symbol_time
    ON account_fills (venue, symbol, fill_time);
)sql"},
    };
    return kMigrations;
}

}  // namespace kestrel::db
