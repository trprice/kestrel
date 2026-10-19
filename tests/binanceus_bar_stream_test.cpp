#include "kestrel/data/binanceus_bar_stream.hpp"

#include "kestrel/util/json.hpp"
#include "test_helpers.hpp"

namespace {

using namespace kestrel;
using kestrel::testing::D;

std::string KlineMessage(bool closed) {
    return std::string(R"({"stream":"btcusd@kline_1m","data":{"e":"kline",)"
                       R"("E":1700000060001,"s":"BTCUSD","k":{"t":1700000000000,)"
                       R"("T":1700000059999,"s":"BTCUSD","i":"1m","o":"100.10",)"
                       R"("c":"105.50","h":"110.00","l":"90.25","v":"12.5",)"
                       R"("n":42,"x":)") +
           (closed ? "true" : "false") + "}}}";
}

TEST(BinanceUSBarStreamTest, ClosedKlineBecomesBar) {
    auto json = util::ParseJson(KlineMessage(true));
    ASSERT_OK(json);
    const auto parsed = data::ParseBinanceUSClosedKline(*json);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->first, "BTC/USD");
    EXPECT_EQ(types::ToEpochMillis(parsed->second.open_time), 1'700'000'000'000);
    EXPECT_EQ(parsed->second.open, D("100.10"));
    EXPECT_EQ(parsed->second.high, D("110.00"));
    EXPECT_EQ(parsed->second.low, D("90.25"));
    EXPECT_EQ(parsed->second.close, D("105.50"));
    EXPECT_EQ(parsed->second.volume, D("12.5"));
}

TEST(BinanceUSBarStreamTest, FormingKlineIsIgnored) {
    auto json = util::ParseJson(KlineMessage(false));
    ASSERT_OK(json);
    EXPECT_FALSE(data::ParseBinanceUSClosedKline(*json).has_value());
}

TEST(BinanceUSBarStreamTest, OtherMessagesAreIgnored) {
    auto json = util::ParseJson(
        R"({"stream":"btcusd@trade","data":{"e":"trade","s":"BTCUSD"}})");
    ASSERT_OK(json);
    EXPECT_FALSE(data::ParseBinanceUSClosedKline(*json).has_value());
}

TEST(BinanceUSBarStreamTest, BuildsCombinedUrl) {
    EXPECT_EQ(data::BinanceUSBarStream::BuildUrl("wss://h:9443",
                                                 {"BTCUSD", "ETHUSD"}, "5m"),
              "wss://h:9443/stream?streams=btcusd@kline_5m/ethusd@kline_5m");
}

}  // namespace
