#pragma once

#include <deque>
#include <map>
#include <string>
#include <vector>

#include "kestrel/net/http_client.hpp"
#include "kestrel/trading/binanceus_api.hpp"

namespace kestrel::testing {

// Answers by URL path from a script, and keeps every request it saw.
class RoutedHttp : public net::HttpClient {
   public:
    void Push(const std::string& path, std::string body) {
        mScript[path].push_back(net::HttpResponse{200, std::move(body), {}});
    }
    // An HTTP failure, classified the way the real client does.
    void PushStatus(const std::string& path, int status, std::string body,
                    std::map<std::string, std::string> headers = {}) {
        mScript[path].push_back(std::unexpected(net::ErrorFromHttpStatus(
            net::HttpResponse{status, std::move(body), std::move(headers)})));
    }
    void PushError(const std::string& path, types::Error error) {
        mScript[path].push_back(std::unexpected(std::move(error)));
    }

    types::Result<net::HttpResponse> Send(const net::HttpRequest& r) override {
        requests.push_back(r);
        const auto path = r.url.substr(r.url.find('/', 8));
        auto it = mScript.find(path);
        if (it == mScript.end() || it->second.empty()) {
            return types::MakeError(types::ErrorCategory::kInternal,
                                    "unscripted request to " + path);
        }
        auto next = std::move(it->second.front());
        it->second.pop_front();
        return next;
    }

    // The requests that went to `path`.
    std::vector<net::HttpRequest> To(const std::string& path) const {
        std::vector<net::HttpRequest> out;
        for (const auto& r : requests) {
            if (r.url.size() >= path.size() &&
                r.url.compare(r.url.size() - path.size(), path.size(), path) ==
                    0) {
                out.push_back(r);
            }
        }
        return out;
    }

    std::vector<net::HttpRequest> requests;

   private:
    std::map<std::string, std::deque<types::Result<net::HttpResponse>>> mScript;
};

// The value of one parameter in a raw query string, or "" if absent.
inline std::string QueryValue(const std::string& query,
                              const std::string& key) {
    const std::string needle = key + "=";
    std::size_t at = 0;
    while (at < query.size()) {
        const auto end = std::min(query.find('&', at), query.size());
        if (query.compare(at, needle.size(), needle) == 0) {
            return query.substr(at + needle.size(), end - at - needle.size());
        }
        at = end + 1;
    }
    return {};
}

}  // namespace kestrel::testing
