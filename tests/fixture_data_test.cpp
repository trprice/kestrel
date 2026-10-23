// Database tests against the market data checked in under tests/data/market
// (real Kraken and Binance.US bars and quotes), so they need no venue
// credentials. Like the other store tests they run only when
// KESTREL_TEST_DBNAME is set; see README "Database integration tests".

#include <map>

#include "db_fixture.hpp"

namespace {

using namespace kestrel;
using kestrel::testing::FixtureBars;

class FixtureDataTest : public ::testing::Test {
   protected:
    static void SetUpTestSuite() {
        if (!kestrel::testing::DatabaseConfigured()) {
            return;
        }
        sData = std::make_unique<kestrel::testing::FixtureData>(
            kestrel::testing::ReadFixtureData());
    }
    static void TearDownTestSuite() { sData.reset(); }

    void SetUp() override {
        if (!kestrel::testing::DatabaseConfigured()) {
            GTEST_SKIP() << "KESTREL_TEST_DBNAME not set";
        }
        auto database = kestrel::testing::ConnectTestDatabase();
        ASSERT_OK(database);
        mDatabase = std::move(*database);
        ASSERT_OK(kestrel::testing::LoadFixtureData(mDatabase, *sData));
        mBars = std::make_unique<db::BarStore>(mDatabase);
        mQuotes = std::make_unique<db::QuoteStore>(mDatabase);
    }

    static constexpr auto kForever = std::chrono::years(100);
    static types::Timestamp Epoch() { return types::FromEpochMillis(0); }

