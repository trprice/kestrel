#pragma once

#include <chrono>
#include <map>
#include <memory>
#include <string>

#include "kestrel/types/error.hpp"

namespace kestrel::net {

enum class HttpMethod { kGet, kPost, kDelete };

struct HttpRequest {
  HttpMethod method = HttpMethod::kGet;
  std::string url;                           // without query string
  std::map<std::string, std::string> query;  // URL-encoded by the client
  std::map<std::string, std::string> headers;
  std::string body;                          // sent verbatim for POST
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

// libcurl-backed implementation using cpr.
class CprHttpClient : public HttpClient {
 public:
  explicit CprHttpClient(std::chrono::milliseconds timeout = std::chrono::seconds(15));
  types::Result<HttpResponse> Send(const HttpRequest& request) override;

 private:
  std::chrono::milliseconds timeout_;
};

}  // namespace kestrel::net
