// Backtests that read bars and quotes from the database. Integration tests;
// they run only when KESTREL_TEST_DBNAME is set (see bar_store_test.cpp).

#include "kestrel/backtest/backtest.hpp"
#include "kestrel/strategy/sma_crossover.hpp"
#include "db_fixture.hpp"

namespace {

using namespace kestrel;
using backtest::ReplayDataSource;
using kestrel::testing::D;
using types::ErrorCategory;
using types::Timeframe;

constexpr const char* kVenue = "kestrel-test-backtest";
constexpr const char* kSymbol = "BTC/USD";

types::Timestamp Hour(int h) { return types::FromEpochSeconds(h * 3600); }

types::Bar BarAt(int hour, const char* close) {
    types::Bar bar;
    bar.open_time = Hour(hour);
    bar.open = bar.high = bar.low = bar.close = D(close);
    return bar;
}

std::vector<types::Bar> Bars(int first, int last, const char* close) {
    std::vector<types::Bar> bars;
    for (int h = first; h <= last; ++h) {
        bars.push_back(BarAt(h, close));
    }
    return bars;
}

// A "venue" that serves bars with a close of 1 and counts how often it is
// asked for them, under the name the stored rows use.
class CountingSource : public ReplayDataSource {
   public:
    CountingSource() {
        AddBars(kSymbol, Timeframe::kH1, Bars(0, 7, "1"));
        SetSymbolInfo({kSymbol, D("0.01"), D("0.00000001"), D("0.0001")});
    }
    std::string_view Name() const override { return kVenue; }
    types::Result<std::vector<types::Bar>> GetBars(
        const types::Symbol& symbol, Timeframe timeframe,
        types::Timestamp start, types::Timestamp end) override {
        ++bar_requests;
        return ReplayDataSource::GetBars(symbol, timeframe, start, end);
    }
    int bar_requests = 0;
};

// Like CountingSource, but its bars repeat the first one. Saving a batch with
// two rows for the same bar makes the database reject the whole insert.
class DuplicateBarSource : public CountingSource {
   public:
    types::Result<std::vector<types::Bar>> GetBars(
        const types::Symbol& symbol, Timeframe timeframe,
        types::Timestamp start, types::Timestamp end) override {
        auto bars = CountingSource::GetBars(symbol, timeframe, start, end);
        if (bars && !bars->empty()) {
            bars->push_back(bars->front());
        }
        return bars;
    }
};

// A connection to the test database that cannot see its tables, so every
// BarStore or QuoteStore query on it fails.
std::shared_ptr<db::Database> ConnectWithoutTables() {
    auto database =
        db::Database::Connect(kestrel::testing::TestConnectionOptions());
    EXPECT_TRUE(database.has_value());
    std::shared_ptr<db::Database> shared = std::move(*database);
    EXPECT_TRUE(shared->Query("SET search_path TO pg_catalog").has_value());
    return shared;
}

std::unique_ptr<strategy::SmaCrossover> Sma() {
    strategy::SmaCrossoverParams p;
    p.symbol = kSymbol;
    p.timeframe = Timeframe::kH1;
    p.fast_period = 2;
    p.slow_period = 3;
    p.quantity = D("0.5");
    return std::make_unique<strategy::SmaCrossover>(p);
}

class QuoteOnly : public strategy::Strategy {
   public:
    std::string_view Name() const override { return "quote-only"; }
    strategy::Requirements Needs() const override {
        strategy::Requirements r;
        r.symbols = {kSymbol};
        r.quotes = true;
        return r;
    }
    explicit QuoteOnly(int* count = nullptr) : count(count) {}
    void OnQuote(strategy::Context&, const types::Quote&) override {
        if (count) {
            ++*count;
        }
    }
    int* count;  // outlives the strategy, which RunBacktest destroys
};

class BacktestDbTest : public ::testing::Test {
   protected:
    void SetUp() override {
        if (!kestrel::testing::DatabaseConfigured()) {
            GTEST_SKIP() << "KESTREL_TEST_DBNAME not set";
        }
        auto database = kestrel::testing::ConnectTestDatabase();
        ASSERT_OK(database);
        mDatabase = std::move(*database);
        Clear();
        mBars = std::make_unique<db::BarStore>(mDatabase);
        mQuotes = std::make_unique<db::QuoteStore>(mDatabase);
    }
    void TearDown() override {
        if (mDatabase) {
            Clear();
        }
    }
    void Clear() {
        for (const char* table : {"bars", "quotes"}) {
            (void)mDatabase->Query(
                std::string("DELETE FROM ") + table + " WHERE venue = $1",
                {kVenue});
        }
    }
    backtest::StoredData Stored() { return {mBars.get(), mQuotes.get()}; }

