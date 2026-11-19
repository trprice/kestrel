#include "kestrel/data/databento_data_source.hpp"

#include <utility>

namespace kestrel::data {

using types::Decimal;
using types::ErrorCategory;
using types::MakeError;

DatabentoDataSource::DatabentoDataSource() : DatabentoDataSource(Options{}) {}

DatabentoDataSource::DatabentoDataSource(Options options)
    : mVenue(options.venue.empty() ? "databento" : std::move(options.venue)),
      mHours(std::make_shared<const types::MarketHours>(
          std::make_shared<const types::UsEquityCalendar>(),
          options.extended_hours)) {}

types::Result<std::vector<types::Bar>> DatabentoDataSource::GetBars(
    const types::Symbol& symbol, types::Timeframe timeframe,
    types::Timestamp start, types::Timestamp end) {
    if (end < start) {
        return MakeError(ErrorCategory::kInvalidArgument,
                         "GetBars: end precedes start");
    }
    return MakeError(
        ErrorCategory::kNotFound,
        "no stored " + std::string(types::ToString(timeframe)) + " bars for " +
            symbol + " from " + mVenue +
            " in the range; import them first with `kestrel_app databento "
            "--store`");
}

types::Result<std::unique_ptr<Subscription>> DatabentoDataSource::StreamQuotes(
    const std::vector<std::string>&, QuoteCallback) {
    return MakeError(ErrorCategory::kNotImplemented,
                     "databento has no live quote stream in Kestrel; use "
                     "`backtest` on imported data");
}

types::Result<types::SymbolInfo> DatabentoDataSource::GetSymbolInfo(
    const types::Symbol& symbol) {
    auto normalized = types::NormalizeSymbol(symbol);
    if (!normalized) {
        return std::unexpected(normalized.error());
    }
    auto parts = types::SplitSymbol(*normalized);
    if (!parts) {
        return std::unexpected(parts.error());
    }
    if (parts->quote) {
        return MakeError(
            ErrorCategory::kUnsupported,
            "Databento US equities do not trade pairs: " + *normalized);
    }
    return types::SymbolInfo{*normalized, Decimal(1, 2), Decimal::FromInt(1),
                             Decimal::FromInt(1), Decimal()};
}

}  // namespace kestrel::data
