#include "kestrel/trading/pnl_tracker.hpp"

#include <spdlog/sinks/ringbuffer_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>

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
    EXPECT_EQ(t.Held("BTC/USD"), D("0.999"));
    t.Apply(F(Side::kSell, "0.999", "100"));
    EXPECT_EQ(t.Realized(), D("-0.1"));
    EXPECT_TRUE(t.Held("BTC/USD").IsZero());

    // A buy whose base fee is at least the quantity adds nothing.
    t.Apply(F(Side::kBuy, "0.001", "100", "0.002", "BTC"));
    EXPECT_TRUE(t.Held("BTC/USD").IsZero());
}

TEST(PnlTrackerTest, ASellFeeInTheBaseAssetLeavesAtTheAverageCost) {
    PnlTracker t;
    t.Apply(F(Side::kBuy, "1", "100"));
    // 0.51 BTC leaves at 120 (+10.2 over cost 100), less 0.01 * 120 = 1.2 fee.
    t.Apply(F(Side::kSell, "0.5", "120", "0.01", "BTC"));
    EXPECT_EQ(t.Realized(), D("9"));
    EXPECT_EQ(t.Held("BTC/USD"), D("0.49"));
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
            {"BTC/USD", D("0.01"), D("0.00000001"), D("0.0001"), {}});
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

TEST(PnlTrackerTest, WithoutShortsAnExcessSaleIsNotAShort) {
    PnlTracker t;
    t.Apply(F(Side::kBuy, "1", "100"));
    t.Apply(F(Side::kSell, "3", "110"));
    EXPECT_EQ(t.Held("BTC/USD"), D("0"));
    EXPECT_EQ(t.Realized(), D("10"));
}

TEST(PnlTrackerTest, WithoutShortsASaleAndItsBaseFeeTakeNoMoreThanIsHeld) {
    PnlTracker t;
    t.Apply(F(Side::kBuy, "1", "100"));
    // 1 sold and 0.01 BTC of fee, of the 1 held: all of it leaves at 110
    // (+10), less 0.01 * 110 = 1.1 fee.
    t.Apply(F(Side::kSell, "1", "110", "0.01", "BTC"));
    EXPECT_TRUE(t.Held("BTC/USD").IsZero());
    EXPECT_EQ(t.Realized(), D("8.9"));
}

TEST(PnlTrackerTest, WithShortsASaleBeyondTheHoldingGoesShort) {
    PnlTracker t(/*shorts=*/true);
    t.Apply(F(Side::kBuy, "1", "100"));
    t.Apply(F(Side::kSell, "3", "110"));  // +10 on the long, then 2 short
    EXPECT_EQ(t.Held("BTC/USD"), D("-2"));
    EXPECT_EQ(t.Realized(), D("10"));
    t.Apply(F(Side::kSell, "2", "120"));  // average 115
    EXPECT_EQ(t.Positions().front().average_price, D("115"));
    t.Apply(F(Side::kBuy, "1", "105", "1"));  // +10, less the fee
    EXPECT_EQ(t.Held("BTC/USD"), D("-3"));
    EXPECT_EQ(t.Realized(), D("19"));
    t.Apply(F(Side::kBuy, "4", "125"));  // -30 on the cover, then 1 long
    EXPECT_EQ(t.Held("BTC/USD"), D("1"));
    EXPECT_EQ(t.Positions().front().average_price, D("125"));
    EXPECT_EQ(t.Realized(), D("-11"));
}

TEST(PnlTrackerTest, WithShortsBaseAssetFeesApplyToShortSalesAndCovers) {
    PnlTracker t(/*shorts=*/true);
    t.Apply(F(Side::kSell, "1", "100", "0.01", "BTC"));
    EXPECT_EQ(t.Held("BTC/USD"), D("-1.01"));
    EXPECT_EQ(t.Positions().front().average_price, D("100"));
    EXPECT_EQ(t.Realized(), D("-1"));

    // Buying 1 with a 0.01 BTC fee covers 0.99 of the short at 90 (+9.9, less
    // 0.9 fee).
    t.Apply(F(Side::kBuy, "1", "90", "0.01", "BTC"));
    EXPECT_EQ(t.Held("BTC/USD"), D("-0.02"));
    EXPECT_EQ(t.Realized(), D("8"));
}

