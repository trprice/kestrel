#pragma once

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "kestrel/types/error.hpp"

namespace cpr {
class Session;
}

namespace kestrel::net {

enum class HttpMethod { kGet, kPost, kDelete };

struct HttpRequest {
    HttpMethod method = HttpMethod::kGet;
    std::string url;                           // without query string
    std::map<std::string, std::string> query;  // URL-encoded by the client
    // A query string the caller has already encoded, sent verbatim in the
    // order given (a signed request needs its parameters in the order it
    // signed them). Use it instead of `query`, not alongside it. It is part of
    // the request, not of `url`, so timing and logging never show it.
    std::string raw_query;
    std::map<std::string, std::string> headers;
    std::string body;  // sent verbatim for POST
};

struct HttpResponse {
    int status = 0;
    std::string body;
    std::map<std::string, std::string> headers;
};

// Seam between provider code and the network, so providers can be unit-tested
// against canned responses. Implementations return an error for transport
// failures and for non-2xx statuses (classified by ErrorFromHttpStatus).
class HttpClient {
   public:
    virtual ~HttpClient() = default;
    virtual types::Result<HttpResponse> Send(const HttpRequest& request) = 0;
};

// Maps a failed HTTP response to an Error: 429 -> kRateLimit (honouring
// Retry-After), 5xx -> kServer, 401/403 -> kAuth, 404 -> kNotFound, other 4xx
// -> kRejected.
types::Error ErrorFromHttpStatus(const HttpResponse& response);

// libcurl-backed implementation using cpr. Thread-safe.
//
// Sessions are kept and reused, so a request to a host already called goes
// over the open connection and skips DNS, TCP and TLS. On Binance.US, whose
// REST API is served from Tokyo through a CloudFront edge, it also keeps the
// edge's connection to Tokyo warm: a call's median fell from 429 ms to 129 ms
// (2026-10-29, from California). Each Send takes an idle session or makes
// one, so concurrent calls never share one; up to kMaxIdleSessions are kept.
// A session whose request failed in transport is dropped rather than reused.
class CprHttpClient : public HttpClient {
   public:
    static constexpr std::size_t kMaxIdleSessions = 4;

    explicit CprHttpClient(
        std::chrono::milliseconds timeout = std::chrono::seconds(15));
    ~CprHttpClient() override;
    types::Result<HttpResponse> Send(const HttpRequest& request) override;

   private:
    std::unique_ptr<cpr::Session> TakeSession();
    void ReturnSession(std::unique_ptr<cpr::Session> session);

    std::chrono::milliseconds mTimeout;
    std::mutex mMutex;  // guards mIdle
    std::vector<std::unique_ptr<cpr::Session>> mIdle;
};

}  // namespace kestrel::net
