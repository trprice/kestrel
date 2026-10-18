#include <cpr/cpr.h>

#include <algorithm>
#include <cctype>
#include <charconv>

#include "kestrel/net/http_client.hpp"

namespace kestrel::net {

using types::ErrorCategory;

types::Error ErrorFromHttpStatus(const HttpResponse& response) {
    types::Error err;
    err.http_status = response.status;
    std::string snippet = response.body.substr(0, 200);
    err.message = "HTTP " + std::to_string(response.status) + ": " + snippet;
    if (response.status == 429) {
        err.category = ErrorCategory::kRateLimit;
        for (const auto& [k, v] : response.headers) {
            std::string lower = k;
            std::ranges::transform(lower, lower.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            if (lower == "retry-after") {
                long secs = 0;
                std::from_chars(v.data(), v.data() + v.size(), secs);
                if (secs > 0) {
                    err.retry_after = std::chrono::seconds(secs);
                }
            }
        }
    } else if (response.status >= 500) {
        err.category = ErrorCategory::kServer;
    } else if (response.status == 401 || response.status == 403) {
        err.category = ErrorCategory::kAuth;
    } else if (response.status == 404) {
        err.category = ErrorCategory::kNotFound;
    } else {
        err.category = ErrorCategory::kRejected;
    }
    return err;
}

CprHttpClient::CprHttpClient(std::chrono::milliseconds timeout)
    : mTimeout(timeout) {}

types::Result<HttpResponse> CprHttpClient::Send(const HttpRequest& request) {
    cpr::Parameters params;
    for (const auto& [k, v] : request.query) params.Add({k, v});
    cpr::Header headers;
    for (const auto& [k, v] : request.headers) headers[k] = v;

    cpr::Session session;
    session.SetUrl(cpr::Url{request.url});
    session.SetParameters(params);
    session.SetHeader(headers);
    session.SetTimeout(
        cpr::Timeout{static_cast<std::int32_t>(mTimeout.count())});
    if (!request.body.empty()) {
        session.SetBody(cpr::Body{request.body});
    }

    cpr::Response r;
    switch (request.method) {
        case HttpMethod::kGet:
            r = session.Get();
            break;
        case HttpMethod::kPost:
            r = session.Post();
            break;
        case HttpMethod::kDelete:
            r = session.Delete();
            break;
    }

    if (r.error) {
        const auto category = r.error.code == cpr::ErrorCode::OPERATION_TIMEDOUT
                                  ? ErrorCategory::kTimeout
                                  : ErrorCategory::kNetwork;
        return types::MakeError(category, "request to " + request.url +
                                              " failed: " + r.error.message);
    }

    HttpResponse out;
    out.status = static_cast<int>(r.status_code);
    out.body = std::move(r.text);
    for (const auto& [k, v] : r.header) out.headers[k] = v;
    if (out.status < 200 || out.status >= 300) {
        return std::unexpected(ErrorFromHttpStatus(out));
    }
    return out;
}

}  // namespace kestrel::net
