#pragma once

#include <functional>
#include <memory>
#include <string>

#include "kestrel/data/data_source.hpp"
#include "kestrel/data/schwab_auth.hpp"
#include "kestrel/data/schwab_streamer.hpp"
#include "kestrel/util/retry.hpp"

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
// StreamQuotes: LEVELONE_EQUITIES on Schwab's streamer, over one connection
// that every StreamQuotes of this data source shares (see SchwabStreamer:
// Schwab allows one streamer connection per user, so two processes streaming
// Schwab quotes at once cannot both work). Schwab sends a symbol's changes
// about once a second, conflated. Quotes carry bid, ask, last, their sizes in
// shares, and the exchange time; not the taker's side.
//
// StreamBars: CHART_EQUITY on the same connection. Schwab streams closed
// 1-minute bars only, each about 2 s after its minute; 5, 10, 15 and 30
// minutes (the intraday timeframes GetBars has) are built from them (see
// SchwabBarAggregator). A minute with no trades has no bar, so a longer bar
// whose last minute had none closes with the next trade. The first longer
// bar after a (re)connect is not given if minutes of it were missed; the
// runner backfills it with GetBars. Unless extended_hours is set, as for
// GetBars, only bars in the regular session (09:30 to 16:00 New York time)
// are given. An unknown symbol gets no bars and no error.
class SchwabDataSource : public DataSource {
   public:
    struct Options {
        bool extended_hours = false;
        std::string base_url = "https://api.schwabapi.com/marketdata/v1";
        // For userPreference, which gives the streamer's address.
        std::string trader_url = "https://api.schwabapi.com/trader/v1";
        SchwabStreamer::SocketFactory make_socket = MakeSchwabStreamerSocket;
        // See SchwabStreamer: zero never reconnects after a takeover.
        std::chrono::milliseconds takeover_reconnect{0};
        std::function<types::Timestamp()> clock = types::Now;
        // Each price-history page is retried on a transient failure (a
        // timeout, 429 or 5xx), honouring Retry-After.
        util::RetryPolicy retry;
        util::Sleeper sleeper = util::DefaultSleep;
    };

    explicit SchwabDataSource(std::shared_ptr<SchwabApi> api)
        : SchwabDataSource(std::move(api), Options{}) {}
    SchwabDataSource(std::shared_ptr<SchwabApi> api, Options options);

    std::string_view Name() const override { return "schwab"; }
    types::Result<std::vector<types::Bar>> GetBars(
        const types::Symbol& symbol, types::Timeframe timeframe,
        types::Timestamp start, types::Timestamp end) override;
    types::Result<std::unique_ptr<Subscription>> StreamQuotes(
        const std::vector<std::string>& symbols,
        QuoteCallback callback) override;
    types::Result<std::unique_ptr<Subscription>> StreamBars(
        const std::vector<std::string>& symbols, types::Timeframe timeframe,
        BarCallback callback) override;
    types::Result<types::SymbolInfo> GetSymbolInfo(
        const types::Symbol& symbol) override;

   private:
    // The symbols as Schwab keys.
    types::Result<std::vector<std::string>> Keys(
        const std::vector<std::string>& symbols) const;

    std::shared_ptr<SchwabApi> mApi;
    Options mOptions;
    std::shared_ptr<SchwabStreamer> mStreamer;
};

}  // namespace kestrel::data
