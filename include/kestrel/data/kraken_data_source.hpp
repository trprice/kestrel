#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

#include "kestrel/data/data_source.hpp"
#include "kestrel/net/http_client.hpp"
#include "kestrel/util/rate_limiter.hpp"
#include "kestrel/util/retry.hpp"

namespace kestrel::data {

// Kraken interval in minutes for the OHLC endpoint (1, 5, 15, 30, 60, 240,
// 1440, 10080, 21600), or kUnsupported.
types::Result<int> KrakenInterval(types::Timeframe timeframe);

// Maps a Kraken error string ("EAPI:Rate limit exceeded") to a category.
types::ErrorCategory ClassifyKrakenError(std::string_view code);

// Kraken market data over public REST and the v2 WebSocket API. No API key is
// needed. Public REST is limited to roughly one call per second; calls are
// throttled and transient failures are retried with exponential backoff.
class KrakenDataSource : public DataSource {
 public:
  struct Options {
    std::string rest_base_url = "https://api.kraken.com";
    std::string ws_url = "wss://ws.kraken.com/v2";
    util::RetryPolicy retry;
    double requests_per_second = 1.0;
    double request_burst = 2.0;
    std::function<types::Timestamp()> clock = types::Now;  // injectable for tests
    util::Sleeper sleeper = util::DefaultSleep;
  };

  explicit KrakenDataSource(std::shared_ptr<net::HttpClient> http, Options options);
  explicit KrakenDataSource(std::shared_ptr<net::HttpClient> http);
  KrakenDataSource();  // real HTTP client, default options

  std::string_view Name() const override { return "kraken"; }

  // NOTE: Kraken's OHLC endpoint only retains the most recent 720 bars per
  // interval. A range reaching further back than that returns kUnsupported
  // rather than silently truncated data.
  types::Result<std::vector<types::Bar>> GetBars(const types::Symbol& symbol,
                                                 types::Timeframe timeframe,
                                                 types::Timestamp start,
                                                 types::Timestamp end) override;

  types::Result<std::unique_ptr<Subscription>> StreamQuotes(
      const std::vector<std::string>& symbols, QuoteCallback callback) override;

  types::Result<types::SymbolInfo> GetSymbolInfo(const types::Symbol& symbol) override;

 private:
  // GET /0/public/<method>, returning the "result" object.
  types::Result<nlohmann::json> Call(const std::string& method,
                                     std::map<std::string, std::string> query);

  std::shared_ptr<net::HttpClient> mHttp;
  Options mOptions;
  util::RateLimiter mLimiter;

  std::mutex mCacheMutex;
  std::map<types::Symbol, types::SymbolInfo> mSymbolCache;
};

}  // namespace kestrel::data