    static std::unique_ptr<kestrel::testing::FixtureData> sData;
    std::shared_ptr<db::Database> mDatabase;
    std::unique_ptr<db::BarStore> mBars;
    std::unique_ptr<db::QuoteStore> mQuotes;
};

std::unique_ptr<kestrel::testing::FixtureData> FixtureDataTest::sData;

TEST_F(FixtureDataTest, CheckedInDataCoversBothVenuesAndAllTimeframes) {
    std::map<std::string, std::size_t> sets_per_venue;
    for (const auto& set : sData->bars) {
        EXPECT_GE(set.bars.size(), 29U) << set.venue << ' ' << set.symbol;
        ++sets_per_venue[set.venue];
    }
    // 2 symbols x {1m, 1h, 1d} per venue.
    for (const char* venue : kestrel::testing::kFixtureVenues) {
        EXPECT_EQ(sets_per_venue[venue], 6U) << venue;
    }
    EXPECT_GT(sData->quotes.size(), 100U);
}

TEST_F(FixtureDataTest, BarsRoundTripThroughTheDatabase) {
    for (const auto& set : sData->bars) {
        auto loaded = mBars->Load(set.venue, set.symbol, set.timeframe,
                                  Epoch(), Epoch() + kForever);
        ASSERT_OK(loaded);
        ASSERT_EQ(loaded->size(), set.bars.size())
            << set.venue << ' ' << set.symbol << ' '
            << types::ToString(set.timeframe);
        for (std::size_t i = 0; i < loaded->size(); ++i) {
            const auto& got = (*loaded)[i];
            const auto& want = set.bars[i];
            EXPECT_EQ(got.open_time, want.open_time);
            EXPECT_EQ(got.open, want.open);
            EXPECT_EQ(got.high, want.high);
            EXPECT_EQ(got.low, want.low);
            EXPECT_EQ(got.close, want.close);
            EXPECT_EQ(got.volume, want.volume);
        }
    }
}

TEST_F(FixtureDataTest, BarsAreSaneAndEvenlySpaced) {
    for (const auto& set : sData->bars) {
        const auto length = *types::FixedDuration(set.timeframe);
        for (std::size_t i = 0; i < set.bars.size(); ++i) {
            const auto& b = set.bars[i];
            EXPECT_LE(b.low, b.open) << set.venue << ' ' << set.symbol;
            EXPECT_LE(b.low, b.close);
            EXPECT_GE(b.high, b.open);
            EXPECT_GE(b.high, b.close);
            if (i > 0) {
                // Quiet minutes can be missing; time must still move forward
                // in whole bars.
                const auto gap = b.open_time - set.bars[i - 1].open_time;
                EXPECT_GT(gap.count(), 0);
                EXPECT_EQ(gap % length, std::chrono::seconds(0));
            }
        }
    }
}

TEST_F(FixtureDataTest, BarLoadRangeIsHalfOpen) {
    const FixtureBars* set = nullptr;
    for (const auto& s : sData->bars) {
        if (s.timeframe == types::Timeframe::kH1) {
            set = &s;
            break;
        }
    }
    ASSERT_NE(set, nullptr);
    ASSERT_GE(set->bars.size(), 10U);
    const auto start = set->bars[3].open_time;
    const auto end = set->bars[8].open_time;
    auto loaded = mBars->Load(set->venue, set->symbol, set->timeframe, start,
                              end);
    ASSERT_OK(loaded);
    ASSERT_EQ(loaded->size(), 5U);  // bars 3..7; the bar opening at `end` is out
    EXPECT_EQ(loaded->front().open_time, start);
    EXPECT_LT(loaded->back().open_time, end);
}

TEST_F(FixtureDataTest, ReSavingBarsReplacesInsteadOfDuplicating) {
    const auto& set = sData->bars.front();
    auto again = mBars->Save(set.venue, set.symbol, set.timeframe, set.bars);
    ASSERT_OK(again);
    auto loaded = mBars->Load(set.venue, set.symbol, set.timeframe, Epoch(),
                              Epoch() + kForever);
    ASSERT_OK(loaded);
    EXPECT_EQ(loaded->size(), set.bars.size());
}

TEST_F(FixtureDataTest, VenuesAndSymbolsAreKeptApart) {
    auto missing = mBars->Load("fixture-kraken", "DOGE/USD",
                               types::Timeframe::kH1, Epoch(),
                               Epoch() + kForever);
    ASSERT_OK(missing);
    EXPECT_TRUE(missing->empty());
}

TEST_F(FixtureDataTest, QuotesRoundTripInArrivalOrder) {
    std::map<std::pair<std::string, types::Symbol>,
             std::vector<const types::Quote*>>
        expected;
    for (const auto& q : sData->quotes) {
        expected[{q.venue, q.symbol}].push_back(&q);
    }
    ASSERT_GE(expected.size(), 4U);
    for (const auto& [key, want] : expected) {
        auto loaded =
            mQuotes->Load(key.first, key.second, Epoch(), Epoch() + kForever);
        ASSERT_OK(loaded);
        ASSERT_EQ(loaded->size(), want.size()) << key.first << ' ' << key.second;
        for (std::size_t i = 0; i < loaded->size(); ++i) {
            const auto& got = (*loaded)[i];
            EXPECT_EQ(got.seq, want[i]->seq);
            EXPECT_EQ(got.received_time, want[i]->received_time);
            EXPECT_EQ(got.exchange_time, want[i]->exchange_time);
            EXPECT_EQ(got.bid, want[i]->bid);
            EXPECT_EQ(got.bid_size, want[i]->bid_size);
            EXPECT_EQ(got.ask, want[i]->ask);
            EXPECT_EQ(got.ask_size, want[i]->ask_size);
            EXPECT_EQ(got.last, want[i]->last);
            EXPECT_EQ(got.last_size, want[i]->last_size);
            if (i > 0) {
                EXPECT_GT(got.seq, (*loaded)[i - 1].seq);
            }
        }
        auto last = mQuotes->LastSeq(key.first, key.second);
        ASSERT_OK(last);
        EXPECT_EQ(*last, want.back()->seq);
    }
}

TEST_F(FixtureDataTest, QuoteWriterContinuesNumberingAfterStoredQuotes) {
    const auto& first = sData->quotes.front();
    auto before = mQuotes->LastSeq(first.venue, first.symbol);
    ASSERT_OK(before);
    ASSERT_GT(*before, 0);
    {
        db::QuoteWriter writer(mDatabase);
        types::Quote q = first;
        q.seq = 0;
        q.received_time = types::Now();
        writer.Push(q);
        writer.Flush();
    }
    auto after = mQuotes->LastSeq(first.venue, first.symbol);
    ASSERT_OK(after);
    EXPECT_EQ(*after, *before + 1);
}

}  // namespace
