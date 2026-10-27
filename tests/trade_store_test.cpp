// Integration tests; run only when KESTREL_TEST_DBNAME is set (see
// bar_store_test.cpp).

#include "kestrel/db/trade_store.hpp"

#include "db_fixture.hpp"
#include "kestrel/config/configuration.hpp"
#include "kestrel/factory.hpp"

namespace {

using namespace kestrel;
using kestrel::OpenTradeLog;
using kestrel::testing::D;

constexpr const char* kStrategy = "trade-store-test";

class TradeStoreTest : public ::testing::Test {
   protected:
    void SetUp() override {
        if (!kestrel::testing::DatabaseConfigured()) {
            GTEST_SKIP() << "KESTREL_TEST_DBNAME not set";
        }
        auto database = kestrel::testing::ConnectTestDatabase();
        ASSERT_OK(database);
        mDatabase = *database;
        Clean();
        auto store = db::TradeStore::StartRun(
            mDatabase, {kStrategy, "paper-venue", "paper",
                        types::FromEpochSeconds(1'700'000'000)});
        ASSERT_OK(store);
        mStore = std::move(*store);
    }
    void TearDown() override {
        mStore.reset();
        if (mDatabase) {
            Clean();
        }
    }

    // Everything these tests wrote, children first.
    void Clean() {
        for (const char* table : {"fills", "pnl_snapshots"}) {
            (void)mDatabase->Query(
                std::string("DELETE FROM ") + table +
                    " WHERE run_id IN (SELECT run_id FROM runs WHERE "
                    "strategy = $1)",
                {kStrategy});
        }
        (void)mDatabase->Query("DELETE FROM runs WHERE strategy = $1",
                               {kStrategy});
    }

    static types::Fill FillOf(const char* id, int seconds) {
        types::Fill f;
        f.fill_id = id;
        f.order_id = "o-" + std::string(id);
        f.symbol = "ZEC/USDT";
        f.side = types::Side::kSell;
        f.quantity = D("0.043");
        f.price = D("1144.16");
        f.fee = D("0.0098");
        f.fee_asset = "USDT";
        f.liquidity = types::Liquidity::kMaker;
        f.time = types::FromEpochSeconds(1'700'000'000 + seconds);
        return f;
    }

    std::shared_ptr<db::Database> mDatabase;
    std::unique_ptr<db::TradeStore> mStore;
};

TEST_F(TradeStoreTest, FillsRoundTrip) {
    const std::vector<types::Fill> fills = {FillOf("b", 20), FillOf("a", 10)};
    ASSERT_OK(mStore->WriteFills(fills));
    auto loaded = mStore->LoadFills();
    ASSERT_OK(loaded);
    ASSERT_EQ(loaded->size(), 2u);
    EXPECT_EQ((*loaded)[0].fill_id, "a");  // oldest first
    EXPECT_EQ((*loaded)[1].fill_id, "b");
    const auto& f = (*loaded)[0];
    EXPECT_EQ(f.order_id, "o-a");
    EXPECT_EQ(f.symbol, "ZEC/USDT");
    EXPECT_EQ(f.side, types::Side::kSell);
    EXPECT_EQ(f.quantity, D("0.043"));
    EXPECT_EQ(f.price, D("1144.16"));
    EXPECT_EQ(f.fee, D("0.0098"));
    EXPECT_EQ(f.fee_asset, "USDT");
    EXPECT_EQ(f.liquidity, types::Liquidity::kMaker);
    EXPECT_EQ(f.time, types::FromEpochSeconds(1'700'000'010));
}

TEST_F(TradeStoreTest, WritingAFillTwiceStoresItOnce) {
    const std::vector<types::Fill> fills = {FillOf("a", 10)};
    ASSERT_OK(mStore->WriteFills(fills));
    ASSERT_OK(mStore->WriteFills(fills));
    auto loaded = mStore->LoadFills();
    ASSERT_OK(loaded);
    EXPECT_EQ(loaded->size(), 1u);
}

TEST_F(TradeStoreTest, RunsKeepTheirOwnFills) {
    auto other = db::TradeStore::StartRun(
        mDatabase, {kStrategy, "paper-venue", "paper",
                    types::FromEpochSeconds(1'700'000'100)});
    ASSERT_OK(other);
    EXPECT_NE((*other)->RunId(), mStore->RunId());
    const std::vector<types::Fill> fills = {FillOf("a", 10)};
    ASSERT_OK(mStore->WriteFills(fills));
    ASSERT_OK((*other)->WriteFills(fills));  // same fill id, other run
    EXPECT_EQ(mStore->LoadFills()->size(), 1u);
    EXPECT_EQ((*other)->LoadFills()->size(), 1u);
}

TEST_F(TradeStoreTest, PnlSnapshotsRoundTripWithNulls) {
    trading::PnlRow priced;
    priced.symbol = "ZEC/USDT";
    priced.quantity = D("0.5");
    priced.average_price = D("100");
    priced.mark_price = D("101");
    priced.realized = D("-0.25");
    priced.unrealized = D("0.5");
    trading::PnlRow unpriced;
    unpriced.symbol = "ETH/USDT";
    const std::vector<trading::PnlRow> rows = {priced, unpriced};
    ASSERT_OK(mStore->WritePnl(types::FromEpochSeconds(1'700'000'060), rows));

    auto loaded = mStore->LoadPnl();
    ASSERT_OK(loaded);
    ASSERT_EQ(loaded->size(), 2u);
    EXPECT_EQ((*loaded)[0].row.symbol, "ETH/USDT");  // ordered by symbol
    EXPECT_FALSE((*loaded)[0].row.mark_price.has_value());
    EXPECT_FALSE((*loaded)[0].row.unrealized.has_value());
    const auto& p = (*loaded)[1];
    EXPECT_EQ(p.time, types::FromEpochSeconds(1'700'000'060));
    EXPECT_EQ(p.row.quantity, D("0.5"));
    EXPECT_EQ(p.row.average_price, D("100"));
    EXPECT_EQ(p.row.mark_price, D("101"));
    EXPECT_EQ(p.row.realized, D("-0.25"));
    EXPECT_EQ(p.row.unrealized, D("0.5"));
}

TEST_F(TradeStoreTest, TheTotalsViewSumsTheSymbols) {
    trading::PnlRow a;
    a.symbol = "A/USDT";
    a.realized = D("1.5");
    a.unrealized = D("0.5");
    trading::PnlRow b;
    b.symbol = "B/USDT";
    b.realized = D("-1");  // no mark: counts as zero unrealized
    ASSERT_OK(mStore->WritePnl(types::FromEpochSeconds(1'700'000'060),
                               std::vector<trading::PnlRow>{a, b}));
    auto rows = mDatabase->Query(
        "SELECT realized, unrealized, total FROM pnl_totals WHERE run_id = $1",
        {std::to_string(mStore->RunId())});
    ASSERT_OK(rows);
    ASSERT_EQ(rows->size(), 1u);
    EXPECT_EQ(D((*rows)[0][0]->c_str()), D("0.5"));
    EXPECT_EQ(D((*rows)[0][1]->c_str()), D("0.5"));
    EXPECT_EQ(D((*rows)[0][2]->c_str()), D("1"));
}

TEST_F(TradeStoreTest, FinishSetsTheEndTime) {
    auto before =
        mDatabase->Query("SELECT ended_at IS NULL FROM runs WHERE run_id = $1",
                         {std::to_string(mStore->RunId())});
    ASSERT_OK(before);
    EXPECT_EQ(*(*before)[0][0], "t");
    ASSERT_OK(mStore->Finish(types::FromEpochSeconds(1'700'000'500)));
    auto after = mDatabase->Query(
        "SELECT (extract(epoch FROM ended_at))::bigint FROM runs "
        "WHERE run_id = $1",
        {std::to_string(mStore->RunId())});
    ASSERT_OK(after);
    EXPECT_EQ(*(*after)[0][0], "1700000500");
}

TEST_F(TradeStoreTest, OpenTradeLogStartsARunFromTheConfiguration) {
    const auto options = kestrel::testing::TestConnectionOptions();
    config::LoadOptions load;
    load.getenv = [&](const std::string& name) -> std::optional<std::string> {
        if (name == "KESTREL_DATABASE_PASSWORD") return options.password;
        return std::nullopt;
    };
    auto cfg = config::BinanceUSConfiguration::Parse(
        "[general]\nmode='paper'\nsymbols=['BTC/USD']\n[database]\ndbname='" +
            options.dbname + "'\nhost='" + options.host + "'\nuser='" +
            options.user + "'\n",
        load);
    ASSERT_OK(cfg);
    auto log = OpenTradeLog(*cfg, kStrategy, "a-venue");
    ASSERT_OK(log);
    ASSERT_OK((*log)->Finish(types::FromEpochSeconds(1'700'000'900)));

    auto rows = mDatabase->Query(
        "SELECT venue, mode, ended_at IS NOT NULL FROM runs "
        "WHERE strategy = $1 AND venue = 'a-venue'",
        {kStrategy});
    ASSERT_OK(rows);
    ASSERT_EQ(rows->size(), 1u);
    EXPECT_EQ(*(*rows)[0][1], "paper");
    EXPECT_EQ(*(*rows)[0][2], "t");
}

}  // namespace
