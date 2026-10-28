#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "kestrel/net/http_client.hpp"
#include "kestrel/types/latency.hpp"

namespace kestrel::net {

// Thread-safe response-time statistics per endpoint. Record() adds one call.
// Two views are kept independently: Summarize() reports everything since the
// last Summarize(true) (the whole run, for the summary at the end), and
// TakeWindow() reports and clears what has come in since the last TakeWindow()
// (the periodic rollups a run stores).
class LatencyStats {
   public:
    // `max_samples` is how many recent samples per endpoint the percentiles
    // are worked out from; the rest of a longer window is still counted.
    explicit LatencyStats(std::size_t max_samples = 4096);

    // A failed call with HTTP status 429 or 418 also counts as rate limited.
    void Record(const std::string& endpoint, std::chrono::microseconds elapsed,
                bool ok, int http_status);

    // Endpoints with at least one call, sorted by name.
    std::vector<types::EndpointLatency> Summarize(bool reset);

    // The calls since the previous TakeWindow() (or since the start), then
    // starts the next window. Leaves Summarize()'s totals alone.
    std::vector<types::EndpointLatency> TakeWindow();

   private:
    struct Series {
        std::vector<double> samples;  // ring buffer once full
        std::size_t next = 0;
        types::EndpointLatency totals;
    };

    using SeriesMap = std::map<std::string, Series>;

    void Add(SeriesMap& map, const std::string& endpoint, double ms, bool ok,
             int http_status) const;
    static std::vector<types::EndpointLatency> Report(const SeriesMap& map);

    const std::size_t mMaxSamples;
    std::mutex mMutex;
    SeriesMap mSeries;  // since the last Summarize(true)
    SeriesMap mWindow;  // since the last TakeWindow()
};

// The default for the slow-request warning (`[http] slow_request_ms`).
inline constexpr std::chrono::milliseconds kDefaultSlowRequest{2000};

// Logs one info line per endpoint (and nothing if there were no calls).
void LogLatency(const std::vector<types::EndpointLatency>& summary);

// Times every call of the client it wraps, and warns when one is slow. The
// time is the whole Send(): connect, request and response, for one attempt, so
// a call that the caller retries shows up as several samples. The endpoint is
// the method and the URL without its query string, so signatures and
// parameters never reach the log or the statistics.
class TimedHttpClient : public HttpClient {
   public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    TimedHttpClient(
        std::shared_ptr<HttpClient> inner, std::shared_ptr<LatencyStats> stats,
        std::chrono::milliseconds slow_threshold = kDefaultSlowRequest,
        Clock clock = std::chrono::steady_clock::now);

    types::Result<HttpResponse> Send(const HttpRequest& request) override;

   private:
    std::shared_ptr<HttpClient> mInner;
    std::shared_ptr<LatencyStats> mStats;
    std::chrono::milliseconds mSlow;
    Clock mClock;
};

// The process-wide statistics the factory's HTTP clients report to, so a
// command can print them when it ends.
LatencyStats& HttpLatency();

// A CprHttpClient wrapped in a TimedHttpClient that reports to HttpLatency()
// and warns about calls that take `slow_threshold` or longer.
std::shared_ptr<HttpClient> MakeTimedHttpClient(
    std::chrono::milliseconds slow_threshold = kDefaultSlowRequest);

}  // namespace kestrel::net
