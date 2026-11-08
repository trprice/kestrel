// Integration tests; run only when KESTREL_TEST_DBNAME is set (see
// bar_store_test.cpp).

#include "kestrel/db/strategy_state_store.hpp"

#include "db_fixture.hpp"

namespace {

using namespace kestrel;

// Only keys under these scopes are touched.
constexpr const char* kScope = "state-test/paper/";
constexpr const char* kOtherScope = "state-test/live/";

class StrategyStateStoreTest : public ::testing::Test {
   protected:
    void SetUp() override {
        if (!kestrel::testing::DatabaseConfigured()) {
            GTEST_SKIP() << "KESTREL_TEST_DBNAME not set";
        }
        auto database = kestrel::testing::ConnectTestDatabase();
        ASSERT_OK(database);
        mDatabase = *database;
        Clean();
    }
    void TearDown() override {
        if (mDatabase) {
            Clean();
        }
    }
    void Clean() {
        (void)mDatabase->Query(
            "DELETE FROM strategy_state WHERE key LIKE 'state-test/%'");
    }

    std::shared_ptr<db::Database> mDatabase;
};

TEST_F(StrategyStateStoreTest, SavesReplacesLoadsAndErases) {
    db::StrategyStateStore store(mDatabase, kScope);
    auto none = store.Load("market_maker/ZEC/USDT");
    ASSERT_OK(none);
    EXPECT_FALSE(*none);

    ASSERT_OK(store.Save("market_maker/ZEC/USDT", R"({"halted":false})"));
    ASSERT_OK(store.Save("market_maker/ZEC/USDT", R"({"halted":true})"));
    auto loaded = store.Load("market_maker/ZEC/USDT");
    ASSERT_OK(loaded);
    ASSERT_TRUE(*loaded);
    EXPECT_EQ(**loaded, R"({"halted":true})");  // the newest, once

    ASSERT_OK(store.Erase("market_maker/ZEC/USDT"));
    auto erased = store.Load("market_maker/ZEC/USDT");
    ASSERT_OK(erased);
    EXPECT_FALSE(*erased);
    ASSERT_OK(store.Erase("market_maker/ZEC/USDT"));  // nothing to erase
}

TEST_F(StrategyStateStoreTest, ScopesKeepVenuesAndModesApart) {
    db::StrategyStateStore paper(mDatabase, kScope);
    db::StrategyStateStore live(mDatabase, kOtherScope);
    ASSERT_OK(paper.Save("market_maker/ZEC/USDT", "paper"));
    auto loaded = live.Load("market_maker/ZEC/USDT");
    ASSERT_OK(loaded);
    EXPECT_FALSE(*loaded);
}

}  // namespace