    std::shared_ptr<db::Database> mDatabase;
    std::unique_ptr<db::BarStore> mBars;
    std::unique_ptr<db::QuoteStore> mQuotes;
};

TEST_F(BacktestDbTest, UsesStoredBarsWithoutAskingTheVenue) {
    ASSERT_OK(mBars->Save(kVenue, kSymbol, Timeframe::kH1, Bars(1, 7, "5")));
    CountingSource source;
    // Warmup reaches back to hour 1; the database holds hours 1-7.
    auto loaded = backtest::LoadReplayData(source, Sma()->Needs(), Hour(6),
                                           Hour(8), Stored());
    ASSERT_OK(loaded);
    EXPECT_EQ(source.bar_requests, 0);
    const auto& bars = (*loaded)->Series(kSymbol, Timeframe::kH1);
    ASSERT_EQ(bars.size(), 7u);
    EXPECT_EQ(bars.front().close, D("5"));
    // Symbol info always comes from the venue.
    EXPECT_TRUE((*loaded)->GetSymbolInfo(kSymbol).has_value());
}

TEST_F(BacktestDbTest, FallsBackToTheVenueWhenTheDatabaseIsEmpty) {
    CountingSource source;
    auto loaded = backtest::LoadReplayData(source, Sma()->Needs(), Hour(6),
                                           Hour(8), Stored());
    ASSERT_OK(loaded);
    EXPECT_EQ(source.bar_requests, 1);
    EXPECT_EQ((*loaded)->Series(kSymbol, Timeframe::kH1).front().close, D("1"));
}

TEST_F(BacktestDbTest, SavesVenueBarsSoTheNextRunUsesTheDatabase) {
    CountingSource source;
    auto first = backtest::LoadReplayData(source, Sma()->Needs(), Hour(6),
                                          Hour(8), Stored());
    ASSERT_OK(first);
    EXPECT_EQ(source.bar_requests, 1);
    auto saved = mBars->Load(kVenue, kSymbol, Timeframe::kH1, Hour(0), Hour(8));
    ASSERT_OK(saved);
    EXPECT_EQ(saved->size(), (*first)->Series(kSymbol, Timeframe::kH1).size());

    auto second = backtest::LoadReplayData(source, Sma()->Needs(), Hour(6),
                                           Hour(8), Stored());
    ASSERT_OK(second);
    EXPECT_EQ(source.bar_requests, 1);  // served from the database this time
    EXPECT_EQ((*second)->Series(kSymbol, Timeframe::kH1).size(),
              (*first)->Series(kSymbol, Timeframe::kH1).size());
}

TEST_F(BacktestDbTest, FallsBackWhenTheDatabaseStartsTooLateAndKeepsBoth) {
    ASSERT_OK(mBars->Save(kVenue, kSymbol, Timeframe::kH1, Bars(4, 7, "5")));
    CountingSource source;
    auto loaded = backtest::LoadReplayData(source, Sma()->Needs(), Hour(6),
                                           Hour(8), Stored());
    ASSERT_OK(loaded);
    EXPECT_EQ(source.bar_requests, 1);
    EXPECT_EQ((*loaded)->Series(kSymbol, Timeframe::kH1).front().close, D("1"));
    auto saved = mBars->Load(kVenue, kSymbol, Timeframe::kH1, Hour(0), Hour(8));
    ASSERT_OK(saved);
    ASSERT_EQ(saved->size(), 7u);  // hours 1-7: the warmup window onwards
    EXPECT_EQ(saved->front().open_time, Hour(1));
    EXPECT_EQ(saved->back().close, D("1"));  // the venue's copy replaced 4-7
}

TEST_F(BacktestDbTest, FallsBackWhenTheDatabaseEndsTooEarly) {
    ASSERT_OK(mBars->Save(kVenue, kSymbol, Timeframe::kH1, Bars(1, 4, "5")));
    CountingSource source;
    auto loaded = backtest::LoadReplayData(source, Sma()->Needs(), Hour(6),
                                           Hour(8), Stored());
    ASSERT_OK(loaded);
    EXPECT_EQ(source.bar_requests, 1);
}

TEST_F(BacktestDbTest, FallsBackToTheVenueWhenTheDatabaseCannotBeRead) {
    db::BarStore unreadable(ConnectWithoutTables());
    ASSERT_FALSE(unreadable.Load(kVenue, kSymbol, Timeframe::kH1, Hour(0),
                                 Hour(8)));
    CountingSource source;
    auto loaded = backtest::LoadReplayData(source, Sma()->Needs(), Hour(6),
                                           Hour(8), {&unreadable, nullptr});
    ASSERT_OK(loaded);
    EXPECT_EQ(source.bar_requests, 1);
    EXPECT_EQ((*loaded)->Series(kSymbol, Timeframe::kH1).size(), 7u);
}

TEST_F(BacktestDbTest, LoadsFromTheVenueEvenWhenSavingItsBarsFails) {
    DuplicateBarSource source;
    auto loaded = backtest::LoadReplayData(source, Sma()->Needs(), Hour(6),
                                           Hour(8), Stored());
    ASSERT_OK(loaded);
    EXPECT_EQ(source.bar_requests, 1);
    EXPECT_EQ((*loaded)->Series(kSymbol, Timeframe::kH1).size(), 8u);
    auto saved = mBars->Load(kVenue, kSymbol, Timeframe::kH1, Hour(0), Hour(8));
    ASSERT_OK(saved);
    EXPECT_TRUE(saved->empty());  // the rejected insert wrote nothing
}

TEST_F(BacktestDbTest, IgnoresStoredBarsOfOtherVenues) {
    ASSERT_OK(mBars->Save("kestrel-test-other", kSymbol, Timeframe::kH1,
                          Bars(1, 7, "5")));
    CountingSource source;
    auto loaded = backtest::LoadReplayData(source, Sma()->Needs(), Hour(6),
                                           Hour(8), Stored());
    (void)mDatabase->Query("DELETE FROM bars WHERE venue = $1",
                           {"kestrel-test-other"});
    ASSERT_OK(loaded);
    EXPECT_EQ(source.bar_requests, 1);
}

TEST_F(BacktestDbTest, WithoutAStoreEverythingComesFromTheVenue) {
    CountingSource source;
    auto loaded =
        backtest::LoadReplayData(source, Sma()->Needs(), Hour(6), Hour(8));
    ASSERT_OK(loaded);
    EXPECT_EQ(source.bar_requests, 1);
    auto saved = mBars->Load(kVenue, kSymbol, Timeframe::kH1, Hour(0), Hour(8));
    ASSERT_OK(saved);
    EXPECT_TRUE(saved->empty());  // nothing is written without a store
}

TEST_F(BacktestDbTest, LoadsQuotesFromTheDatabaseAndReplaysThem) {
    std::vector<types::Quote> quotes;
    for (int i = 0; i < 4; ++i) {
        types::Quote q;
        q.venue = kVenue;
        q.symbol = kSymbol;
        q.seq = i + 1;
        q.received_time = Hour(1) + std::chrono::minutes(i * 10);
        q.bid = D("99");
        q.ask = D("101");
        quotes.push_back(q);
    }
    ASSERT_OK(mQuotes->Save(quotes));

    CountingSource source;
    int count = 0;
    auto strategy = std::make_unique<QuoteOnly>(&count);
    // [1:10, 1:30) holds two of the four quotes.
    const auto from = Hour(1) + std::chrono::minutes(10);
    const auto to = Hour(1) + std::chrono::minutes(30);
    auto loaded = backtest::LoadReplayData(source, strategy->Needs(), from, to,
                                           Stored());
    ASSERT_OK(loaded);
    EXPECT_EQ((*loaded)->QuotesFor(kSymbol).size(), 2u);
    EXPECT_EQ(source.bar_requests, 0);  // no bar timeframes were needed

    backtest::BacktestOptions options;
    options.start = from;
    options.end = to;
    auto result = backtest::RunBacktest(*loaded, std::move(strategy), options);
    ASSERT_OK(result);
    EXPECT_EQ(result->quotes_replayed, 2u);
    EXPECT_EQ(count, 2);
}

TEST_F(BacktestDbTest, QuotesAreAnErrorWhenNoneAreStored) {
    CountingSource source;
    QuoteOnly strategy;
    auto none = backtest::LoadReplayData(source, strategy.Needs(), Hour(1),
                                         Hour(2), Stored());
    ASSERT_FALSE(none);
    EXPECT_EQ(none.error().category, ErrorCategory::kNotFound);

    auto no_store =
        backtest::LoadReplayData(source, strategy.Needs(), Hour(1), Hour(2));
    ASSERT_FALSE(no_store);
    EXPECT_EQ(no_store.error().category, ErrorCategory::kNotFound);
}

TEST_F(BacktestDbTest, QuotesThatCannotBeReadAreAnErrorNotAMissingRange) {
    db::QuoteStore unreadable(ConnectWithoutTables());
    CountingSource source;
    QuoteOnly strategy;
    auto loaded = backtest::LoadReplayData(source, strategy.Needs(), Hour(1),
                                           Hour(2), {nullptr, &unreadable});
    ASSERT_FALSE(loaded);
    EXPECT_NE(loaded.error().category, ErrorCategory::kNotFound);
}

}  // namespace
