// Integration tests; run only when KESTREL_TEST_DBNAME is set (see
// bar_store_test.cpp).

#include "kestrel/db/quote_store.hpp"

#include <cstdlib>
#include <string>
#include <vector>

#include "kestrel/db/bar_store.hpp"
#include "test_helpers.hpp"

namespace {

using namespace kestrel;
using kestrel::testing::D;

std::string Env(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

class QuoteStoreTest : public ::testing::Test {
   protected:
    void SetUp() override {
        if (Env("KESTREL_TEST_DBNAME").empty()) {
            GTEST_SKIP() << "KESTREL_TEST_DBNAME not set";
        }
        db::ConnectionOptions options;
        options.dbname = Env("KESTREL_TEST_DBNAME");
        options.host = Env("KESTREL_TEST_DBHOST");
        options.user = Env("KESTREL_TEST_DBUSER");
        options.password = Env("KESTREL_TEST_DBPASSWORD");
        if (const auto port = Env("KESTREL_TEST_DBPORT"); !port.empty()) {
            options.port = std::stoi(port);
        }
        auto database = db::Database::Connect(options);
        ASSERT_OK(database);
        mDatabase = std::move(*database);
        ASSERT_OK(mDatabase->Migrate());
        ASSERT_OK(
            mDatabase->Query("DELETE FROM quotes WHERE venue = $1", {kVenue}));
    }
    void TearDown() override {
        if (mDatabase) {
            (void)mDatabase->Query("DELETE FROM quotes WHERE venue = $1",
                                   {kVenue});
        }
    }

    static types::Quote MakeQuote(std::int64_t ms) {
        types::Quote q;
        q.seq = ms + 1;
        q.symbol = "BTC/USD";
        q.venue = kVenue;
        q.received_time = types::FromEpochMillis(1'700'000'000'000 + ms);
        q.bid = D("100.10");
        q.bid_size = D("1.5");
        q.ask = D("100.20");
        q.ask_size = D("2");
        return q;  // no last / exchange_time: stored as NULL
    }

    static constexpr const char* kVenue = "kestrel-test";
    std::shared_ptr<db::Database> mDatabase;
};

TEST_F(QuoteStoreTest, RoundTripsQuotesIncludingMissingFields) {
    db::QuoteStore store(mDatabase);
    auto full = MakeQuote(1);
    full.exchange_time = types::FromEpochMillis(1'700'000'000'000);
    full.last = D("100.15");
    full.last_size = D("0.25");
    full.last_taker_side = types::Side::kSell;
    const std::vector<types::Quote> quotes = {MakeQuote(0), full};
    auto saved = store.Save(quotes);
    ASSERT_OK(saved);
    EXPECT_EQ(*saved, 2U);

    auto loaded = store.Load(kVenue, "BTC/USD", quotes[0].received_time,
                             full.received_time + std::chrono::milliseconds(1));
    ASSERT_OK(loaded);
    ASSERT_EQ(loaded->size(), 2U);
    EXPECT_EQ((*loaded)[0].bid, D("100.10"));
    EXPECT_FALSE((*loaded)[0].last.has_value());
    EXPECT_FALSE((*loaded)[0].exchange_time.has_value());
    EXPECT_EQ((*loaded)[1].last, D("100.15"));
    EXPECT_EQ((*loaded)[1].last_size, D("0.25"));
    EXPECT_EQ((*loaded)[1].exchange_time, full.exchange_time);
    EXPECT_FALSE((*loaded)[0].last_taker_side.has_value());
    EXPECT_EQ((*loaded)[1].last_taker_side, types::Side::kSell);
}

// A flush that failed after some of its batches went in is retried whole.
TEST_F(QuoteStoreTest, SavingQuotesAgainSkipsTheStoredOnes) {
    db::QuoteStore store(mDatabase);
    std::vector<types::Quote> first;
    for (int i = 0; i < 3; ++i) {
        first.push_back(MakeQuote(i));
    }
    auto saved = store.Save(first);
    ASSERT_OK(saved);
    EXPECT_EQ(*saved, 3U);

    auto again = first;
    again.push_back(MakeQuote(3));
    auto resaved = store.Save(again);
    ASSERT_OK(resaved);
    EXPECT_EQ(*resaved, 1U);  // only the new one
    auto loaded = store.Load(kVenue, "BTC/USD", MakeQuote(0).received_time,
                             MakeQuote(10).received_time);
    ASSERT_OK(loaded);
    EXPECT_EQ(loaded->size(), 4U);
}

TEST_F(QuoteStoreTest, WriterFlushesBufferedQuotesOnDestruction) {
    {
        db::QuoteWriter writer(mDatabase, std::chrono::hours(1));
        auto sink = writer.MakeStreamSink();
        for (int i = 0; i < 25; ++i) {
            sink(MakeQuote(i));
        }
    }
    db::QuoteStore store(mDatabase);
    auto loaded = store.Load(kVenue, "BTC/USD", MakeQuote(0).received_time,
                             MakeQuote(100).received_time);
    ASSERT_OK(loaded);
    EXPECT_EQ(loaded->size(), 25U);
}

TEST_F(QuoteStoreTest, WriterNumbersSameMillisecondQuotesAcrossRestarts) {
    // Every quote shares one received_time; only seq can order them.
    auto quote = [](const char* symbol, const std::string& bid) {
        auto q = MakeQuote(0);
        q.symbol = symbol;
        q.bid = D(bid.c_str());
        return q;
    };
    for (int run = 0; run < 2; ++run) {
        db::QuoteWriter writer(mDatabase, std::chrono::hours(1));
        for (int i = 0; i < 3; ++i) {
            writer.Push(quote("BTC/USD", std::to_string(run * 3 + i)));
            writer.Push(quote("ETH/USD", "7"));
        }
    }
    db::QuoteStore store(mDatabase);
    const auto start = MakeQuote(0).received_time;
    const auto end = MakeQuote(1).received_time;
    auto btc = store.Load(kVenue, "BTC/USD", start, end);
    ASSERT_OK(btc);
    ASSERT_EQ(btc->size(), 6U);
    for (std::size_t i = 0; i < btc->size(); ++i) {
        EXPECT_EQ((*btc)[i].seq, static_cast<std::int64_t>(i + 1));
        EXPECT_EQ((*btc)[i].bid, D(std::to_string(i).c_str()));
    }
    auto eth = store.Load(kVenue, "ETH/USD", start, end);
    ASSERT_OK(eth);
    ASSERT_EQ(eth->size(), 6U);
    EXPECT_EQ(eth->back().seq, 6);
}

TEST_F(QuoteStoreTest, WriterFlushesPeriodically) {
    db::QuoteWriter writer(mDatabase, std::chrono::milliseconds(20));
    writer.Push(MakeQuote(0));
    db::QuoteStore store(mDatabase);
    for (int i = 0; i < 100; ++i) {
        auto loaded = store.Load(kVenue, "BTC/USD", MakeQuote(0).received_time,
                                 MakeQuote(1).received_time);
        ASSERT_OK(loaded);
        if (loaded->size() == 1U) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    FAIL() << "quote was never flushed";
}

TEST_F(QuoteStoreTest, QuotesTableIsAHypertable) {
    auto rows = mDatabase->Query(
        "SELECT 1 FROM timescaledb_information.hypertables WHERE "
        "hypertable_name = 'quotes'");
    ASSERT_OK(rows);
    EXPECT_EQ(rows->size(), 1U);
}

TEST_F(QuoteStoreTest, QuotesKeepOnlyTheIndexesTheirQueriesUse) {
    auto rows = mDatabase->Query(
        "SELECT indexname FROM pg_indexes WHERE schemaname = current_schema() "
        "AND tablename = 'quotes' ORDER BY indexname");
    ASSERT_OK(rows);
    std::vector<std::string> names;
    for (const auto& row : *rows) {
        names.push_back(row.front().value_or(""));
    }
    EXPECT_EQ(names, (std::vector<std::string>{"quotes_seq_unique",
                                               "quotes_venue_symbol_seq"}));
}

TEST_F(QuoteStoreTest, LastSeqFindsTheNewestAcrossChunksAndClockSkew) {
    db::QuoteStore store(mDatabase);
    auto empty = store.LastSeq(kVenue, "BTC/USD");
    ASSERT_OK(empty);
    EXPECT_EQ(*empty, 0);

    // An older chunk (3 days earlier) and the newest chunk, where a small
    // backward clock step gave seq 5 an earlier received_time than seq 4.
    auto old_chunk = MakeQuote(0);
    old_chunk.seq = 3;
    old_chunk.received_time -= std::chrono::hours(72);
    auto q4 = MakeQuote(5000);
    q4.seq = 4;
    auto q5 = MakeQuote(0);  // 5 s earlier than q4, higher seq
    q5.seq = 5;
    ASSERT_OK(store.Save(std::vector<types::Quote>{old_chunk, q4, q5}));

    auto last = store.LastSeq(kVenue, "BTC/USD");
    ASSERT_OK(last);
    EXPECT_EQ(*last, 5);
}

class CompressedQuoteStoreTest : public QuoteStoreTest {
   protected:
    void SetUp() override {
        QuoteStoreTest::SetUp();
        if (IsSkipped() || HasFatalFailure()) {
            return;
        }
        auto license = mDatabase->Query(
            "SELECT current_setting('timescaledb.license', true)");
        ASSERT_OK(license);
        if (license->empty() || license->front().empty() ||
            license->front().front() != "timescale") {
            GTEST_SKIP() << "TimescaleDB's Apache build has no compression";
        }
        auto ver = mDatabase->Query(
            "SELECT split_part(extversion, '.', 1)::int * 100 + "
            "NULLIF(regexp_replace(split_part(extversion, '.', 2), "
            "'[^0-9].*$', ''), '')::int "
            "FROM pg_extension WHERE extname = 'timescaledb'");
        ASSERT_OK(ver);
        if (ver->empty() || ver->front().empty() || !ver->front().front() ||
            std::stoi(*ver->front().front()) < 211) {
            GTEST_SKIP()
                << "TimescaleDB < 2.11 cannot upsert compressed chunks";
        }
        ASSERT_OK(
            mDatabase->Query("DELETE FROM bars WHERE venue = $1", {kVenue}));
        DecompressTestChunks();
    }

    void TearDown() override {
        if (mDatabase) {
            (void)mDatabase->Query("DELETE FROM bars WHERE venue = $1",
                                   {kVenue});
            if (!IsSkipped()) {
                DecompressTestChunks();
            }
        }
        QuoteStoreTest::TearDown();
    }

    // Compresses the chunk holding the test's data (2023-11-14T22:13:20Z).
    void CompressTestChunk(const char* table) {
        auto compressed = mDatabase->Query(
            "SELECT compress_chunk(format('%I.%I', chunk_schema, chunk_name)"
            "::regclass, if_not_compressed => true) "
            "FROM timescaledb_information.chunks "
            "WHERE hypertable_schema = current_schema() "
            "AND hypertable_name = $1 "
            "AND range_start <= TIMESTAMPTZ '2023-11-14T22:13:20Z' "
            "AND range_end > TIMESTAMPTZ '2023-11-14T22:13:20Z'",
            {table});
        ASSERT_OK(compressed);
        ASSERT_EQ(compressed->size(), 1U) << table;
    }

    void DecompressTestChunks() {
        (void)mDatabase->Query(
            "SELECT decompress_chunk(format('%I.%I', chunk_schema, chunk_name)"
            "::regclass, if_compressed => true) "
            "FROM timescaledb_information.chunks "
            "WHERE hypertable_schema = current_schema() "
            "AND hypertable_name IN ('quotes', 'bars') "
            "AND range_start <= TIMESTAMPTZ '2023-11-14T22:13:20Z' "
            "AND range_end > TIMESTAMPTZ '2023-11-14T22:13:20Z'");
    }
};

TEST_F(CompressedQuoteStoreTest, CompressionIsOnForTheHypertables) {
    for (const char* table : {"quotes", "market_trades", "bars"}) {
        auto rows = mDatabase->Query(
            "SELECT h.compression_enabled, count(j.job_id) FROM "
            "timescaledb_information.hypertables h LEFT JOIN "
            "timescaledb_information.jobs j ON j.hypertable_schema = "
            "h.hypertable_schema AND j.hypertable_name = "
            "h.hypertable_name AND j.proc_name = 'policy_compression' WHERE "
            "h.hypertable_schema = current_schema() AND h.hypertable_name = $1 "
            "GROUP BY h.compression_enabled",
            {table});
        ASSERT_OK(rows);
        ASSERT_EQ(rows->size(), 1U) << table;
        EXPECT_EQ((*rows)[0][0], "t") << table;
        EXPECT_EQ((*rows)[0][1], "1") << table;
    }
}

TEST_F(CompressedQuoteStoreTest, CompressedQuotesLoadAndSkipRepeats) {
    db::QuoteStore store(mDatabase);
    std::vector<types::Quote> first;
    for (int i = 0; i < 3; ++i) {
        first.push_back(MakeQuote(i));
    }
    ASSERT_OK(store.Save(first));
    CompressTestChunk("quotes");
    if (HasFatalFailure()) {
        return;
    }

    auto again = first;
    again.push_back(MakeQuote(3));
    auto resaved = store.Save(again);
    ASSERT_OK(resaved);
    EXPECT_EQ(*resaved, 1U);  // only the new one, into a compressed chunk
    auto last = store.LastSeq(kVenue, "BTC/USD");
    ASSERT_OK(last);
    EXPECT_EQ(*last, MakeQuote(3).seq);
    auto loaded = store.Load(kVenue, "BTC/USD", MakeQuote(0).received_time,
                             MakeQuote(10).received_time);
    ASSERT_OK(loaded);
    ASSERT_EQ(loaded->size(), 4U);
    EXPECT_EQ((*loaded)[0].bid, D("100.10"));
    EXPECT_EQ((*loaded)[3].seq, MakeQuote(3).seq);
}

TEST_F(CompressedQuoteStoreTest, CompressedBarsUpsertAndLoad) {
    db::BarStore store(mDatabase);
    const auto t0 = MakeQuote(0).received_time;
    const auto t1 = t0 + std::chrono::minutes(1);
    const types::Bar first{t0, D("100"), D("105"), D("99"), D("102"), D("10")};
    ASSERT_OK(store.Save(kVenue, "BTC/USD", types::Timeframe::kM1,
                         std::span{&first, 1}));
    CompressTestChunk("bars");
    if (HasFatalFailure()) {
        return;
    }

    const std::vector<types::Bar> updated = {
        {t0, D("100"), D("106"), D("99"), D("104"), D("12")},
        {t1, D("104"), D("107"), D("103"), D("105"), D("8")},
    };
    auto saved = store.Save(kVenue, "BTC/USD", types::Timeframe::kM1, updated);
    ASSERT_OK(saved);
    EXPECT_EQ(*saved, 2U);
    auto loaded = store.Load(kVenue, "BTC/USD", types::Timeframe::kM1, t0,
                             t1 + std::chrono::minutes(1));
    ASSERT_OK(loaded);
    ASSERT_EQ(loaded->size(), 2U);
    EXPECT_EQ((*loaded)[0].close, D("104"));
    EXPECT_EQ((*loaded)[0].volume, D("12"));
    EXPECT_EQ((*loaded)[1].close, D("105"));
}

}  // namespace
