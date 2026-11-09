#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>

#include "kestrel/data/data_source.hpp"
#include "kestrel/data/schwab_auth.hpp"
#include "kestrel/data/schwab_streamer.hpp"
#include "kestrel/types/market_hours.hpp"
#include "kestrel/util/retry.hpp"

namespace kestrel::data {

// The US equity calendar (types::UsEquityCalendar) with Schwab's word on the
// days it answers for: GET /markets?markets=equity&date=..., which accepts a
// date up to 7 days back and a year ahead. Each date from 6 days back to 364
// ahead that the rules open is asked about once, on a background thread, and
// the answer remembered; until it is in (and outside that window, and while
// Schwab cannot be asked: a failure is retried after 10 minutes), the rules
// answer. So no caller ever waits on Schwab.
//
// Schwab can only restrict a day: close it, or move its opens later and its
// closes earlier (an unscheduled closure or a delayed open), each time taken
// from whichever is more restrictive; a difference is logged once. It cannot
// open a day the rules close, so those are never asked about: Schwab fills in
// a year's holidays late (in Nov 2026 every 2027 holiday read as a normal
// day). Thread-safe. Destroying it waits for a request already under way
// (up to the HTTP timeout), so a shutdown can take that long.
class SchwabCalendar : public types::TradingCalendar {
   public:
    SchwabCalendar(std::shared_ptr<SchwabApi> api, std::string base_url,
                   std::function<types::Timestamp()> clock = types::Now);
    ~SchwabCalendar() override;

    std::optional<types::TradingDay> On(
        std::chrono::year_month_day date) const override;
    std::chrono::year_month_day DateAt(types::Timestamp t) const override;

    // Waits until every date asked about so far has been answered (or has
    // failed). For tests.
    void WaitForRequests() const;

    // A /markets answer for equities: the day's sessions, or nullopt when
    // Schwab says it is closed. kParse if the answer has neither, or a time
    // in it is malformed (a pre- or after-market session that is absent is
    // taken as none).
    static types::Result<std::optional<types::TradingDay>> ParseMarketHours(
        std::string_view body);

   private:
    // Asks Schwab about one date; not under mMutex.
    types::Result<std::optional<types::TradingDay>> Ask(
        std::chrono::year_month_day date) const;
    // The background thread: answers the queued dates.
    void Work();

    const std::shared_ptr<SchwabApi> mApi;
    const std::string mBaseUrl;
    const std::function<types::Timestamp()> mClock;
    const types::UsEquityCalendar mRules;

    mutable std::mutex mMutex;              // guards the members below
    mutable std::condition_variable mWake;  // a date queued, or stopping
    mutable std::condition_variable mIdle;  // the queue drained
    mutable std::map<std::chrono::year_month_day,
                     std::optional<types::TradingDay>>
        mDays;  // answered
    mutable std::deque<std::chrono::year_month_day> mQueue;
    mutable std::set<std::chrono::year_month_day> mQueued;  // or being asked
    // Held back while mRetryAt is ahead, and queued once it has passed.
    std::set<std::chrono::year_month_day> mDeferred;
    mutable types::Timestamp mRetryAt{};  // after a failed request
    bool mStopping = false;

    std::thread mWorker;  // last: started once the members above exist
};

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
// GetSymbolInfo: Schwab publishes no tick or lot size, so US equity rules,
// checked against a quote (GET /quotes, once per symbol, then cached): an
// unknown symbol is kNotFound, anything but a stock or ETF kUnsupported. The
// tick is 0.01, or 0.0001 for a stock under $1 at the time it is asked; lot
// and minimum 1 share.
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
// GetBars, only bars in the regular session (by the calendar below, so an
// early close ends at 13:00) are given. An unknown symbol gets no bars and no
// error.
//
// Hours: the US equity market's, from SchwabCalendar: the regular session,
// or with extended_hours also 07:00 to 09:30 and the close to 20:00 (Schwab's
// pre- and after-market). The overnight session's bars come with
// extended_hours, but it does not count as open: orders are refused then.
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
    std::shared_ptr<const types::MarketHours> Hours() const override {
        return mHours;
    }

   private:
    // The symbols as Schwab keys.
    types::Result<std::vector<std::string>> Keys(
        const std::vector<std::string>& symbols) const;

    std::shared_ptr<SchwabApi> mApi;
    Options mOptions;
    std::shared_ptr<SchwabStreamer> mStreamer;
    std::mutex mSymbolMutex;  // guards mSymbols
    std::map<types::Symbol, types::SymbolInfo> mSymbols;
    std::shared_ptr<const types::MarketHours> mHours;
    // The regular session alone, for the bar stream's filter.
    std::shared_ptr<const types::MarketHours> mRegular;
};

}  // namespace kestrel::data
