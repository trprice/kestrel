#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>

#include "kestrel/data/data_source.hpp"
#include "kestrel/net/http_client.hpp"
#include "kestrel/util/rate_limiter.hpp"
#include "kestrel/util/retry.hpp"

namespace kestrel::data {

// Binance.US kline interval string ("1m", "4h", "1M", ...), or kUnsupported.
types::Result<std::string_view> BinanceUSInterval(types::Timeframe timeframe);

// Binance.US market data over public REST and the combined WebSocket stream.
// No API key is needed. The request-weight budget is 6000/minute per IP;
// calls are throttled below that and transient failures (HTTP 429, 5xx) are
// retried with exponential backoff. HTTP 418 (IP ban) is not retried.
class BinanceUSDataSource : public DataSource {
   public:
    struct Options {
        std::string rest_base_url = "https://api.binance.us";
        std::string ws_url = "wss://stream.binance.us:9443";
        util::RetryPolicy retry;
        double requests_per_second = 10.0;
        double request_burst = 20.0;
        std::function<types::Timestamp()> clock =
            types::Now;  // injectable for tests
        util::Sleeper sleeper = util::DefaultSleep;
    };

    explicit BinanceUSDataSource(std::shared_ptr<net::HttpClient> http,
                                 Options options);
    explicit BinanceUSDataSource(std::shared_ptr<net::HttpClient> http);
    BinanceUSDataSource();  // real HTTP client, default options

    std::string_view Name() const override { return "binanceus"; }

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

    // From /api/v3/aggTrades, which keeps every symbol's trades from its
    // listing (ZEC/USDT: 2022-01-13). Each request covers at most an hour or
    // 1000 trades, so a day of ZEC/USDT is about 20-40 requests.
    types::Result<std::vector<types::MarketTrade>> GetTrades(
        const types::Symbol& symbol, types::Timestamp start,
        types::Timestamp end) override;

   private:
    // GET <base>/api/v3/<path>, returning the parsed body.
    types::Result<nlohmann::json> Call(
        const std::string& path, std::map<std::string, std::string> query);

    std::shared_ptr<net::HttpClient> mHttp;
    Options mOptions;
    util::RateLimiter mLimiter;

    std::mutex mCacheMutex;
    std::map<types::Symbol, types::SymbolInfo> mSymbolCache;
};

}  // namespace kestrel::data
