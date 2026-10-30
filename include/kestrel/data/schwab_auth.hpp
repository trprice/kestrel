#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "kestrel/config/credentials.hpp"
#include "kestrel/config/schwab_token_store.hpp"
#include "kestrel/net/http_client.hpp"
#include "kestrel/types/error.hpp"
#include "kestrel/types/time.hpp"
#include "kestrel/util/rate_limiter.hpp"

namespace kestrel::data {

inline constexpr std::string_view kSchwabAuthorizeUrl =
    "https://api.schwabapi.com/v1/oauth/authorize";
inline constexpr std::string_view kSchwabTokenUrl =
    "https://api.schwabapi.com/v1/oauth/token";

// How long a refresh token lasts from the authorization that issued it.
// Refreshing the access token does not extend it.
inline constexpr std::chrono::hours kSchwabRefreshLifetime{24 * 7};

// The page to open in a browser to authorize the app. Schwab then sends the
// browser to `redirect_uri` with the authorization code in its query.
std::string SchwabAuthorizeUrl(std::string_view app_key,
                               std::string_view redirect_uri);

// The authorization code in the URL the browser was sent to (or a bare code
// pasted on its own), decoded: Schwab's codes end in an encoded '@'.
// kInvalidArgument if there is none, or the URL carries an `error` instead
// (access was not approved).
types::Result<std::string> SchwabCodeFromRedirect(std::string_view pasted);

// Schwab's OAuth tokens, shared by everything that calls Schwab. Thread-safe.
//
// Authorize() exchanges a code from the browser for the first tokens. After
// that AccessToken() hands out the access token, renewing it with the refresh
// token when it has under kRefreshMargin left, and saves every renewal to the
// token file (retried every kRetryInterval if that fails). One caller renews
// while the others wait, so the token endpoint sees one request per renewal;
// the wait is one token request at most, bounded by the HTTP client's timeout
// (15 s by default). Other failed renewals are retried after kRetryInterval.
// The refresh token's 7-day deadline is kept from the authorization; once it
// passes, or Schwab refuses the refresh token, only a new Authorize() (the
// `authorize` command) helps.
//
// No token, code or secret is ever logged, nor put in an error message.
class SchwabAuth {
   public:
    using Clock = std::function<types::Timestamp()>;

    // Renew the access token this long before it expires.
    static constexpr std::chrono::minutes kRefreshMargin{5};
    // After a renewal fails, wait this long before trying again (the current
    // token is handed out meanwhile while it is still valid).
    static constexpr std::chrono::seconds kRetryInterval{10};

    SchwabAuth(config::SchwabCredentials credentials,
               config::SchwabTokenStore store,
               std::shared_ptr<net::HttpClient> http, std::string redirect_uri,
               Clock clock = types::Now,
               std::string token_url = std::string(kSchwabTokenUrl));

    // Reads the token file. kAuth, with the re-authorization instructions
    // logged, if there is none or its refresh token has expired.
    types::Result<void> Load();

    // Exchanges an authorization code for tokens and saves them.
    types::Result<void> Authorize(std::string_view code);

    // A valid access token, renewed first if needed. kAuth when only
    // re-authorizing can help; the renewal's own error (kNetwork, kServer,
    // ...) when it failed and the current token has expired.
    types::Result<std::string> AccessToken();

    // A request sent with `token` was refused as unauthorized: renew on the
    // next AccessToken(), unless another caller already has, or a renewal
    // failed under kRetryInterval ago.
    void Invalidate(std::string_view token);

    // When the refresh token expires; unset before Load() or Authorize().
    std::optional<types::Timestamp> RefreshExpiresAt() const;

    const std::filesystem::path& TokenFile() const { return mStore.Path(); }

   private:
    // POSTs a form to the token endpoint and returns the new tokens, with the
    // refresh token's deadline set to `refresh_expires_at`.
    types::Result<config::SchwabTokens> RequestTokens(
        const std::map<std::string, std::string>& form,
        const std::string& old_refresh_token,
        types::Timestamp refresh_expires_at, types::Timestamp now);
    types::Result<void> Refresh(types::Timestamp now);  // requires mMutex
    // Saves mTokens; on failure logs and schedules a retry. Requires mMutex.
    void Save(types::Timestamp now);

    const config::SchwabCredentials mCredentials;
    const config::SchwabTokenStore mStore;
    const std::shared_ptr<net::HttpClient> mHttp;
    const std::string mRedirectUri;
    const Clock mClock;
    const std::string mTokenUrl;

    mutable std::mutex mMutex;  // guards the members below
    std::optional<config::SchwabTokens> mTokens;
    bool mRefreshRefused = false;  // Schwab refused the refresh token
    types::Timestamp mNextAttempt{};
    std::optional<types::Error> mLastError;  // of the last failed renewal
    bool mSavePending = false;  // mTokens is newer than the token file
    bool mRotated = false;  // ... and holds a refresh token the file lacks
    types::Timestamp mNextSave{};  // when to retry a failed save
};

// Sends requests to Schwab's API with the access token, within a rate limit.
// Thread-safe. One per API product, since each has its own limit (market data:
// 120 requests a minute per app).
class SchwabApi {
   public:
    struct Options {
        // A little under Schwab's 120 a minute, burst included.
        double requests_per_second = 1.8;
        double burst = 5;
    };

    SchwabApi(std::shared_ptr<SchwabAuth> auth,
              std::shared_ptr<net::HttpClient> http, Options options);

    // Sends `request` with `Authorization: Bearer <token>` once the rate
    // limit allows. If Schwab answers 401, the token is renewed and the
    // request sent once more. A 429 holds back the requests that follow.
    types::Result<net::HttpResponse> Send(net::HttpRequest request);

    SchwabAuth& Auth() { return *mAuth; }

   private:
    std::shared_ptr<SchwabAuth> mAuth;
    std::shared_ptr<net::HttpClient> mHttp;
    util::RateLimiter mLimiter;
};

}  // namespace kestrel::data
