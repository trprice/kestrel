#include "kestrel/trading/pnl_tracker.hpp"

#include "kestrel/backtest/replay_data_source.hpp"
#include "kestrel/trading/paper_trading_venue.hpp"
#include "test_helpers.hpp"

namespace {

using namespace kestrel;
using kestrel::testing::D;
using trading::PnlTracker;
using types::OrderRequest;
using types::OrderType;
using types::Side;

types::Fill F(Side side, const char* quantity, const char* price,
              const char* fee = "0", const char* fee_asset = "USD",
              const char* symbol = "BTC/USD") {
    types::Fill f;
    f.symbol = symbol;
    f.side = side;
    f.quantity = D(quantity);
    f.price = D(price);
    f.fee = D(fee);
    f.fee_asset = fee_asset;
    return f;
}

TEST(PnlTrackerTest, StartsAtZero) {
    PnlTracker t;
    EXPECT_TRUE(t.Realized().IsZero());
    EXPECT_TRUE(t.Held("BTC/USD").IsZero());
}

TEST(PnlTrackerTest, SellRealizesAgainstTheAverageCost) {
    PnlTracker t;
    t.Apply(F(Side::kBuy, "1", "100"));
    t.Apply(F(Side::kBuy, "1", "200"));  // average 150
    EXPECT_EQ(t.Held("BTC/USD"), D("2"));
    t.Apply(F(Side::kSell, "1", "180"));  // +30
    EXPECT_EQ(t.Realized(), D("30"));
    t.Apply(F(Side::kSell, "1", "120"));  // -30, average unchanged by sells
    EXPECT_EQ(t.Realized(), D("0"));
    EXPECT_TRUE(t.Held("BTC/USD").IsZero());
}

TEST(PnlTrackerTest, FeesAreRealizedWhenPaid) {
    PnlTracker t;
    t.Apply(F(Side::kBuy, "1", "100", "0.4"));
    EXPECT_EQ(t.Realized(), D("-0.4"));
    t.Apply(F(Side::kSell, "1", "110", "0.5"));
    EXPECT_EQ(t.Realized(), D("9.1"));  // 10 - 0.4 - 0.5
}

TEST(PnlTrackerTest, AFeeInTheBaseAssetIsConvertedAtTheFillPrice) {
    PnlTracker t;
    t.Apply(F(Side::kBuy, "1", "100", "0.001", "BTC"));  // 0.001 BTC = 0.1 USD
    EXPECT_EQ(t.Realized(), D("-0.1"));
}

TEST(PnlTrackerTest, SellingMoreThanHeldRealizesNothingOnTheExcess) {
    PnlTracker t;
    t.Apply(F(Side::kBuy, "1", "100"));
    t.Apply(F(Side::kSell, "3", "150"));  // only 1 has a cost basis
    EXPECT_EQ(t.Realized(), D("50"));
    EXPECT_TRUE(t.Held("BTC/USD").IsZero());
}

TEST(PnlTrackerTest, ResetTakesPositionsAsTheCostBasis) {
    PnlTracker t;
    t.Apply(F(Side::kBuy, "1", "1"));
    types::Position p;
    p.symbol = "BTC/USD";
    p.quantity = D("2");
    p.average_price = D("100");
    p.realized_pnl = D("999");  // history the tracker must ignore
    t.Reset({p});
    EXPECT_TRUE(t.Realized().IsZero());
    EXPECT_EQ(t.Held("BTC/USD"), D("2"));
    t.Apply(F(Side::kSell, "2", "90"));
    EXPECT_EQ(t.Realized(), D("-20"));
}

TEST(PnlTrackerTest, ResetRealizedKeepsTheBasis) {
    PnlTracker t;
    t.Apply(F(Side::kBuy, "1", "100", "1"));
    t.ResetRealized();
    EXPECT_TRUE(t.Realized().IsZero());
    t.Apply(F(Side::kSell, "1", "130"));
    EXPECT_EQ(t.Realized(), D("30"));
}

TEST(PnlTrackerTest, PositionsListsEverySymbolIncludingFlatOnes) {
    PnlTracker t;
    t.Apply(F(Side::kBuy, "2", "100"));
    t.Apply(F(Side::kBuy, "1", "5", "0", "USD", "ETH/USD"));
    t.Apply(F(Side::kSell, "1", "7", "0", "USD", "ETH/USD"));

    auto positions = t.Positions();
    ASSERT_EQ(positions.size(), 2u);
    EXPECT_EQ(positions[0].symbol, "BTC/USD");
    EXPECT_EQ(positions[0].quantity, D("2"));
    EXPECT_EQ(positions[0].average_price, D("100"));
    EXPECT_EQ(positions[0].realized_pnl, D("0"));
    EXPECT_EQ(positions[1].symbol, "ETH/USD");
    EXPECT_TRUE(positions[1].quantity.IsZero());
    EXPECT_TRUE(positions[1].average_price.IsZero());
    EXPECT_EQ(positions[1].realized_pnl, D("2"));
}

TEST(PnlTrackerTest, SymbolsAreTrackedSeparately) {
    PnlTracker t;
    t.Apply(F(Side::kBuy, "1", "100"));
    t.Apply(F(Side::kBuy, "10", "5", "0", "USD", "ETH/USD"));
    t.Apply(F(Side::kSell, "10", "4", "0", "USD", "ETH/USD"));
    EXPECT_EQ(t.Realized("ETH/USD"), D("-10"));
    EXPECT_EQ(t.Realized("BTC/USD"), D("0"));
    EXPECT_EQ(t.Realized(), D("-10"));
    EXPECT_EQ(t.Held("BTC/USD"), D("1"));
}

// PaperTradingVenue reports P&L from a PnlTracker of its own, so these tests
// do not check two implementations against each other. They check the
// tracker against the venue's cash accounting, which is kept separately, and
// that a tracker fed the venue's fills ends up where the venue is.
class PaperReconciliationTest : public ::testing::Test {
   protected:
    PaperReconciliationTest() {
        data->SetSymbolInfo(
            {"BTC/USD", D("0.01"), D("0.00000001"), D("0.0001")});
        trading::PaperOptions options;  // default fees and slippage
        options.initial_balances = {{"USD", D("200000")}};
        venue = std::make_unique<trading::PaperTradingVenue>(
            data, options, [this] { return now; });
    }

