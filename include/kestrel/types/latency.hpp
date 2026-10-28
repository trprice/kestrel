#pragma once

#include <cstdint>
#include <string>

namespace kestrel::types {

// One kind of call's response times over a window, in milliseconds. The
// percentiles may come from the most recent samples only (see
// net::LatencyStats); count, errors, min and max cover every call in the
// window.
struct EndpointLatency {
    // What was timed, e.g. "GET https://host/path" (without the query string)
    // for an HTTP call.
    std::string endpoint;
    std::uint64_t count = 0;
    std::uint64_t errors = 0;        // calls that returned an error
    std::uint64_t rate_limited = 0;  // of those, refused for the rate limit
    double min_ms = 0;
    double p50_ms = 0;
    double p95_ms = 0;
    double p99_ms = 0;
    double max_ms = 0;
};

}  // namespace kestrel::types
