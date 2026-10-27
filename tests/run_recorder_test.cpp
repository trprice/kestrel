#include "kestrel/strategy/run_recorder.hpp"

#include "strategy_fakes.hpp"
#include "test_helpers.hpp"

namespace {

using namespace kestrel;
using kestrel::testing::D;
using kestrel::testing::FakeTradeLog;
using strategy::RunRecorder;
using types::Side;

constexpr const char* kSymbol = "ZEC/USDT";

types::Timestamp At(int seconds) { return types::FromEpochSeconds(seconds); }

types::Fill FillOf(const char* id, Side side, const char* quantity,
                   const char* price, const char* fee = "0") {
    types::Fill f;
    f.fill_id = id;
    f.order_id = "order-" + std::string(id);
    f.symbol = kSymbol;
    f.side = side;
    f.quantity = D(quantity);
    f.price = D(price);
    f.fee = D(fee);
    f.fee_asset = "USDT";
    f.time = At(1);
    return f;
}

struct Fixture {
    explicit Fixture(int interval_seconds = 60)
        : log(std::make_shared<FakeTradeLog>()),
          recorder(log, std::chrono::seconds(interval_seconds)) {
        recorder.Start({}, At(0));
    }
    std::shared_ptr<FakeTradeLog> log;
    RunRecorder recorder;
};

TEST(RunRecorderTest, WritesFillsOnFlush) {
    Fixture f;
    f.recorder.NoteFill(FillOf("a", Side::kBuy, "1", "100"));
    EXPECT_TRUE(f.log->fills.empty());  // queued until the flush
    f.recorder.Flush();
    ASSERT_EQ(f.log->fills.size(), 1u);
    EXPECT_EQ(f.log->fills[0].fill_id, "a");
    f.recorder.Flush();
    EXPECT_EQ(f.log->fill_writes, 1);  // nothing queued, nothing written
}

TEST(RunRecorderTest, KeepsFillsThatCouldNotBeWrittenAndRetries) {
    Fixture f;
    f.log->fail_fills = true;
    f.recorder.NoteFill(FillOf("a", Side::kBuy, "1", "100"));
    f.recorder.Flush();
    f.recorder.NoteFill(FillOf("b", Side::kSell, "1", "101"));
    f.recorder.Flush();
    EXPECT_TRUE(f.log->fills.empty());

    f.log->fail_fills = false;
    f.recorder.Flush();
    ASSERT_EQ(f.log->fills.size(), 2u);
    EXPECT_EQ(f.log->fills[0].fill_id, "a");
    EXPECT_EQ(f.log->fills[1].fill_id, "b");
}

TEST(RunRecorderTest, SnapshotsRealizedAndUnrealizedPnl) {
    Fixture f;
    f.recorder.NoteFill(FillOf("a", Side::kBuy, "2", "100", "0.5"));
    f.recorder.NoteFill(FillOf("b", Side::kSell, "1", "103"));
    f.recorder.NotePrice(kSymbol, D("104"));
    f.recorder.Tick(At(60));
    ASSERT_EQ(f.log->snapshots.size(), 1u);
    EXPECT_EQ(f.log->snapshots[0].first, At(60));
    const auto& row = f.log->snapshots[0].second.at(0);
    EXPECT_EQ(row.symbol, kSymbol);
    EXPECT_EQ(row.quantity, D("1"));
    EXPECT_EQ(row.average_price, D("100"));
    EXPECT_EQ(row.realized, D("2.5"));  // 3 on the sale, 0.5 fee on the buy
    EXPECT_EQ(row.mark_price, D("104"));
    EXPECT_EQ(row.unrealized, D("4"));
}

TEST(RunRecorderTest, SnapshotsOnlyWhenTheIntervalHasPassed) {
    Fixture f;
    f.recorder.NotePrice(kSymbol, D("100"));
    f.recorder.Tick(At(59));
    EXPECT_TRUE(f.log->snapshots.empty());
    f.recorder.Tick(At(60));
    EXPECT_EQ(f.log->snapshots.size(), 1u);
    f.recorder.Tick(At(100));
    EXPECT_EQ(f.log->snapshots.size(), 1u);
    f.recorder.Tick(At(120));
    EXPECT_EQ(f.log->snapshots.size(), 2u);
}

TEST(RunRecorderTest, AZeroIntervalNeverSnapshotsOnATick) {
    Fixture f(0);
    f.recorder.NotePrice(kSymbol, D("100"));
    f.recorder.Tick(At(100000));
    EXPECT_TRUE(f.log->snapshots.empty());
}

TEST(RunRecorderTest, ASymbolWithAPriceButNoFillsStillGetsARow) {
    Fixture f;
    f.recorder.NotePrice(kSymbol, D("100"));
    f.recorder.Tick(At(60));
    ASSERT_EQ(f.log->snapshots.size(), 1u);
    const auto& row = f.log->snapshots[0].second.at(0);
    EXPECT_EQ(row.quantity, D("0"));
    EXPECT_EQ(row.realized, D("0"));
    EXPECT_EQ(row.unrealized, D("0"));
}

TEST(RunRecorderTest, NoMarkMeansNoUnrealizedFigure) {
    Fixture f;
    f.recorder.NoteFill(FillOf("a", Side::kBuy, "1", "100"));
    f.recorder.Tick(At(60));
    ASSERT_EQ(f.log->snapshots.size(), 1u);
    EXPECT_FALSE(f.log->snapshots[0].second.at(0).mark_price.has_value());
    EXPECT_FALSE(f.log->snapshots[0].second.at(0).unrealized.has_value());
}

TEST(RunRecorderTest, TheStartingPositionsAreTheCostBasis) {
    auto log = std::make_shared<FakeTradeLog>();
    RunRecorder recorder(log, std::chrono::seconds(60));
    types::Position held;
    held.symbol = kSymbol;
    held.quantity = D("2");
    held.average_price = D("90");
    recorder.Start({held}, At(0));
    recorder.NoteFill(FillOf("a", Side::kSell, "1", "100"));
    recorder.NotePrice(kSymbol, D("100"));
    recorder.Tick(At(60));
    const auto& row = log->snapshots.at(0).second.at(0);
    EXPECT_EQ(row.realized, D("10"));
    EXPECT_EQ(row.quantity, D("1"));
}

TEST(RunRecorderTest, ASkippedSnapshotDoesNotStopTheNext) {
    Fixture f;
    f.recorder.NotePrice(kSymbol, D("100"));
    f.log->fail_pnl = true;
    f.recorder.Tick(At(60));
    f.log->fail_pnl = false;
    f.recorder.Tick(At(120));
    EXPECT_EQ(f.log->snapshots.size(), 1u);
}

TEST(RunRecorderTest, FinishFlushesSnapshotsAndMarksTheRunDone) {
    Fixture f;
    f.recorder.NoteFill(FillOf("a", Side::kBuy, "1", "100"));
    f.recorder.NotePrice(kSymbol, D("100"));
    f.recorder.Finish(At(5));  // well before the interval
    EXPECT_EQ(f.log->fills.size(), 1u);
    EXPECT_EQ(f.log->snapshots.size(), 1u);
    EXPECT_EQ(f.log->finished, At(5));
}

}  // namespace
