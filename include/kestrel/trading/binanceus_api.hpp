#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

#include "kestrel/config/credentials.hpp"
#include "kestrel/net/http_client.hpp"
#include "kestrel/types/time.hpp"
#include "kestrel/util/rate_limiter.hpp"
#include "kestrel/util/retry.hpp"

namespace kestrel::trading {

// Signed REST access to Binance.US: the part of the trading venue that knows
// about keys, signatures, the server clock and the request-weight budget.
//
// A signed request carries the API key in X-MBX-APIKEY, plus `timestamp`,
// `recvWindow` and `signature` = hex(HMAC-SHA256(secret, query string)). The
// query is built here, encoded, in a fixed order, and sent verbatim
// (HttpRequest::raw_query) so the bytes that were signed are the bytes sent.
//
// The timestamp is the local clock corrected by an offset measured against
// GET /api/v3/time, taken before the first signed call and again when the
// server answers -1021 (timestamp outside recvWindow); that request is then
// sent once more, which is safe because the server rejected it unexecuted.
//
// Calls draw their request weight from a token bucket kept well below
// Binance.US's 6000 per minute (per IP, shared with the market-data client),
// blocking when it is empty. HTTP 418 means the IP is banned:
// calls then fail at once, without touching the network, until the
// Retry-After the server gave has passed.
//
// Retries: a call marked idempotent (a read) is retried with backoff on
// transient failures. A write is never retried here, because after a timeout
// the order may or may not exist; the caller has to look it up.
class BinanceUSApi {
   public:
    struct Options {
        std::string rest_base_url = "https://api.binance.us";
        std::chrono::milliseconds recv_window{5000};
        util::RetryPolicy retry;
        double weight_per_second = 40.0;  // 2400 per minute of the 6000
        double weight_burst = 100.0;
        // New orders, which Binance.US also counts on their own: 100 per 10
        // seconds and 200,000 a day. Used by BinanceUSTradingVenue.
        double orders_per_second = 9.0;
        double order_burst = 10.0;
        // Used when a 418 carries no Retry-After.
        std::chrono::seconds default_ban{120};
        std::function<types::Timestamp()> clock = types::Now;
        std::function<std::chrono::steady_clock::time_point()> steady =
            std::chrono::steady_clock::now;
        util::Sleeper sleeper = util::DefaultSleep;
    };

    // Parameters are sent in the order given.
    using Params = std::vector<std::pair<std::string, std::string>>;

    BinanceUSApi(config::BinanceUSCredentials credentials,
                 std::shared_ptr<net::HttpClient> http, Options options);

    // A signed request. `weight` is the endpoint's request weight.
    types::Result<nlohmann::json> Signed(net::HttpMethod method,
                                         const std::string& path,
                                         const Params& params, int weight,
                                         bool idempotent);

    // An unsigned request to the same host.
    types::Result<nlohmann::json> Public(const std::string& path,
                                         const Params& params, int weight);

    // Measures the server clock offset now; Signed() does this itself when it
    // needs to. Returns the offset in milliseconds (server minus local).
    types::Result<std::int64_t> SyncClock();

    // The offset in use, zero before the first sync.
    std::int64_t ClockOffsetMs() const { return mOffsetMs; }

    // The query string a signed request would carry for these parameters at
    // this timestamp, including the signature. Exposed for tests.
    std::string SignedQuery(const Params& params,
                            std::int64_t timestamp_ms) const;

   private:
    types::Result<nlohmann::json> Send(net::HttpMethod method,
                                       const std::string& path,
                                       const Params& params, int weight,
                                       bool idempotent, bool sign);
    types::Result<nlohmann::json> SendOnce(net::HttpMethod method,
                                           const std::string& path,
                                           const Params& params, int weight,
                                           bool sign);
    void Acquire(int weight);
    // An error if a 418 ban is still in force.
    types::Result<void> CheckBan();

    config::BinanceUSCredentials mCredentials;
    std::shared_ptr<net::HttpClient> mHttp;
    Options mOptions;
    util::RateLimiter mLimiter;

    std::atomic<std::int64_t> mOffsetMs{0};
    std::atomic<bool> mSynced{false};
    std::mutex mSyncMutex;

    std::mutex mBanMutex;
    std::chrono::steady_clock::time_point mBannedUntil{};
    bool mBanned = false;
};

}  // namespace kestrel::trading
