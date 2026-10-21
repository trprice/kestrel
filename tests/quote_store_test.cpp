// Integration tests; run only when KESTREL_TEST_DBNAME is set (see
// bar_store_test.cpp).

#include "kestrel/db/quote_store.hpp"

#include <cstdlib>

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
        ASSERT_OK(mDatabase->Query("DELETE FROM quotes WHERE venue = $1",
                                   {kVenue}));
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

}  // namespace
