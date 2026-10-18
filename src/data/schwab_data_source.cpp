#include "kestrel/data/schwab_data_source.hpp"

namespace kestrel::data {

using types::ErrorCategory;
using types::MakeError;

types::Result<SchwabFrequency> SchwabFrequencyFor(types::Timeframe tf) {
    using types::Timeframe;
    switch (tf) {
        case Timeframe::kM1:
            return SchwabFrequency{"minute", 1};
        case Timeframe::kM5:
            return SchwabFrequency{"minute", 5};
        case Timeframe::kM10:
            return SchwabFrequency{"minute", 10};
        case Timeframe::kM15:
            return SchwabFrequency{"minute", 15};
        case Timeframe::kM30:
            return SchwabFrequency{"minute", 30};
        case Timeframe::kD1:
            return SchwabFrequency{"daily", 1};
        case Timeframe::kW1:
            return SchwabFrequency{"weekly", 1};
        case Timeframe::kMo1:
            return SchwabFrequency{"monthly", 1};
        default:
            return MakeError(ErrorCategory::kUnsupported,
                             "Schwab does not offer " +
                                 std::string(types::ToString(tf)) + " bars");
    }
}

types::Result<std::vector<types::Bar>> SchwabDataSource::GetBars(
    const types::Symbol&, types::Timeframe tf, types::Timestamp,
    types::Timestamp) {
    if (auto f = SchwabFrequencyFor(tf); !f) {
        return std::unexpected(f.error());
    }
    return MakeError(ErrorCategory::kNotImplemented,
                     "SchwabDataSource::GetBars is not implemented yet");
}

types::Result<std::unique_ptr<Subscription>> SchwabDataSource::StreamQuotes(
    const std::vector<std::string>&, QuoteCallback) {
    return MakeError(ErrorCategory::kNotImplemented,
                     "SchwabDataSource::StreamQuotes is not implemented yet");
}

types::Result<types::SymbolInfo> SchwabDataSource::GetSymbolInfo(
    const types::Symbol&) {
    return MakeError(ErrorCategory::kNotImplemented,
                     "SchwabDataSource::GetSymbolInfo is not implemented yet");
}

}  // namespace kestrel::data
