// Integration tests; run only when KESTREL_TEST_DBNAME is set (see
// bar_store_test.cpp).

#include "kestrel/db/market_trade_store.hpp"

#include "db_fixture.hpp"

namespace {

using namespace kestrel;
using kestrel::testing::D;

// Only rows under these venues are touched.
constexpr const char* kVenue = "market-trade-test";
constexpr const char* kOtherVenue = "market-trade-test-other";

types::Timestamp At(int seconds) {
    return types::FromEpochSeconds(1'700'000'000 + seconds);
}

types::MarketTrade TradeOf(std::int64_t id, int seconds,
                           types::Side taker = types::Side::kBuy) {
    types::MarketTrade t;
    t.trade_id = id;
    t.time = At(seconds);
    t.price = D("1240.51");
    t.quantity = D("0.004");
    t.taker_side = taker;
    return t;
}

class MarketTradeStoreTest : public ::testing::Test {
   protected:
    void SetUp() override {
        if (!kestrel::testing::DatabaseConfigured()) {
            GTEST_SKIP() << "KESTREL_TEST_DBNAME not set";
        }
        auto database = kestrel::testing::ConnectTestDatabase();
        ASSERT_OK(database);
        mDatabase = *database;
        Clean();
        mStore = std::make_unique<db::MarketTradeStore>(mDatabase);
    }
    void TearDown() override {
        if (mDatabase) {
            Clean();
        }
    }
    void Clean() {
        (void)mDatabase->Query(
            "DELETE FROM market_trades WHERE venue = $1 OR venue = $2",
            {kVenue, kOtherVenue});
    }

    std::shared_ptr<db::Database> mDatabase;
    std::unique_ptr<db::MarketTradeStore> mStore;
};

TEST_F(MarketTradeStoreTest, TradesRoundTripOnceEachInTimeOrder) {
    // Out of order, and two in the same second.
    const std::vector<types::MarketTrade> trades = {
        TradeOf(12, 20, types::Side::kSell), TradeOf(10, 10), TradeOf(11, 10)};
    auto stored = mStore->Save(kVenue, "ZEC/USDT", trades);
    ASSERT_OK(stored);
    EXPECT_EQ(*stored, 3u);
    auto again = mStore->Save(kVenue, "ZEC/USDT", trades);
    ASSERT_OK(again);
    EXPECT_EQ(*again, 0u);  // already there

    auto loaded = mStore->Load(kVenue, "ZEC/USDT", At(0), At(100));
    ASSERT_OK(loaded);
    ASSERT_EQ(loaded->size(), 3u);
    EXPECT_EQ((*loaded)[0].trade_id, 10);
    EXPECT_EQ((*loaded)[1].trade_id, 11);
    EXPECT_EQ((*loaded)[2].trade_id, 12);
    EXPECT_EQ((*loaded)[2].time, At(20));
    EXPECT_EQ((*loaded)[2].price, D("1240.51"));
    EXPECT_EQ((*loaded)[2].quantity, D("0.004"));
    EXPECT_EQ((*loaded)[2].taker_side, types::Side::kSell);
    EXPECT_EQ((*loaded)[0].taker_side, types::Side::kBuy);
}

TEST_F(MarketTradeStoreTest, LoadAndNewestKeepToTheRangeSymbolAndVenue) {
    ASSERT_OK(mStore->Save(kVenue, "ZEC/USDT",
                           std::vector{TradeOf(1, 10), TradeOf(2, 50)}));
    ASSERT_OK(mStore->Save(kVenue, "BTC/USDT", std::vector{TradeOf(3, 30)}));
    ASSERT_OK(mStore->Save(kOtherVenue, "ZEC/USDT", std::vector{TradeOf(4, 30)}));

    auto loaded = mStore->Load(kVenue, "ZEC/USDT", At(10), At(50));
    ASSERT_OK(loaded);
    ASSERT_EQ(loaded->size(), 1u);  // [from, to): 50 is out
    EXPECT_EQ((*loaded)[0].trade_id, 1);

    auto newest = mStore->Newest(kVenue, "ZEC/USDT", At(0), At(100));
    ASSERT_OK(newest);
    EXPECT_EQ(*newest, At(50));
    auto in_range = mStore->Newest(kVenue, "ZEC/USDT", At(0), At(40));
    ASSERT_OK(in_range);
    EXPECT_EQ(*in_range, At(10));
    auto none = mStore->Newest(kVenue, "ZEC/USDT", At(60), At(100));
    ASSERT_OK(none);
    EXPECT_FALSE(none->has_value());
}

}  // namespace
