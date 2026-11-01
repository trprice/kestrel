#include "kestrel/backtest/summary.hpp"
#include "test_helpers.hpp"

namespace {

using namespace kestrel;
using backtest::SummaryBuilder;
using kestrel::testing::D;
using types::ErrorCategory;
using types::Side;

constexpr const char* kZec = "ZEC/USDT";
constexpr const char* kBtc = "BTC/USDT";

types::Timestamp At(int seconds) { return types::FromEpochSeconds(seconds); }

std::vector<types::Balance> Balances(const char* usdt, const char* zec) {
    return {{"USDT", D(usdt), D(usdt), D("0")},
            {"ZEC", D(zec), D(zec), D("0")}};
}

types::Fill FillOf(Side side, const char* qty, const char* price, int seconds,
                   const char* fee = "0", const char* fee_asset = "USDT",
                   types::Liquidity liquidity = types::Liquidity::kMaker) {
    types::Fill f;
    f.symbol = kZec;
    f.side = side;
    f.quantity = D(qty);
    f.price = D(price);
    f.fee = D(fee);
    f.fee_asset = fee_asset;
    f.liquidity = liquidity;
    f.time = At(seconds);
    return f;
}

// 1000 USDT and 1 ZEC, sampled every minute from time 0.
SummaryBuilder Builder(
    std::chrono::milliseconds interval = std::chrono::minutes(1)) {
    auto b = SummaryBuilder::Create(
        {kZec}, "USD", {{"USDT", D("1000")}, {"ZEC", D("1")}}, At(0), interval);
    if (!b) {
        throw std::runtime_error(b.error().ToString());
    }
    return std::move(*b);
}

TEST(BacktestSummaryTest, ValuesTheBalancesAndComparesWithHolding) {
    auto b = Builder();
    b.Observe(At(10), kZec, D("100"), Balances("1000", "1"));
    b.Observe(At(20), kZec, D("100"), Balances("900", "2"));  // bought 1 @ 100
    b.Observe(At(30), kZec, D("110"), Balances("900", "2"));
    auto s = b.Finish(At(40), {FillOf(Side::kBuy, "1", "100", 20)});
    ASSERT_OK(s);

    EXPECT_EQ(s->quote_asset, "USDT");
    EXPECT_EQ(s->start_time, At(10));
    EXPECT_EQ(s->start_equity, D("1100"));
    EXPECT_EQ(s->end_equity, D("1120"));   // 900 + 2 * 110
    EXPECT_EQ(s->hold_equity, D("1110"));  // 1000 + 1 * 110
    EXPECT_EQ(s->return_fraction, D("0.01818182"));
    EXPECT_EQ(s->realized, D("0"));
    EXPECT_EQ(s->unrealized, D("20"));
    EXPECT_EQ(s->fills, 1u);
    EXPECT_EQ(s->buys, 1u);
    EXPECT_EQ(s->volume, D("100"));
    ASSERT_EQ(s->symbols.size(), 1u);
    EXPECT_EQ(s->symbols[0].first_mark, D("100"));
    EXPECT_EQ(s->symbols[0].last_mark, D("110"));
    EXPECT_EQ(s->symbols[0].min_held, D("1"));
    EXPECT_EQ(s->symbols[0].max_held, D("2"));
    EXPECT_EQ(s->symbols[0].end_held, D("2"));
}

TEST(BacktestSummaryTest,
     CountsWinningSellsWithTheStartingHoldingAtItsFirstMark) {
    auto b = Builder();
    b.Observe(At(10), kZec, D("100"), Balances("1000", "1"));
    b.Observe(At(20), kZec, D("100"), Balances("900", "2"));
    b.Observe(At(30), kZec, D("105"), Balances("1005", "1"));
    b.Observe(At(40), kZec, D("95"), Balances("1100", "0"));
    auto s = b.Finish(At(50), {FillOf(Side::kBuy, "1", "100", 20),
                               // Both lots cost 100, so 105 wins and 95 loses.
                               FillOf(Side::kSell, "1", "105", 30),
                               FillOf(Side::kSell, "1", "95", 40, "0.001",
                                      "ZEC", types::Liquidity::kTaker)});
    ASSERT_OK(s);
    EXPECT_EQ(s->closing_sells, 2u);
    EXPECT_EQ(s->winning_sells, 1u);
    EXPECT_EQ(s->maker_fills, 2u);
    EXPECT_EQ(s->taker_fills, 1u);
    EXPECT_EQ(s->fees, D("0.095"));       // 0.001 ZEC at 95
    EXPECT_EQ(s->realized, D("-0.095"));  // +5 - 5 - fee
    EXPECT_EQ(s->end_equity, D("1100"));
    EXPECT_EQ(s->unrealized, s->end_equity - s->start_equity - s->realized);
}

TEST(BacktestSummaryTest, ASellOfNothingHeldIsNotAClosingSell) {
    auto b = SummaryBuilder::Create({kZec}, "USD", {{"USDT", D("1000")}}, At(0),
                                    std::chrono::minutes(1));
    ASSERT_OK(b);
    b->Observe(At(10), kZec, D("100"),
               {{"USDT", D("1000"), D("1000"), D("0")}});
    auto s = b->Finish(At(20), {FillOf(Side::kSell, "1", "100", 15)});
    ASSERT_OK(s);
    EXPECT_EQ(s->sells, 1u);
    EXPECT_EQ(s->closing_sells, 0u);
}

TEST(BacktestSummaryTest, MeasuresTheLargestFallFromAPeak) {
    auto b = Builder();
    b.Observe(At(10), kZec, D("100"), Balances("1000", "1"));  // 1100
    b.Observe(At(20), kZec, D("200"), Balances("1000", "1"));  // 1200, peak
    b.Observe(At(30), kZec, D("80"), Balances("1000", "1"));   // 1080
    b.Observe(At(40), kZec, D("150"), Balances("1000", "1"));  // 1150
    b.Observe(At(50), kZec, D("120"), Balances("1000", "1"));  // 1120
    auto s = b.Finish(At(60), {});
    ASSERT_OK(s);
    EXPECT_EQ(s->max_drawdown, D("120"));
    EXPECT_EQ(s->max_drawdown_fraction, D("0.1"));
    EXPECT_EQ(s->drawdown_peak_time, At(20));
    EXPECT_EQ(s->drawdown_trough_time, At(30));
}

TEST(BacktestSummaryTest, SamplesTheCurveEveryIntervalFromTheStart) {
    auto b = Builder();
    b.Observe(At(30), kZec, D("100"), Balances("1000", "1"));   // 1100
    b.Observe(At(130), kZec, D("110"), Balances("1000", "1"));  // 1110
    auto s = b.Finish(At(240), {});
    ASSERT_OK(s);

    std::vector<std::pair<types::Timestamp, types::Decimal>> got;
    for (const auto& p : s->curve) {
        got.emplace_back(p.time, p.equity);
    }
    const std::vector<std::pair<types::Timestamp, types::Decimal>> want = {
        {At(30), D("1100")},  {At(60), D("1100")},  {At(120), D("1100")},
        {At(180), D("1110")}, {At(240), D("1110")},
    };
    EXPECT_EQ(got, want);
    EXPECT_EQ(s->curve[3].hold_equity, D("1110"));
    EXPECT_EQ(s->curve[3].marks, std::vector<types::Decimal>{D("110")});
    EXPECT_EQ(s->curve[3].held, std::vector<types::Decimal>{D("1")});
}

TEST(BacktestSummaryTest, AZeroIntervalKeepsTheFirstAndLastPoints) {
    auto b = Builder(std::chrono::milliseconds(0));
    b.Observe(At(30), kZec, D("100"), Balances("1000", "1"));
    b.Observe(At(130), kZec, D("110"), Balances("1000", "1"));
    auto s = b.Finish(At(240), {});
    ASSERT_OK(s);
    ASSERT_EQ(s->curve.size(), 2u);
    EXPECT_EQ(s->curve[0].time, At(30));
    EXPECT_EQ(s->curve[1].time, At(240));
}

TEST(BacktestSummaryTest, WaitsUntilEveryBalanceHeldHasAPrice) {
    auto b = SummaryBuilder::Create(
        {kZec, kBtc}, "USD",
        {{"USDT", D("1000")}, {"ZEC", D("1")}, {"BTC", D("0.1")}}, At(0),
        std::chrono::minutes(1));
    ASSERT_OK(b);
    const std::vector<types::Balance> balances = {
        {"USDT", D("1000"), D("1000"), D("0")},
        {"ZEC", D("1"), D("1"), D("0")},
        {"BTC", D("0.1"), D("0.1"), D("0")}};
    b->Observe(At(10), kZec, D("100"), balances);  // BTC has no price yet
    b->Observe(At(20), kBtc, D("50000"), balances);
    auto s = b->Finish(At(30), {});
    ASSERT_OK(s);
    EXPECT_EQ(s->start_time, At(20));
    EXPECT_EQ(s->start_equity, D("6100"));
    EXPECT_EQ(s->curve.front().time, At(20));
}

TEST(BacktestSummaryTest, FailsWithoutAPriceForEveryBalanceHeld) {
    auto b = Builder();
    b.Observe(At(10), kZec, std::nullopt, Balances("1000", "1"));
    auto s = b.Finish(At(20), {});
    ASSERT_FALSE(s);
    EXPECT_EQ(s.error().category, ErrorCategory::kNotFound);
}

TEST(BacktestSummaryTest, RefusesWhatItCannotValue) {
    auto mixed = SummaryBuilder::Create({kZec, "ETH/BTC"}, "USD", {}, At(0),
                                        std::chrono::minutes(1));
    ASSERT_FALSE(mixed);
    EXPECT_EQ(mixed.error().category, ErrorCategory::kInvalidArgument);

    auto unpriced = SummaryBuilder::Create(
        {kZec}, "USD", {{"USDT", D("1000")}, {"BNB", D("1")}}, At(0),
        std::chrono::minutes(1));
    ASSERT_FALSE(unpriced);
    EXPECT_EQ(unpriced.error().category, ErrorCategory::kInvalidArgument);

    // An equity takes the default quote asset.
    EXPECT_TRUE(SummaryBuilder::Create({"AAPL"}, "USD", {{"USD", D("1000")}},
                                       At(0), std::chrono::minutes(1)));
}

TEST(BacktestSummaryTest, MarksOutEachFillAgainstTheMidAfterIt) {
    auto b = Builder();
    b.Observe(At(10), kZec, D("100"), Balances("1000", "1"));
    b.Observe(At(20), kZec, D("98"), Balances("901", "2"));  // the buy's quote
    b.Observe(At(21), kZec, D("97"), Balances("901", "2"));
    b.Observe(At(80), kZec, D("102"), Balances("901", "2"));
    b.Observe(At(350), kZec, D("102"), Balances("1002", "1"));
    auto s = b.Finish(At(400), {FillOf(Side::kBuy, "1", "99", 5),  // no mid yet
                                FillOf(Side::kBuy, "1", "99", 20),
                                FillOf(Side::kSell, "1", "101", 350)});
    ASSERT_OK(s);

    struct Want {
        std::optional<std::chrono::milliseconds> horizon;
        std::size_t fills;
        const char* pnl;
        const char* bps;
    };
    using std::chrono::milliseconds;
    using std::chrono::minutes;
    using std::chrono::seconds;
    // The buy at 99: the mid before it was 100 (+1), then 97 (-2), then 102
    // (+3). The sell at 101 against 102 is -1 throughout, and too near the
    // end for a minute's markout.
    const std::vector<Want> want = {
        {milliseconds(0), 2, "0", "0"}, {seconds(1), 2, "-3", "-150"},
        {seconds(10), 2, "-3", "-150"}, {minutes(1), 1, "3", "303.03"},
        {minutes(5), 1, "3", "303.03"}, {std::nullopt, 2, "2", "100"},
    };
    ASSERT_EQ(s->markouts.size(), want.size());
    for (std::size_t i = 0; i < want.size(); ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(s->markouts[i].horizon, want[i].horizon);
        EXPECT_EQ(s->markouts[i].fills, want[i].fills);
        EXPECT_EQ(s->markouts[i].pnl, D(want[i].pnl));
        EXPECT_EQ(s->markouts[i].bps, D(want[i].bps));
    }
}

TEST(BacktestSummaryTest, MarksAtTheMidOrTheLastTrade) {
    types::Quote q;
    q.bid = D("99");
    q.ask = D("100");
    q.last = D("98");
    EXPECT_EQ(SummaryBuilder::MarkOf(q), D("99.5"));
    q.ask.reset();
    EXPECT_EQ(SummaryBuilder::MarkOf(q), D("98"));
    q.last.reset();
    EXPECT_FALSE(SummaryBuilder::MarkOf(q));
}

}  // namespace
