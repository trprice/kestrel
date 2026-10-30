#include "kestrel/trading/binanceus_api.hpp"

#include <spdlog/spdlog.h>

#include "kestrel/data/binanceus_errors.hpp"
#include "kestrel/util/hmac.hpp"
#include "kestrel/util/json.hpp"
#include "kestrel/util/url.hpp"

namespace kestrel::trading {

using nlohmann::json;
using types::ErrorCategory;
using types::MakeError;
using types::Result;

namespace {

constexpr const char* kTimeWeightPath = "/api/v3/time";

bool IsClockError(const types::Error& error) {
    return error.provider_code && *error.provider_code == "-1021";
}

std::string Plain(const BinanceUSApi::Params& params) {
    std::string out;
    for (const auto& [key, value] : params) {
        if (!out.empty()) {
            out += '&';
        }
        out += util::UrlEncode(key) + '=' + util::UrlEncode(value);
    }
    return out;
}

}  // namespace

BinanceUSApi::BinanceUSApi(config::BinanceUSCredentials credentials,
                           std::shared_ptr<net::HttpClient> http,
                           Options options)
    : mCredentials(std::move(credentials)),
      mHttp(std::move(http)),
      mOptions(std::move(options)),
      mLimiter(mOptions.weight_per_second, mOptions.weight_burst) {}

std::string BinanceUSApi::SignedQuery(const Params& params,
                                      std::int64_t timestamp_ms) const {
    Params all = params;
    all.emplace_back("recvWindow",
                     std::to_string(mOptions.recv_window.count()));
    all.emplace_back("timestamp", std::to_string(timestamp_ms));
    std::string query = Plain(all);
    const auto signature =
        util::HexEncode(util::HmacSha256(mCredentials.api_secret, query));
    return query + "&signature=" + signature;
}

void BinanceUSApi::Acquire(int weight) {
    for (int i = 0; i < weight; ++i) {
        mLimiter.Acquire();
    }
}

Result<void> BinanceUSApi::CheckBan() {
    std::lock_guard lock(mBanMutex);
    if (!mBanned) {
        return {};
    }
    const auto now = mOptions.steady();
    if (now >= mBannedUntil) {
        mBanned = false;
        spdlog::warn("binanceus: the IP ban has ended; resuming requests");
        return {};
    }
    const auto left =
        std::chrono::duration_cast<std::chrono::seconds>(mBannedUntil - now);
    types::Error error;
    error.category = ErrorCategory::kRateLimit;
    error.message =
        "binanceus: IP banned (HTTP 418); not sending requests for " +
        std::to_string(left.count()) + " more seconds";
    return std::unexpected(std::move(error));
}

Result<std::int64_t> BinanceUSApi::SyncClock() {
    const auto before = mOptions.clock();
    auto body = SendOnce(net::HttpMethod::kGet, kTimeWeightPath, {}, 1, false);
    const auto after = mOptions.clock();
    if (!body) {
        return std::unexpected(body.error());
    }
    if (!body->is_object() || !body->contains("serverTime") ||
        !(*body)["serverTime"].is_number_integer()) {
        return MakeError(ErrorCategory::kParse,
                         "binanceus time: no serverTime in the response");
    }
    const std::int64_t server = (*body)["serverTime"].get<std::int64_t>();
    // The server stamped its answer about halfway through the round trip.
    const std::int64_t local =
        (types::ToEpochMillis(before) + types::ToEpochMillis(after)) / 2;
    mOffsetMs = server - local;
    mSynced = true;
    spdlog::info("binanceus: server clock is {} ms {} this machine's",
                 std::abs(mOffsetMs.load()),
                 mOffsetMs >= 0 ? "ahead of" : "behind");
    return mOffsetMs.load();
}

Result<json> BinanceUSApi::SendOnce(net::HttpMethod method,
                                    const std::string& path,
                                    const Params& params, int weight,
                                    bool sign) {
    if (auto ok = CheckBan(); !ok) {
        return std::unexpected(ok.error());
    }
    Acquire(weight);

    net::HttpRequest request;
    request.method = method;
    request.url = mOptions.rest_base_url + path;
    if (sign) {
        const std::int64_t now =
            types::ToEpochMillis(mOptions.clock()) + mOffsetMs.load();
        request.raw_query = SignedQuery(params, now);
        request.headers["X-MBX-APIKEY"] = mCredentials.api_key;
    } else {
        request.raw_query = Plain(params);
    }

    auto response = mHttp->Send(request);
    if (!response) {
        auto error = data::RefineBinanceUSError(response.error());
        if (error.http_status == 418) {
            const auto ban = error.retry_after.value_or(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    mOptions.default_ban));
            {
                std::lock_guard lock(mBanMutex);
                mBanned = true;
                mBannedUntil = mOptions.steady() + ban;
            }
            spdlog::error(
                "binanceus: IP banned (HTTP 418) for {} s; no requests until "
                "then",
                std::chrono::duration_cast<std::chrono::seconds>(ban).count());
        } else if (error.category == ErrorCategory::kRateLimit) {
            mLimiter.Penalize();
        }
        spdlog::debug("binanceus {} {}: {}", path,
                      error.provider_code.value_or(""), error.ToString());
        return std::unexpected(std::move(error));
    }
    return util::ParseJson(response->body);
}

Result<json> BinanceUSApi::Send(net::HttpMethod method, const std::string& path,
                                const Params& params, int weight,
                                bool idempotent, bool sign) {
    if (sign && !mSynced) {
        std::lock_guard lock(mSyncMutex);
        if (!mSynced) {
            if (auto synced = SyncClock(); !synced) {
                // Carry on with no offset: -1021 will say if it matters.
                spdlog::warn("binanceus: cannot read the server clock: {}",
                             synced.error().ToString());
            }
        }
    }

    auto attempt = [&]() -> Result<json> {
        auto result = SendOnce(method, path, params, weight, sign);
        if (!result && sign && IsClockError(result.error())) {
            spdlog::warn(
                "binanceus: the server refused the timestamp; resyncing the "
                "clock and sending the request again");
            if (auto synced = SyncClock(); synced) {
                result = SendOnce(method, path, params, weight, sign);
            }
        }
        return result;
    };

    if (!idempotent) {
        return attempt();
    }
    return util::Retry(mOptions.retry, attempt, mOptions.sleeper);
}

Result<json> BinanceUSApi::Signed(net::HttpMethod method,
                                  const std::string& path, const Params& params,
                                  int weight, bool idempotent) {
    return Send(method, path, params, weight, idempotent, true);
}

Result<json> BinanceUSApi::Public(const std::string& path, const Params& params,
                                  int weight) {
    return Send(net::HttpMethod::kGet, path, params, weight, true, false);
}

}  // namespace kestrel::trading
