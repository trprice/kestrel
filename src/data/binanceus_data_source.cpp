#include "kestrel/data/binanceus_data_source.hpp"

namespace kestrel::data {

using types::ErrorCategory;
using types::MakeError;

types::Result<std::string_view> BinanceUSInterval(types::Timeframe tf) {
    using types::Timeframe;
    switch (tf) {
        case Timeframe::kM1:
            return "1m";
        case Timeframe::kM3:
            return "3m";
        case Timeframe::kM5:
            return "5m";
        case Timeframe::kM15:
            return "15m";
        case Timeframe::kM30:
            return "30m";
        case Timeframe::kH1:
            return "1h";
        case Timeframe::kH2:
            return "2h";
        case Timeframe::kH4:
            return "4h";
        case Timeframe::kH6:
            return "6h";
        case Timeframe::kH8:
            return "8h";
        case Timeframe::kH12:
            return "12h";
        case Timeframe::kD1:
            return "1d";
        case Timeframe::kD3:
            return "3d";
        case Timeframe::kW1:
            return "1w";
        case Timeframe::kMo1:
            return "1M";
        default:
            return MakeError(ErrorCategory::kUnsupported,
                             "Binance.US does not offer " +
                                 std::string(types::ToString(tf)) + " bars");
    }
}

types::Result<std::vector<types::Bar>> BinanceUSDataSource::GetBars(
    const types::Symbol&, types::Timeframe tf, types::Timestamp,
    types::Timestamp) {
    if (auto i = BinanceUSInterval(tf); !i) {
        return std::unexpected(i.error());
    }
    return MakeError(ErrorCategory::kNotImplemented,
                     "BinanceUSDataSource::GetBars is not implemented yet");
}

types::Result<std::unique_ptr<Subscription>> BinanceUSDataSource::StreamQuotes(
    const std::vector<std::string>&, QuoteCallback) {
    return MakeError(
        ErrorCategory::kNotImplemented,
        "BinanceUSDataSource::StreamQuotes is not implemented yet");
}

types::Result<types::SymbolInfo> BinanceUSDataSource::GetSymbolInfo(
    const types::Symbol&) {
    return MakeError(
        ErrorCategory::kNotImplemented,
        "BinanceUSDataSource::GetSymbolInfo is not implemented yet");
}

}  // namespace kestrel::data
