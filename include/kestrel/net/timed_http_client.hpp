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

namespace kestrel::net {

// One endpoint's response times over a window, in milliseconds. The
// percentiles come from the most recent samples kept for the endpoint (see
// LatencyStats); count, errors, min and max cover every call in the window.
struct EndpointLatency {
    std::string endpoint;  // "GET https://host/path", without the query string
    std::uint64_t count = 0;
    std::uint64_t errors = 0;        // calls that returned an error
    std::uint64_t rate_limited = 0;  // of those, HTTP 429 or 418
    double min_ms = 0;
    double p50_ms = 0;
    double p95_ms = 0;
    double p99_ms = 0;
    double max_ms = 0;
};

// Thread-safe response-time statistics per endpoint. Record() adds one call;
// Summarize() reports the window since the last reset and can start a new one.
class LatencyStats {
   public:
    // `max_samples` is how many recent samples per endpoint the percentiles
    // are worked out from; the rest of a longer window is still counted.
    explicit LatencyStats(std::size_t max_samples = 4096);

    void Record(const std::string& endpoint, std::chrono::microseconds elapsed,
                bool ok, int http_status);

    // Endpoints with at least one call, sorted by name.
    std::vector<EndpointLatency> Summarize(bool reset);

   private:
    struct Series {
        std::vector<double> samples;  // ring buffer once full
        std::size_t next = 0;
        EndpointLatency totals;
    };

    const std::size_t mMaxSamples;
    std::mutex mMutex;
    std::map<std::string, Series> mSeries;
};

// Logs one info line per endpoint (and nothing if there were no calls).
void LogLatency(const std::vector<EndpointLatency>& summary);

// Times every call of the client it wraps, and warns when one is slow. The
// time is the whole Send(): connect, request and response, for one attempt, so
// a call that the caller retries shows up as several samples. The endpoint is
// the method and the URL without its query string, so signatures and
// parameters never reach the log or the statistics.
class TimedHttpClient : public HttpClient {
   public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    TimedHttpClient(std::shared_ptr<HttpClient> inner,
                    std::shared_ptr<LatencyStats> stats,
                    std::chrono::milliseconds slow_threshold =
                        std::chrono::milliseconds(2000),
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

// A CprHttpClient wrapped in a TimedHttpClient that reports to HttpLatency().
std::shared_ptr<HttpClient> MakeTimedHttpClient();

}  // namespace kestrel::net