    // Moves the market, then trades at it; every new fill goes to the
    // tracker, and the tracker is compared with the venue's own figures.
    void Trade(Side side, const char* quantity, const char* bid,
               const char* ask) {
        now += std::chrono::seconds(1);
        types::Quote q;
        q.symbol = "BTC/USD";
        q.venue = "replay";
        q.received_time = now;
        q.bid = D(bid);
        q.ask = D(ask);
        venue->OnQuote(q);

        OrderRequest order;
        order.symbol = "BTC/USD";
        order.side = side;
        order.type = OrderType::kMarket;
        order.quantity = D(quantity);
        ASSERT_OK(venue->PlaceOrder(order));

        auto fills = venue->GetFills(types::FromEpochSeconds(0));
        ASSERT_OK(fills);
        for (; applied < fills->size(); ++applied) {
            tracker.Apply((*fills)[applied]);
        }
    }

    void ExpectAgreesWithOpenPosition() {
        auto positions = venue->GetPositions();
        ASSERT_OK(positions);
        ASSERT_EQ(positions->size(), 1u);
        EXPECT_EQ((*positions)[0].realized_pnl, tracker.Realized());
        EXPECT_EQ((*positions)[0].quantity, tracker.Held("BTC/USD"));
    }

    std::shared_ptr<backtest::ReplayDataSource> data =
        std::make_shared<backtest::ReplayDataSource>();
    std::unique_ptr<trading::PaperTradingVenue> venue;
    types::Timestamp now = types::FromEpochSeconds(1000);
    PnlTracker tracker;
    std::size_t applied = 0;
};

TEST_F(PaperReconciliationTest, ATrackerFedTheFillsMatchesTheVenueWhileOpen) {
    Trade(Side::kBuy, "0.3", "49990.1", "50000.3");
    ExpectAgreesWithOpenPosition();
    Trade(Side::kBuy, "0.2", "51000.7", "51010.9");
    ExpectAgreesWithOpenPosition();
    Trade(Side::kSell, "0.1", "52000.5", "52005.1");
    ExpectAgreesWithOpenPosition();
    Trade(Side::kBuy, "0.07", "48000.2", "48001.3");
    ExpectAgreesWithOpenPosition();
    Trade(Side::kSell, "0.4", "49000.9", "49002.2");
    ExpectAgreesWithOpenPosition();
}

TEST_F(PaperReconciliationTest, MatchesTheCashBalanceOnceThePositionIsClosed) {
    Trade(Side::kBuy, "0.3", "49990.1", "50000.3");
    Trade(Side::kBuy, "0.2", "51000.7", "51010.9");
    Trade(Side::kSell, "0.5", "47000.4", "47010.8");
    ASSERT_TRUE(venue->GetPositions()->empty());  // its P&L is gone from there
    EXPECT_TRUE(tracker.Held("BTC/USD").IsZero());

    // All cash, so the balance change is the realized P&L, fees included.
    auto balances = venue->GetBalances();
    ASSERT_OK(balances);
    types::Decimal usd;
    for (const auto& b : *balances) {
        if (b.asset == "USD") {
            usd = b.total;
        }
    }
    EXPECT_EQ(usd - D("200000"), tracker.Realized());
    EXPECT_TRUE(tracker.Realized().IsNegative());
}

}  // namespace
