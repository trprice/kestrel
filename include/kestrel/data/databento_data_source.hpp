#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "kestrel/data/data_source.hpp"
#include "kestrel/types/market_hours.hpp"

namespace kestrel::data {

// Read-only US equity market-data source for backtests on imported Databento
// tables (`venue = "databento"` by default).
//
// Databento bills per gigabyte, so historical bars are never fetched on a
// backtest cache miss: `GetBars` returns `kNotFound` pointing at `kestrel_app
// databento --store`. Market hours follow Kestrel's US equity calendar
// (`UsEquityCalendar`), with or without extended hours, and symbol rules use
// standard Reg NMS >= $1 increments ($0.01 tick, 1-share lot, 1-share minimum;
// sub-$1 stocks that quote in $0.0001 increments are not distinguished without
// a live quote lookup).
class DatabentoDataSource final : public DataSource {
   public:
    struct Options {
        bool extended_hours = false;
        std::string venue = "databento";
    };

    DatabentoDataSource();
    explicit DatabentoDataSource(Options options);

    std::string_view Name() const override { return mVenue; }

    types::Result<std::vector<types::Bar>> GetBars(
        const types::Symbol& symbol, types::Timeframe timeframe,
        types::Timestamp start, types::Timestamp end) override;

    types::Result<std::unique_ptr<Subscription>> StreamQuotes(
        const std::vector<std::string>& symbols,
        QuoteCallback callback) override;

    types::Result<types::SymbolInfo> GetSymbolInfo(
        const types::Symbol& symbol) override;

    std::shared_ptr<const types::MarketHours> Hours() const override {
        return mHours;
    }

   private:
    std::string mVenue;
    std::shared_ptr<const types::MarketHours> mHours;
};

}  // namespace kestrel::data
