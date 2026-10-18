#include "kestrel/data/symbols.hpp"

#include <algorithm>
#include <array>
#include <string>

namespace kestrel::data {

using types::ErrorCategory;
using types::MakeError;
using types::Result;
using types::Symbol;

namespace {

struct Alias {
    std::string_view normalized;
    std::string_view kraken;
};
constexpr std::array<Alias, 2> kKrakenAliases{
    {{"BTC", "XBT"}, {"DOGE", "XDG"}}};

// Kraken's legacy four-letter codes (X = crypto, Z = fiat).
constexpr std::array<std::string_view, 17> kKrakenLegacy{
    "XETH", "XETC", "XLTC", "XMLN", "XREP", "XXBT", "XXDG", "XXLM", "XXMR",
    "XXRP", "XZEC", "ZAUD", "ZCAD", "ZEUR", "ZGBP", "ZJPY", "ZUSD"};

std::string KrakenAssetToNormalized(std::string asset) {
    for (const auto legacy : kKrakenLegacy) {
        if (asset == legacy) {
            asset.erase(0, 1);
            break;
        }
    }
    for (const auto& a : kKrakenAliases)
        if (asset == a.kraken) {
            return std::string(a.normalized);
        }
    return asset;
}

// Quote assets recognised when splitting concatenated pairs, longest first so
// "USDT" wins over "USD".
constexpr std::array<std::string_view, 17> kQuotes{
    "ZUSD", "ZEUR", "ZGBP", "ZCAD", "ZJPY", "USDT", "USDC", "BUSD", "USD",
    "EUR",  "GBP",  "CAD",  "JPY",  "AUD",  "CHF",  "BTC",  "ETH"};

Result<Symbol> SplitConcatenated(std::string_view native, bool kraken) {
    for (const auto quote : kQuotes) {
        if (native.size() > quote.size() && native.ends_with(quote)) {
            std::string base(native.substr(0, native.size() - quote.size()));
            std::string q(quote);
            if (kraken) {
                base = KrakenAssetToNormalized(base);
                q = KrakenAssetToNormalized(q);
            }
            return types::JoinSymbol(base, q);
        }
    }
    return MakeError(
        ErrorCategory::kInvalidArgument,
        "cannot split native symbol '" + std::string(native) + "'");
}

Result<types::SymbolParts> RequirePair(std::string_view symbol,
                                       std::string_view provider) {
    auto parts = types::SplitSymbol(symbol);
    if (!parts) {
        return std::unexpected(parts.error());
    }
    if (!parts->quote) {
        return MakeError(ErrorCategory::kInvalidArgument,
                         std::string(provider) + " trades pairs; '" +
                             std::string(symbol) + "' has no quote asset");
    }
    return parts;
}

}  // namespace

Result<std::string> ToKrakenSymbol(std::string_view symbol) {
    auto parts = RequirePair(symbol, "Kraken");
    if (!parts) {
        return std::unexpected(parts.error());
    }
    return types::JoinSymbol(parts->base, *parts->quote);
}

Result<Symbol> FromKrakenSymbol(std::string_view native) {
    if (native.find('/') != std::string_view::npos) {
        auto parts = types::SplitSymbol(native);
        if (!parts || !parts->quote) {
            return MakeError(
                ErrorCategory::kInvalidArgument,
                "invalid Kraken symbol '" + std::string(native) + "'");
        }
        return types::JoinSymbol(KrakenAssetToNormalized(parts->base),
                                 KrakenAssetToNormalized(*parts->quote));
    }
    // Legacy "XXBTZUSD": two four-letter codes.
    if (native.size() == 8) {
        const std::string base(native.substr(0, 4)), quote(native.substr(4));
        const auto legacy = [](const std::string& s) {
            return std::ranges::find(kKrakenLegacy, std::string_view(s)) !=
                   kKrakenLegacy.end();
        };
        if (legacy(base) && legacy(quote)) {
            return types::JoinSymbol(KrakenAssetToNormalized(base),
                                     KrakenAssetToNormalized(quote));
        }
    }
    return SplitConcatenated(native, /*kraken=*/true);
}

Result<std::string> ToBinanceUSSymbol(std::string_view symbol) {
    auto parts = RequirePair(symbol, "Binance.US");
    if (!parts) {
        return std::unexpected(parts.error());
    }
    return parts->base + *parts->quote;
}

Result<Symbol> FromBinanceUSSymbol(std::string_view native) {
    return SplitConcatenated(native, /*kraken=*/false);
}

Result<std::string> ToSchwabSymbol(std::string_view symbol) {
    auto parts = types::SplitSymbol(symbol);
    if (!parts) {
        return std::unexpected(parts.error());
    }
    if (parts->quote) {
        return MakeError(
            ErrorCategory::kUnsupported,
            "Schwab does not trade crypto pairs: " + std::string(symbol));
    }
    return std::string(symbol);
}

Result<Symbol> FromSchwabSymbol(std::string_view native) {
    return types::NormalizeSymbol(native);
}

}  // namespace kestrel::data
