#pragma once

#include <memory>

#include "kestrel/data/data_source.hpp"
#include "kestrel/data/schwab_auth.hpp"

namespace kestrel::data {

// Schwab price-history parameters for a timeframe.
struct SchwabFrequency {
    std::string_view
        frequency_type;  // "minute" | "daily" | "weekly" | "monthly"
    int frequency;       // 1, 5, 10, 15, 30 for minute; 1 otherwise
};
types::Result<SchwabFrequency> SchwabFrequencyFor(types::Timeframe timeframe);

// STUB. Not yet functional; every call returns kNotImplemented. The
// authorization is done: requests go through SchwabApi, which adds the access
// token (renewing it as needed) and keeps to the rate limit.
//
// TODO(schwab): implement against the Schwab Trader API "Market Data" product.
//   Base URL: https://api.schwabapi.com/marketdata/v1
//   Auth:     SchwabApi::Send (see SchwabAuth and `kestrel_app authorize`).
//   GetBars:  GET /pricehistory?symbol=&periodType=&frequencyType=&frequency=
//             &startDate=<epoch ms>&endDate=<epoch ms>&needExtendedHoursData=
//             Response: {"candles":[{"open","high","low","close","volume",
//             "datetime"(epoch ms)}], "empty":bool}. Drop the forming candle.
//   Quotes:   GET /quotes?symbols=AAPL,MSFT&fields=quote for snapshots.
//   Stream:   GET https://api.schwabapi.com/trader/v1/userPreference returns
//             `streamerInfo` (socket URL, customerId, correlId, channel,
//             functionId). Open that WebSocket, send a LOGIN command with the
//             access token, then SUBS to LEVELONE_EQUITIES (fields: 0 symbol,
//             1 bid, 2 ask, 3 last, 4 bid size, 5 ask size, 9 last size, 35
//             quote time). Updates are deltas, so merge them.
//   SymbolInfo: Schwab has no instrument-metadata endpoint for tick/lot size;
//             US equities use tick 0.01 (0.0001 under $1), lot 1, min 1.
//   Limits:   ~120 market-data requests/minute per app (SchwabApi's limiter).
class SchwabDataSource : public DataSource {
   public:
    explicit SchwabDataSource(std::shared_ptr<SchwabApi> api)
        : mApi(std::move(api)) {}

    std::string_view Name() const override { return "schwab"; }
    types::Result<std::vector<types::Bar>> GetBars(const types::Symbol&,
                                                   types::Timeframe,
                                                   types::Timestamp,
                                                   types::Timestamp) override;
    types::Result<std::unique_ptr<Subscription>> StreamQuotes(
        const std::vector<std::string>&, QuoteCallback) override;
    types::Result<types::SymbolInfo> GetSymbolInfo(
        const types::Symbol&) override;

   private:
    std::shared_ptr<SchwabApi> mApi;
};

}  // namespace kestrel::data
