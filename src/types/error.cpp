#include "kestrel/types/error.hpp"

namespace kestrel::types {

std::string_view ToString(ErrorCategory category) {
  switch (category) {
    case ErrorCategory::kInvalidArgument: return "invalid_argument";
    case ErrorCategory::kNetwork: return "network";
    case ErrorCategory::kTimeout: return "timeout";
    case ErrorCategory::kRateLimit: return "rate_limit";
    case ErrorCategory::kAuth: return "auth";
    case ErrorCategory::kRejected: return "rejected";
    case ErrorCategory::kInsufficientFunds: return "insufficient_funds";
    case ErrorCategory::kNotFound: return "not_found";
    case ErrorCategory::kUnsupported: return "unsupported";
    case ErrorCategory::kParse: return "parse";
    case ErrorCategory::kServer: return "server";
    case ErrorCategory::kNotImplemented: return "not_implemented";
    case ErrorCategory::kInternal: return "internal";
  }
  return "unknown";
}

bool Error::IsTransient() const {
  switch (category) {
    case ErrorCategory::kNetwork:
    case ErrorCategory::kTimeout:
    case ErrorCategory::kRateLimit:
    case ErrorCategory::kServer:
      return true;
    default:
      return false;
  }
}

std::string Error::ToString() const {
  std::string out = "[" + std::string(types::ToString(category)) + "] " + message;
  if (http_status) out += " (http " + std::to_string(*http_status) + ")";
  if (provider_code) out += " (code " + *provider_code + ")";
  return out;
}

}  // namespace kestrel::types
