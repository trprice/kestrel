// Integration tests against a real PostgreSQL + TimescaleDB. They run only
// when KESTREL_TEST_DBNAME names a database (host/user/password come from the
// usual PG* variables or KESTREL_TEST_DB{HOST,PORT,USER,PASSWORD}); otherwise
// they skip.

#include "kestrel/db/bar_store.hpp"

#include <cstdlib>

#include "test_helpers.hpp"

namespace {

using namespace kestrel;
using kestrel::testing::D;

std::string Env(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

class BarStoreTest : public ::testing::Test {
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
        ASSERT_OK(mDatabase->Migrate());  // idempotent
        mStore = std::make_unique<db::BarStore>(mDatabase);
        ASSERT_OK(mDatabase->Query("DELETE FROM bars WHERE venue = $1",
                                   {kVenue}));
    }
    void TearDown() override {
        if (mDatabase) {
            (void)mDatabase->Query("DELETE FROM bars WHERE venue = $1",
                                   {kVenue});
        }
    }

    static types::Bar MakeBar(std::int64_t minute, const char* close) {
        types::Bar b;
        b.open_time = types::FromEpochMillis(1'700'000'000'000 + minute * 60'000);
        b.open = D("100.10");
        b.high = D("110");
        b.low = D("90.123456789");
        b.close = D(close);
        b.volume = D("12.5");
        return b;
    }

    static constexpr const char* kVenue = "kestrel-test";
    std::shared_ptr<db::Database> mDatabase;
    std::unique_ptr<db::BarStore> mStore;
};

TEST_F(BarStoreTest, RoundTripsBarsExactly) {
    const std::vector<types::Bar> bars = {MakeBar(0, "105.5"),
                                          MakeBar(1, "106")};
    auto saved = mStore->Save(kVenue, "BTC/USD", types::Timeframe::kM1, bars);
    ASSERT_OK(saved);
    EXPECT_EQ(*saved, 2U);

    auto loaded = mStore->Load(kVenue, "BTC/USD", types::Timeframe::kM1,
                               bars[0].open_time,
                               bars[1].open_time + std::chrono::milliseconds(1));
    ASSERT_OK(loaded);
    ASSERT_EQ(loaded->size(), 2U);
    EXPECT_EQ((*loaded)[0].open_time, bars[0].open_time);
    EXPECT_EQ((*loaded)[0].low, D("90.123456789"));
    EXPECT_EQ((*loaded)[1].close, D("106"));
}

TEST_F(BarStoreTest, SavingAgainReplacesTheBar) {
    ASSERT_OK(mStore->Save(kVenue, "BTC/USD", types::Timeframe::kM1,
                           std::vector{MakeBar(0, "105")}));
    ASSERT_OK(mStore->Save(kVenue, "BTC/USD", types::Timeframe::kM1,
                           std::vector{MakeBar(0, "107")}));
    auto loaded = mStore->Load(kVenue, "BTC/USD", types::Timeframe::kM1,
                               MakeBar(0, "1").open_time,
                               MakeBar(1, "1").open_time);
    ASSERT_OK(loaded);
    ASSERT_EQ(loaded->size(), 1U);
    EXPECT_EQ((*loaded)[0].close, D("107"));
}

TEST_F(BarStoreTest, TimeframesAndSymbolsAreSeparate) {
    ASSERT_OK(mStore->Save(kVenue, "BTC/USD", types::Timeframe::kM1,
                           std::vector{MakeBar(0, "105")}));
    auto other_tf = mStore->Load(kVenue, "BTC/USD", types::Timeframe::kM5,
                                 MakeBar(0, "1").open_time,
                                 MakeBar(9, "1").open_time);
    ASSERT_OK(other_tf);
    EXPECT_TRUE(other_tf->empty());
    auto other_symbol = mStore->Load(kVenue, "ETH/USD", types::Timeframe::kM1,
                                     MakeBar(0, "1").open_time,
                                     MakeBar(9, "1").open_time);
    ASSERT_OK(other_symbol);
    EXPECT_TRUE(other_symbol->empty());
}

TEST_F(BarStoreTest, StreamSinkStoresBars) {
    auto sink = mStore->MakeStreamSink(kVenue);
    sink("ETH/USD", types::Timeframe::kM1, MakeBar(0, "105"));
    auto loaded = mStore->Load(kVenue, "ETH/USD", types::Timeframe::kM1,
                               MakeBar(0, "1").open_time,
                               MakeBar(1, "1").open_time);
    ASSERT_OK(loaded);
    EXPECT_EQ(loaded->size(), 1U);
}

TEST_F(BarStoreTest, BarsTableIsAHypertable) {
    auto rows = mDatabase->Query(
        "SELECT hypertable_name FROM timescaledb_information.hypertables "
        "WHERE hypertable_name = 'bars'");
    ASSERT_OK(rows);
    EXPECT_EQ(rows->size(), 1U);
}

}  // namespace
