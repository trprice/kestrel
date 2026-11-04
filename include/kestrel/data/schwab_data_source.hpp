#pragma once

#include <functional>
#include <memory>
#include <string>

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

// Market data from the Schwab Trader API ("Market Data" product), sent
// through SchwabApi, which adds the access token (renewing it as needed) and
// keeps to the rate limit.
//
// GetBars: GET /marketdata/v1/pricehistory, which answers in whole days
// whatever times are asked for (a request for five minutes gets the day), so
// the bars are trimmed to the range here. Minute bars go ten days to a
// request, daily and longer twenty years. Schwab keeps a limited intraday
// history (in Nov 2026 about 48 days of 1-minute bars, 200 of 5-minute and
// 260 of 30-minute; daily back to 1993 for SPY): older days come back empty,
// and a range that starts before the history is logged. An unknown symbol
// also comes back empty, not as an error. Intraday bars are the regular
// session's unless extended_hours is set (then also pre-market, after hours
// and the overnight session). Daily, weekly and monthly bars are stamped
// midnight US Central at Schwab; here they open at 00:00 UTC of their date.
//
// GetSymbolInfo: Schwab publishes no tick or lot size, so US equity rules:
// tick 0.01, lot 1, minimum 1. Sub-dollar stocks quote in 0.0001, which one
// tick size per symbol cannot say; 0.01 is valid for them too, only coarser.
// The symbol is not checked with Schwab.
//
// TODO(schwab): StreamQuotes on the streamer. GET /trader/v1/userPreference
//   returns `streamerInfo` (socket URL, schwabClientCustomerId, correlId,
//   channel, functionId). Open that WebSocket, send ADMIN LOGIN with the
//   access token and wait for its reply (code 3: token refused; log in again
//   with a renewed one), then SUBS to LEVELONE_EQUITIES. Fields, per the
//   table: 0 symbol, 1 bid, 2 ask, 3 last, 4 bid size and 5 ask size (in lots
//   of 100 shares), 9 last size (shares), 34 quote time, 35 trade time; the
//   document's own example is one off from field 12, so check a live message.
//   Updates carry only changed fields: merge them. One streamer connection
//   per user at a time.
class SchwabDataSource : public DataSource {
   public:
    struct Options {
        bool extended_hours = false;
        std::string base_url = "https://api.schwabapi.com/marketdata/v1";
        std::function<types::Timestamp()> clock = types::Now;
    };

    explicit SchwabDataSource(std::shared_ptr<SchwabApi> api)
        : SchwabDataSource(std::move(api), Options{}) {}
    SchwabDataSource(std::shared_ptr<SchwabApi> api, Options options)
        : mApi(std::move(api)), mOptions(std::move(options)) {}

    std::string_view Name() const override { return "schwab"; }
    types::Result<std::vector<types::Bar>> GetBars(
        const types::Symbol& symbol, types::Timeframe timeframe,
        types::Timestamp start, types::Timestamp end) override;
    types::Result<std::unique_ptr<Subscription>> StreamQuotes(
        const std::vector<std::string>&, QuoteCallback) override;
    types::Result<types::SymbolInfo> GetSymbolInfo(
        const types::Symbol& symbol) override;

   private:
    std::shared_ptr<SchwabApi> mApi;
    Options mOptions;
};

}  // namespace kestrel::data