class WarnCapture {
   public:
    WarnCapture()
        : mSink(std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(64)) {
        mSink->set_level(spdlog::level::warn);
        spdlog::default_logger()->sinks().push_back(mSink);
    }
    ~WarnCapture() {
        auto& sinks = spdlog::default_logger()->sinks();
        sinks.erase(std::remove(sinks.begin(), sinks.end(), mSink),
                    sinks.end());
    }
    bool Empty() const { return mSink->last_formatted().empty(); }
    bool Saw(const std::string& text) const {
        for (const auto& line : mSink->last_formatted()) {
            if (line.find(text) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

   private:
    std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> mSink;
};

types::Fill TimedFill(const char* id, int seconds, Side side,
                      const char* quantity, const char* price,
                      const char* fee = "0", const char* fee_asset = "USDT",
                      const char* symbol = "ZEC/USDT") {
    types::Fill f = F(side, quantity, price, fee, fee_asset, symbol);
    f.fill_id = id;
    f.time = types::FromEpochSeconds(1'700'000'000 + seconds);
    return f;
}

types::Balance Bal(const char* asset, const char* total) {
    return types::Balance{asset, D(total), D(total), D("0")};
}

TEST(ReconcileStartingPositionsTest,
     ReplaysChronologicallyResetsRoundTripsAndZeroesRealizedPnl) {
    WarnCapture warns;
    // Out-of-order input, including a same-second numeric tie broken by
    // (length(fill_id), fill_id) so "ZECUSDT:9" precedes "ZECUSDT:10":
    //   t=10 ("ZECUSDT:9"): buy 1 @ 80
    //   t=10 ("ZECUSDT:10"): sell 1 @ 120 -> flat (+40 historical, reset!)
    //   t=20 ("c"): buy 1.005 @ 100 with 0.005 ZEC base fee -> 1.000 ZEC @ 100
    //   t=30 ("d"): buy 1 @ 120 -> 2.000 ZEC @ 110
    const std::vector<types::Fill> fills = {
        TimedFill("d", 30, Side::kBuy, "1", "120"),
        TimedFill("ZECUSDT:10", 10, Side::kSell, "1", "120"),
        TimedFill("c", 20, Side::kBuy, "1.005", "100", "0.005", "ZEC"),
        TimedFill("ZECUSDT:9", 10, Side::kBuy, "1", "80"),
    };
    const std::vector<types::Symbol> symbols = {"ZEC/USDT"};
    const std::vector<types::Balance> balances = {
        Bal("ZEC", "2"), Bal("USDT", "5000"), Bal("DOGE", "100")};

    bool asked_price = false;
    const auto positions = trading::ReconcileStartingPositions(
        symbols, fills, balances, [&](const types::Symbol&) {
            asked_price = true;
            return D("999");
        });
    EXPECT_FALSE(asked_price);
    EXPECT_TRUE(warns.Empty());
    ASSERT_EQ(positions.size(), 1u);
    EXPECT_EQ(positions[0].symbol, "ZEC/USDT");
    EXPECT_EQ(positions[0].quantity, D("2"));
    EXPECT_EQ(positions[0].average_price, D("110"));
    EXPECT_TRUE(positions[0].realized_pnl.IsZero());
}

TEST(ReconcileStartingPositionsTest,
     AggregatesSharedBaseAssetAcrossPairsWithoutDoubleCountingOrFalseWarning) {
    // 1 ZEC bought on ZEC/USD @ 90 and 1 ZEC bought on ZEC/USDT @ 110 == 2 ZEC
    // in wallet: exact match across the shared base asset.
    {
        WarnCapture warns;
        const std::vector<types::Symbol> symbols = {"ZEC/USD", "ZEC/USDT"};
        const std::vector<types::Fill> fills = {
            TimedFill("f1", 10, Side::kBuy, "1", "90", "0", "USD", "ZEC/USD"),
            TimedFill("f2", 20, Side::kBuy, "1", "110", "0", "USDT",
                      "ZEC/USDT"),
        };
        const std::vector<types::Balance> balances = {Bal("ZEC", "2")};
        const auto positions =
            trading::ReconcileStartingPositions(symbols, fills, balances);
        EXPECT_TRUE(warns.Empty());
        ASSERT_EQ(positions.size(), 2u);
        EXPECT_EQ(positions[0].symbol, "ZEC/USD");
        EXPECT_EQ(positions[0].quantity, D("1"));
        EXPECT_EQ(positions[0].average_price, D("90"));
        EXPECT_EQ(positions[1].symbol, "ZEC/USDT");
        EXPECT_EQ(positions[1].quantity, D("1"));
        EXPECT_EQ(positions[1].average_price, D("110"));
    }

    // 2 ZEC transferred in with no stored fills across ZEC/USD + ZEC/USDT:
    // counted once (sum == 2 ZEC), never 2 ZEC on each pair.
    {
        WarnCapture warns;
        const std::vector<types::Symbol> symbols = {"ZEC/USD", "ZEC/USDT"};
        const std::vector<types::Balance> balances = {Bal("ZEC", "2")};
        const auto positions = trading::ReconcileStartingPositions(
            symbols, {}, balances, [&](const types::Symbol& sym) {
                return D(sym == "ZEC/USD" ? "100" : "101");
            });
        EXPECT_TRUE(warns.Saw("ZEC"));
        EXPECT_TRUE(warns.Saw("ZEC/USD"));
        types::Decimal total_zec;
        for (const auto& p : positions) {
            total_zec += p.quantity;
        }
        EXPECT_EQ(total_zec, D("2"));
        ASSERT_EQ(positions.size(), 1u);
        EXPECT_EQ(positions[0].symbol, "ZEC/USD");
        EXPECT_EQ(positions[0].average_price, D("100"));
    }

    // Deficit across shared base: 1 ZEC on ZEC/USD + 1 ZEC on ZEC/USDT, but
    // only 1.4 ZEC remains in wallet.
    {
        WarnCapture warns;
        const std::vector<types::Symbol> symbols = {"ZEC/USD", "ZEC/USDT"};
        const std::vector<types::Fill> fills = {
            TimedFill("f1", 10, Side::kBuy, "1", "90", "0", "USD", "ZEC/USD"),
            TimedFill("f2", 20, Side::kBuy, "1", "110", "0", "USDT",
                      "ZEC/USDT"),
        };
        const std::vector<types::Balance> balances = {Bal("ZEC", "1.4")};
        const auto positions =
            trading::ReconcileStartingPositions(symbols, fills, balances);
        EXPECT_TRUE(warns.Saw("ZEC"));
        EXPECT_TRUE(warns.Saw("ZEC/USDT"));
        ASSERT_EQ(positions.size(), 2u);
        EXPECT_EQ(positions[0].quantity, D("1"));
        EXPECT_EQ(positions[0].average_price, D("90"));
        EXPECT_EQ(positions[1].quantity, D("0.4"));
        EXPECT_EQ(positions[1].average_price, D("110"));
    }
}

TEST(ReconcileStartingPositionsTest,
     ValuesExcessHoldingsAtStartupMarkAndClampsDeficits) {
    const std::vector<types::Symbol> symbols = {"ZEC/USDT"};

    // Pure transfer-in: 2 ZEC in wallet, 0 stored fills, mark = 105. Selling
    // at the startup mark of 105 realizes zero phantom gain/loss.
    {
        WarnCapture warns;
        const auto positions = trading::ReconcileStartingPositions(
            symbols, {}, std::vector{Bal("ZEC", "2")},
            [](const types::Symbol&) { return D("105"); });
        EXPECT_TRUE(warns.Saw("ZEC"));
        EXPECT_TRUE(warns.Saw("ZEC/USDT"));
        ASSERT_EQ(positions.size(), 1u);
        EXPECT_EQ(positions[0].quantity, D("2"));
        EXPECT_EQ(positions[0].average_price, D("105"));

        PnlTracker tracker;
        tracker.Reset(positions);
        tracker.Apply(F(Side::kSell, "2", "105", "0", "USDT", "ZEC/USDT"));
        EXPECT_TRUE(tracker.Realized().IsZero());
    }

    // Blended excess: 1 ZEC synced @ 80 + 1 ZEC older than history @ mark 100
    // -> 2 ZEC @ 90. Selling all 2 at 100 realizes +20 (+20 on synced, 0 on
    // older).
    {
        WarnCapture warns;
        const std::vector<types::Fill> fills = {
            TimedFill("f1", 10, Side::kBuy, "1", "80")};
        const auto positions = trading::ReconcileStartingPositions(
            symbols, fills, std::vector{Bal("ZEC", "2")},
            [](const types::Symbol&) { return D("100"); });
        EXPECT_TRUE(warns.Saw("ZEC/USDT"));
        ASSERT_EQ(positions.size(), 1u);
        EXPECT_EQ(positions[0].quantity, D("2"));
        EXPECT_EQ(positions[0].average_price, D("90"));

        PnlTracker tracker;
        tracker.Reset(positions);
        tracker.Apply(F(Side::kSell, "2", "100", "0", "USDT", "ZEC/USDT"));
        EXPECT_EQ(tracker.Realized(), D("20"));
    }

    // Excess when no reference price is available: unpriced excess stays out
    // of the starting position so selling it realizes nothing rather than a
    // phantom gain.
    {
        WarnCapture warns;
        const std::vector<types::Fill> fills = {
            TimedFill("f1", 10, Side::kBuy, "1", "80")};
        const auto with_history = trading::ReconcileStartingPositions(
            symbols, fills, std::vector{Bal("ZEC", "2")},
            [](const types::Symbol&) {
                return std::optional<types::Decimal>{};
            });
        EXPECT_TRUE(warns.Saw("no reference price"));
        ASSERT_EQ(with_history.size(), 1u);
        EXPECT_EQ(with_history[0].quantity, D("1"));
        EXPECT_EQ(with_history[0].average_price, D("80"));

        PnlTracker tracker;
        tracker.Reset(with_history);
        tracker.Apply(F(Side::kSell, "2", "100", "0", "USDT", "ZEC/USDT"));
        EXPECT_EQ(tracker.Realized(), D("20"));

        WarnCapture empty_warns;
        const auto without_history = trading::ReconcileStartingPositions(
            symbols, {}, std::vector{Bal("ZEC", "2")});
        EXPECT_TRUE(empty_warns.Saw("no reference price"));
        EXPECT_TRUE(without_history.empty());

        tracker.Reset(without_history);
        tracker.Apply(F(Side::kSell, "2", "105", "0", "USDT", "ZEC/USDT"));
        EXPECT_TRUE(tracker.Realized().IsZero());
    }

    // External withdrawal: 2 ZEC @ 90 in history, 1.25 ZEC in wallet -> 1.25 @
    // 90; 0 ZEC in wallet -> clamped to 0 @ 0.
    {
        WarnCapture warns;
        const std::vector<types::Fill> fills = {
            TimedFill("f1", 10, Side::kBuy, "2", "90")};
        const auto partial = trading::ReconcileStartingPositions(
            symbols, fills, std::vector{Bal("ZEC", "1.25")});
        EXPECT_TRUE(warns.Saw("ZEC"));
        EXPECT_TRUE(warns.Saw("ZEC/USDT"));
        ASSERT_EQ(partial.size(), 1u);
        EXPECT_EQ(partial[0].quantity, D("1.25"));
        EXPECT_EQ(partial[0].average_price, D("90"));

        WarnCapture zero_warns;
        const auto zero = trading::ReconcileStartingPositions(
            symbols, fills, std::vector<types::Balance>{});
        EXPECT_TRUE(zero_warns.Saw("ZEC/USDT"));
        ASSERT_EQ(zero.size(), 1u);
        EXPECT_TRUE(zero[0].quantity.IsZero());
        EXPECT_TRUE(zero[0].average_price.IsZero());
    }

    // Coins in the wallet beyond the run's own symbol replay (even when total
    // replay across other pairs like ZEC/BTC was higher) have no quote-currency
    // basis on ZEC/USDT and are valued at reference_price.
    {
        WarnCapture warns;
        const std::vector<types::Fill> fills = {
            TimedFill("f1", 10, Side::kBuy, "1", "85", "0", "USDT", "ZEC/USDT"),
            TimedFill("f2", 20, Side::kBuy, "2", "0.002", "0", "BTC",
                      "ZEC/BTC"),
        };
        const auto blended = trading::ReconcileStartingPositions(
            symbols, fills, std::vector{Bal("ZEC", "1.5")},
            [](const types::Symbol&) { return D("115"); });
        EXPECT_TRUE(warns.Saw("replayed on other pairs"));
        EXPECT_TRUE(warns.Saw("ZEC/USDT"));
        ASSERT_EQ(blended.size(), 1u);
        EXPECT_EQ(blended[0].quantity, D("1.5"));
        EXPECT_EQ(blended[0].average_price, D("95"));
    }
}

TEST(ReconcileStartingPositionsTest,
     HoldingsBoughtOnOtherPairsAreValuedAtReferencePriceInsteadOfVanishing) {
    const std::vector<types::Symbol> symbols = {"ZEC/USD"};
    const std::vector<types::Fill> fills = {
        TimedFill("f1", 10, Side::kBuy, "2", "0.002", "0", "BTC", "ZEC/BTC"),
    };

    // 2 ZEC bought on ZEC/BTC with wallet = 2 ZEC: replayed_ours on ZEC/USD is
    // 0, so the 2 ZEC are valued on ZEC/USD at reference_price rather than
    // vanishing when wallet == replayed_all.
    {
        WarnCapture warns;
        const auto positions = trading::ReconcileStartingPositions(
            symbols, fills, std::vector{Bal("ZEC", "2")},
            [](const types::Symbol& sym) {
                EXPECT_EQ(sym, "ZEC/USD");
                return D("105");
            });
        EXPECT_TRUE(warns.Saw("2 ZEC replayed on other pairs"));
        EXPECT_TRUE(warns.Saw("ZEC/USD"));
        ASSERT_EQ(positions.size(), 1u);
        EXPECT_EQ(positions[0].symbol, "ZEC/USD");
        EXPECT_EQ(positions[0].quantity, D("2"));
        EXPECT_EQ(positions[0].average_price, D("105"));
    }

    // Without a reference price, the unpriced cross-pair coins stay out of the
    // starting basis with a warning instead of inventing a zero cost.
    {
        WarnCapture warns;
        const auto positions = trading::ReconcileStartingPositions(
            symbols, fills, std::vector{Bal("ZEC", "2")});
        EXPECT_TRUE(warns.Saw("2 ZEC replayed on other pairs"));
        EXPECT_TRUE(warns.Saw("no reference price"));
        EXPECT_TRUE(positions.empty());
    }

    // Dust check uses ZEC/USD's USD reference price, never ZEC/BTC's 0.002 BTC.
    {
        WarnCapture dust_warns;
        const std::vector<types::Fill> small_btc = {
            TimedFill("d1", 10, Side::kBuy, "0.005", "0.002", "0", "BTC",
                      "ZEC/BTC"),
        };
        const auto dust = trading::ReconcileStartingPositions(
            symbols, small_btc, std::vector{Bal("ZEC", "0.005")},
            [](const types::Symbol&) { return D("100"); });  // 0.50 USD < 1
        ASSERT_EQ(dust.size(), 1u);
        EXPECT_EQ(dust[0].quantity, D("0.005"));
        EXPECT_TRUE(dust_warns.Empty());
    }
}

TEST(ReconcileStartingPositionsTest,
     CoinsSpentAsAnotherPairsQuoteComeOffTheReplayAtAverageCost) {
    WarnCapture warns;
    // BTC bought on BTC/USDT, then 0.5 BTC (plus a 0.001 BTC fee) spent on
    // ZEC/BTC, which the run does not trade: the replay ends at 1.499 BTC,
    // matching the wallet, with the average cost unchanged.
    const std::vector<types::Fill> fills = {
        TimedFill("b1", 10, Side::kBuy, "1", "100", "0", "USDT", "BTC/USDT"),
        TimedFill("b2", 20, Side::kBuy, "1", "200", "0", "USDT", "BTC/USDT"),
        TimedFill("z1", 30, Side::kBuy, "10", "0.05", "0.001", "BTC",
                  "ZEC/BTC"),
        // Sold for BTC: received coins have no USDT cost, so they are left
        // for the excess rule (none here, as the wallet does not show them).
        TimedFill("z2", 40, Side::kSell, "2", "0.05", "0", "BTC", "ZEC/BTC"),
    };
    const std::vector<types::Symbol> symbols = {"BTC/USDT"};
    const std::vector<types::Balance> balances = {Bal("BTC", "1.499")};
    const auto positions =
        trading::ReconcileStartingPositions(symbols, fills, balances);
    ASSERT_EQ(positions.size(), 1u);
    EXPECT_EQ(positions[0].symbol, "BTC/USDT");
    EXPECT_EQ(positions[0].quantity, D("1.499"));
    EXPECT_EQ(positions[0].average_price, D("150"));
    EXPECT_TRUE(warns.Empty());
}

TEST(ReconcileStartingPositionsTest, DustDifferencesLogAtInfoNotAsWarnings) {
    const std::vector<types::Symbol> symbols = {"BTC/USDT"};
    const auto price = [](const types::Symbol&) {
        return std::optional<types::Decimal>(D("60000"));
    };
    {
        // 0.00000012 BTC with no history, worth 0.0072 USDT: info only.
        WarnCapture warns;
        const std::vector<types::Balance> balances = {Bal("BTC", "0.00000012")};
        const auto positions =
            trading::ReconcileStartingPositions(symbols, {}, balances, price);
        ASSERT_EQ(positions.size(), 1u);
        EXPECT_EQ(positions[0].quantity, D("0.00000012"));
        EXPECT_EQ(positions[0].average_price, D("60000"));
        EXPECT_TRUE(warns.Empty());
    }
    {
        // 0.001 BTC (60 USDT) unexplained: a warning.
        WarnCapture warns;
        const std::vector<types::Balance> balances = {Bal("BTC", "0.001")};
        (void)trading::ReconcileStartingPositions(symbols, {}, balances, price);
        EXPECT_TRUE(warns.Saw("more than stored account_fills"));
    }
    {
        // A replayed 1 BTC @ 100 against a wallet 0.000001 short (0.0001
        // USDT at cost): clamped, at info.
        WarnCapture warns;
        const std::vector<types::Fill> fills = {TimedFill(
            "b1", 10, Side::kBuy, "1", "100", "0", "USDT", "BTC/USDT")};
        const std::vector<types::Balance> balances = {Bal("BTC", "0.999999")};
        const auto positions =
            trading::ReconcileStartingPositions(symbols, fills, balances);
        ASSERT_EQ(positions.size(), 1u);
        EXPECT_EQ(positions[0].quantity, D("0.999999"));
        EXPECT_TRUE(warns.Empty());
    }
}

}  // namespace
