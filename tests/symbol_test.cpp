#include "kestrel/data/symbols.hpp"
#include "test_helpers.hpp"

namespace {

using namespace kestrel;

TEST(SymbolNormalize, CleansUpInput) {
    EXPECT_EQ(*types::NormalizeSymbol("btc/usd"), "BTC/USD");
    EXPECT_EQ(*types::NormalizeSymbol(" eth-usdt "), "ETH/USDT");
    EXPECT_EQ(*types::NormalizeSymbol("sol_usd"), "SOL/USD");
    EXPECT_EQ(*types::NormalizeSymbol("aapl"), "AAPL");
    EXPECT_EQ(*types::NormalizeSymbol("brk.b"), "BRK.B");
}

TEST(SymbolNormalize, RejectsInvalid) {
    for (const char* bad :
         {"", "/", "BTC/", "/USD", "A/B/C", "BTC USD", "BTC$", "AAPL!"}) {
        EXPECT_FALSE(types::NormalizeSymbol(bad).has_value()) << bad;
    }
}

TEST(SymbolSplit, PairAndEquity) {
    auto pair = types::SplitSymbol("BTC/USD");
    ASSERT_OK(pair);
    EXPECT_EQ(pair->base, "BTC");
    EXPECT_EQ(pair->quote, "USD");
    auto equity = types::SplitSymbol("AAPL");
    ASSERT_OK(equity);
    EXPECT_FALSE(equity->quote.has_value());
}

TEST(KrakenSymbols, ToNative) {
    EXPECT_EQ(*data::ToKrakenSymbol("BTC/USD"), "BTC/USD");
    EXPECT_FALSE(
        data::ToKrakenSymbol("AAPL").has_value());  // Kraken trades pairs
}

TEST(KrakenSymbols, FromEveryNativeForm) {
    for (const char* native : {"XBT/USD", "BTC/USD", "XBTUSD", "XXBTZUSD"}) {
        auto s = data::FromKrakenSymbol(native);
        ASSERT_OK(s);
        EXPECT_EQ(*s, "BTC/USD") << native;
    }
    EXPECT_EQ(*data::FromKrakenSymbol("XETHZEUR"), "ETH/EUR");
    EXPECT_EQ(*data::FromKrakenSymbol("XDG/USD"), "DOGE/USD");
    EXPECT_EQ(*data::FromKrakenSymbol("XDGUSD"), "DOGE/USD");
    EXPECT_EQ(*data::FromKrakenSymbol("SOLUSD"), "SOL/USD");
    EXPECT_EQ(*data::FromKrakenSymbol("ETH/XBT"), "ETH/BTC");
    EXPECT_EQ(*data::FromKrakenSymbol("ADAUSDT"), "ADA/USDT");
    EXPECT_FALSE(data::FromKrakenSymbol("???").has_value());
}

TEST(BinanceUSSymbols, RoundTrip) {
    EXPECT_EQ(*data::ToBinanceUSSymbol("BTC/USD"), "BTCUSD");
    EXPECT_EQ(*data::ToBinanceUSSymbol("ETH/USDT"), "ETHUSDT");
    EXPECT_EQ(*data::FromBinanceUSSymbol("BTCUSD"), "BTC/USD");
    EXPECT_EQ(*data::FromBinanceUSSymbol("ETHUSDT"),
              "ETH/USDT");  // USDT wins over USD
    EXPECT_EQ(*data::FromBinanceUSSymbol("ETHBTC"), "ETH/BTC");
    EXPECT_FALSE(data::ToBinanceUSSymbol("AAPL").has_value());
}

TEST(SchwabSymbols, EquitiesOnly) {
    EXPECT_EQ(*data::ToSchwabSymbol("AAPL"), "AAPL");
    EXPECT_EQ(*data::FromSchwabSymbol("aapl"), "AAPL");
    auto crypto = data::ToSchwabSymbol("BTC/USD");
    ASSERT_FALSE(crypto.has_value());
    EXPECT_EQ(crypto.error().category, types::ErrorCategory::kUnsupported);
}

}  // namespace
