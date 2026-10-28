#include "kestrel/net/timed_http_client.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

namespace kestrel::net {

namespace {

// Nearest-rank percentile of a sorted, non-empty vector.
double Percentile(const std::vector<double>& sorted, double fraction) {
    const auto rank = static_cast<std::size_t>(
        std::ceil(fraction * static_cast<double>(sorted.size())));
    return sorted[std::clamp<std::size_t>(rank, 1, sorted.size()) - 1];
}

std::string MethodName(HttpMethod method) {
    switch (method) {
        case HttpMethod::kGet:
            return "GET";
        case HttpMethod::kPost:
            return "POST";
        case HttpMethod::kDelete:
            return "DELETE";
    }
    return "?";
}

}  // namespace

LatencyStats::LatencyStats(std::size_t max_samples)
    : mMaxSamples(std::max<std::size_t>(max_samples, 1)) {}

void LatencyStats::Add(SeriesMap& map, const std::string& endpoint, double ms,
                       bool ok, int http_status) const {
    Series& series = map[endpoint];
    types::EndpointLatency& totals = series.totals;
    if (totals.count == 0) {
        totals.min_ms = ms;
        totals.max_ms = ms;
    } else {
        totals.min_ms = std::min(totals.min_ms, ms);
        totals.max_ms = std::max(totals.max_ms, ms);
    }
    ++totals.count;
    if (!ok) {
        ++totals.errors;
        if (http_status == 429 || http_status == 418) {
            ++totals.rate_limited;
        }
    }
    if (series.samples.size() < mMaxSamples) {
        series.samples.push_back(ms);
    } else {
        series.samples[series.next] = ms;
        series.next = (series.next + 1) % mMaxSamples;
    }
}

void LatencyStats::Record(const std::string& endpoint,
                          std::chrono::microseconds elapsed, bool ok,
                          int http_status) {
    const double ms =
        std::chrono::duration<double, std::milli>(elapsed).count();
    std::lock_guard lock(mMutex);
    Add(mSeries, endpoint, ms, ok, http_status);
    Add(mWindow, endpoint, ms, ok, http_status);
}

std::vector<types::EndpointLatency> LatencyStats::Report(const SeriesMap& map) {
    std::vector<types::EndpointLatency> out;
    for (const auto& [endpoint, series] : map) {
        if (series.totals.count == 0) {
            continue;
        }
        types::EndpointLatency row = series.totals;
        row.endpoint = endpoint;
        std::vector<double> sorted = series.samples;
        std::ranges::sort(sorted);
        row.p50_ms = Percentile(sorted, 0.50);
        row.p95_ms = Percentile(sorted, 0.95);
        row.p99_ms = Percentile(sorted, 0.99);
        out.push_back(std::move(row));
    }
    return out;
}

std::vector<types::EndpointLatency> LatencyStats::Summarize(bool reset) {
    std::lock_guard lock(mMutex);
    auto out = Report(mSeries);
    if (reset) {
        mSeries.clear();
    }
    return out;
}

std::vector<types::EndpointLatency> LatencyStats::TakeWindow() {
    std::lock_guard lock(mMutex);
    auto out = Report(mWindow);
    mWindow.clear();
    return out;
}

void LogLatency(const std::vector<types::EndpointLatency>& summary) {
    for (const auto& row : summary) {
        spdlog::info(
            "latency {}: {} calls, {} errors ({} rate limited), ms min {:.0f} "
            "p50 {:.0f} p95 {:.0f} p99 {:.0f} max {:.0f}",
            row.endpoint, row.count, row.errors, row.rate_limited, row.min_ms,
            row.p50_ms, row.p95_ms, row.p99_ms, row.max_ms);
    }
}

TimedHttpClient::TimedHttpClient(std::shared_ptr<HttpClient> inner,
                                 std::shared_ptr<LatencyStats> stats,
                                 std::chrono::milliseconds slow_threshold,
                                 Clock clock)
    : mInner(std::move(inner)),
      mStats(std::move(stats)),
      mSlow(slow_threshold),
      mClock(std::move(clock)) {}

types::Result<HttpResponse> TimedHttpClient::Send(const HttpRequest& request) {
    const auto start = mClock();
    auto response = mInner->Send(request);
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::microseconds>(mClock() - start);

    const std::string endpoint = MethodName(request.method) + " " + request.url;
    const bool ok = response.has_value();
    const int status =
        ok ? response->status : response.error().http_status.value_or(0);
    mStats->Record(endpoint, elapsed, ok, status);
    if (elapsed >= mSlow) {
        spdlog::warn(
            "slow request: {} took {} ms ({})", endpoint,
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed)
                .count(),
            ok ? "HTTP " + std::to_string(status)
               : response.error().ToString());
    }
    return response;
}

LatencyStats& HttpLatency() {
    static LatencyStats stats;
    return stats;
}

std::shared_ptr<HttpClient> MakeTimedHttpClient(
    std::chrono::milliseconds slow_threshold) {
    // A non-owning shared_ptr: the statistics live for the whole process.
    return std::make_shared<TimedHttpClient>(
        std::make_shared<CprHttpClient>(),
        std::shared_ptr<LatencyStats>(&HttpLatency(), [](LatencyStats*) {}),
        slow_threshold);
}

}  // namespace kestrel::net
