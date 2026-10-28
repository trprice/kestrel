// Integration tests; run only when KESTREL_TEST_DBNAME is set (see
// bar_store_test.cpp).

#include "kestrel/db/account_fill_store.hpp"

#include "db_fixture.hpp"
#include "strategy_fakes.hpp"

namespace {

using namespace kestrel;
using kestrel::testing::D;
using kestrel::testing::RecordingVenue;

// RecordingVenue::Name(); the venues these tests write under.
constexpr const char* kVenue = "recording";
constexpr const char* kOtherVenue = "account-fill-test-other";

types::Timestamp At(int seconds) {
    return types::FromEpochSeconds(1'700'000'000 + seconds);
}

types::Fill FillOf(const char* id, int seconds,
                   const char* symbol = "ZEC/USDT") {
    types::Fill f;
    f.fill_id = id;
    f.order_id = "o-" + std::string(id);
    f.client_order_id = "c-" + std::string(id);
    f.symbol = symbol;
    f.side = types::Side::kBuy;
    f.quantity = D("0.015");
    f.price = D("1236.4");
    f.fee = D("0");
    f.fee_asset = "BNB";
    f.liquidity = types::Liquidity::kMaker;
    f.time = At(seconds);
    return f;
}

class AccountFillStoreTest : public ::testing::Test {
   protected:
    void SetUp() override {
        if (!kestrel::testing::DatabaseConfigured()) {
            GTEST_SKIP() << "KESTREL_TEST_DBNAME not set";
        }
        auto database = kestrel::testing::ConnectTestDatabase();
        ASSERT_OK(database);
        mDatabase = *database;
        Clean();
        mStore = std::make_unique<db::AccountFillStore>(mDatabase);
    }
    void TearDown() override {
        if (mDatabase) {
            Clean();
        }
    }
    void Clean() {
        (void)mDatabase->Query(
            "DELETE FROM account_fills WHERE venue = $1 OR venue = $2",
            {kVenue, kOtherVenue});
    }

    std::shared_ptr<db::Database> mDatabase;
    std::unique_ptr<db::AccountFillStore> mStore;
};

TEST_F(AccountFillStoreTest, FillsRoundTripOnceEach) {
    types::Fill taker = FillOf("b", 20);
    taker.side = types::Side::kSell;
    taker.fee = D("0.00297430");
    taker.fee_asset = "USDT";
    taker.liquidity = types::Liquidity::kTaker;
    const std::vector<types::Fill> fills = {FillOf("a", 10), taker};
    auto stored = mStore->Save(kVenue, fills);
    ASSERT_OK(stored);
    EXPECT_EQ(*stored, 2u);
    auto again = mStore->Save(kVenue, fills);
    ASSERT_OK(again);
    EXPECT_EQ(*again, 0u);  // already there

    auto loaded = mStore->Load(kVenue, At(0), At(100));
    ASSERT_OK(loaded);
    ASSERT_EQ(loaded->size(), 2u);
    const auto& a = (*loaded)[0];
    EXPECT_EQ(a.fill_id, "a");
    EXPECT_EQ(a.order_id, "o-a");
    EXPECT_EQ(a.client_order_id, "c-a");
    EXPECT_EQ(a.quantity, D("0.015"));
    EXPECT_EQ(a.price, D("1236.4"));
    EXPECT_EQ(a.fee_asset, "BNB");
    EXPECT_EQ(a.liquidity, types::Liquidity::kMaker);
    EXPECT_EQ(a.time, At(10));
    const auto& b = (*loaded)[1];
    EXPECT_EQ(b.side, types::Side::kSell);
    EXPECT_EQ(b.fee, D("0.00297430"));
    EXPECT_EQ(b.liquidity, types::Liquidity::kTaker);
}

TEST_F(AccountFillStoreTest, LoadFiltersByTimeSymbolAndVenue) {
    const std::vector<types::Fill> fills = {
        FillOf("a", 10), FillOf("b", 20, "BTC/USDT"), FillOf("c", 30)};
    ASSERT_OK(mStore->Save(kVenue, fills));
    ASSERT_OK(mStore->Save(kOtherVenue, fills));

    auto window = mStore->Load(kVenue, At(10), At(30));  // end exclusive
    ASSERT_OK(window);
    ASSERT_EQ(window->size(), 2u);
    EXPECT_EQ((*window)[1].fill_id, "b");

    auto zec = mStore->Load(kVenue, At(0), At(100), "ZEC/USDT");
    ASSERT_OK(zec);
    ASSERT_EQ(zec->size(), 2u);
    EXPECT_EQ((*zec)[1].fill_id, "c");
}

TEST_F(AccountFillStoreTest, NewestIsPerVenueAndSymbol) {
    auto none = mStore->Newest(kVenue, "ZEC/USDT");
    ASSERT_OK(none);
    EXPECT_FALSE(none->has_value());

    const std::vector<types::Fill> fills = {FillOf("a", 10), FillOf("c", 30),
                                            FillOf("b", 50, "BTC/USDT")};
    ASSERT_OK(mStore->Save(kVenue, fills));
    EXPECT_EQ(mStore->Newest(kVenue, "ZEC/USDT").value(), At(30));
    EXPECT_EQ(mStore->Newest(kVenue, "BTC/USDT").value(), At(50));
    EXPECT_FALSE(mStore->Newest(kOtherVenue, "ZEC/USDT")->has_value());
}

TEST_F(AccountFillStoreTest, SyncStartsBackThenCarriesOnFromTheNewest) {
    RecordingVenue venue;
    venue.fills = {FillOf("old", -100), FillOf("a", 10), FillOf("b", 20)};

    auto first = db::SyncAccountFills(venue, *mStore, {"ZEC/USDT"}, At(0));
    ASSERT_OK(first);
    EXPECT_EQ(first->since, At(0));  // nothing stored: first_since
    EXPECT_EQ(first->fetched, 2u);   // "old" is before it
    EXPECT_EQ(first->stored, 2u);

    venue.fills.push_back(FillOf("c", 30));
    auto second = db::SyncAccountFills(venue, *mStore, {"ZEC/USDT"}, At(0));
    ASSERT_OK(second);
    EXPECT_EQ(second->since, At(20));  // the newest stored
    EXPECT_EQ(second->fetched, 2u);    // b again, and c
    EXPECT_EQ(second->stored, 1u);
    EXPECT_EQ(mStore->Load(kVenue, At(-1000), At(1000))->size(), 3u);
}

TEST_F(AccountFillStoreTest, ASymbolNotSyncedBeforeStartsBackFromFirstSince) {
    RecordingVenue venue;
    venue.fills = {FillOf("a", 10)};
    ASSERT_OK(db::SyncAccountFills(venue, *mStore, {"ZEC/USDT"}, At(0)));

    venue.fills.push_back(FillOf("btc", 5, "BTC/USDT"));
    auto sync =
        db::SyncAccountFills(venue, *mStore, {"ZEC/USDT", "BTC/USDT"}, At(0));
    ASSERT_OK(sync);
    EXPECT_EQ(sync->since, At(0));  // BTC/USDT has nothing stored yet
    EXPECT_EQ(sync->stored, 1u);
}

TEST_F(AccountFillStoreTest, SyncReportsAVenueFailureAndStoresNothing) {
    RecordingVenue venue;
    venue.fills = {FillOf("a", 10)};
    venue.fail_fills = true;
    auto sync = db::SyncAccountFills(venue, *mStore, {"ZEC/USDT"}, At(0));
    ASSERT_FALSE(sync.has_value());
    EXPECT_EQ(sync.error().category, types::ErrorCategory::kNetwork);
    EXPECT_TRUE(mStore->Load(kVenue, At(-1000), At(1000))->empty());
}

TEST_F(AccountFillStoreTest, SyncStartIsTheOldestNewestOrFirstSince) {
    const std::vector<types::Fill> fills = {FillOf("a", 30),
                                            FillOf("b", 50, "BTC/USDT")};
    ASSERT_OK(mStore->Save(kVenue, fills));
    EXPECT_EQ(db::SyncStart(*mStore, kVenue, {"ZEC/USDT"}, At(0)).value(),
              At(30));
    EXPECT_EQ(
        db::SyncStart(*mStore, kVenue, {"ZEC/USDT", "BTC/USDT"}, At(0)).value(),
        At(30));
    EXPECT_EQ(
        db::SyncStart(*mStore, kVenue, {"ZEC/USDT", "ETH/USDT"}, At(0)).value(),
        At(0));  // ETH/USDT has nothing stored
}

TEST_F(AccountFillStoreTest, ARowWithAnUnknownSideOrLiquidityIsAParseError) {
    for (const char* column : {"side", "liquidity"}) {
        Clean();
        ASSERT_OK(
            mStore->Save(kVenue, std::vector<types::Fill>{FillOf("a", 10)}));
        ASSERT_OK(mDatabase->Query(std::string("UPDATE account_fills SET ") +
                                       column +
                                       " = 'sideways' WHERE venue = $1",
                                   {kVenue}));
        auto loaded = mStore->Load(kVenue, At(0), At(100));
        ASSERT_FALSE(loaded.has_value()) << column;
        EXPECT_EQ(loaded.error().category, types::ErrorCategory::kParse);
        EXPECT_NE(loaded.error().message.find("sideways"), std::string::npos);
    }
}

}  // namespace
