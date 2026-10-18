#pragma once

#include <chrono>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace kestrel::types {

enum class ErrorCategory {
    kInvalidArgument,  // caller supplied something unusable
    kNetwork,          // connection failure, DNS, TLS
    kTimeout,
    kRateLimit,  // provider told us to slow down
    kAuth,       // bad/expired credentials or signature
    kRejected,   // provider refused the request
    kInsufficientFunds,
    kNotFound,        // unknown symbol / order
    kUnsupported,     // provider can't do this (e.g. unsupported timeframe)
    kParse,           // malformed provider response
    kServer,          // provider-side failure (5xx, "service unavailable")
    kNotImplemented,  // stubbed provider functionality
    kInternal,
};

std::string_view ToString(ErrorCategory category);

struct Error {
    ErrorCategory category = ErrorCategory::kInternal;
    std::string message;
    std::optional<int>
        http_status;  // when the failure came from an HTTP response
    std::optional<std::string>
        provider_code;  // e.g. "EAPI:Rate limit exceeded"
    std::optional<std::chrono::milliseconds>
        retry_after;  // provider's back-off hint

    // Failures that are worth retrying with backoff.
    bool IsTransient() const;
    std::string ToString() const;
};

template <typename T>
using Result = std::expected<T, Error>;

inline std::unexpected<Error> MakeError(
    ErrorCategory category, std::string message,
    std::optional<int> http_status = std::nullopt,
    std::optional<std::string> provider_code = std::nullopt) {
    return std::unexpected(Error{category, std::move(message), http_status,
                                 std::move(provider_code), std::nullopt});
}

}  // namespace kestrel::types
