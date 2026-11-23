#include "kestrel/data/schwab_auth.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <format>
#include <nlohmann/json.hpp>

#include "kestrel/util/hmac.hpp"
#include "kestrel/util/url.hpp"

namespace kestrel::data {

using types::ErrorCategory;
using types::MakeError;
using types::Result;

namespace {

types::Error AuthError(std::string message) {
    return MakeError(ErrorCategory::kAuth, std::move(message)).error();
}

// The value of `key` in a query string, or nullopt.
std::optional<std::string_view> QueryParam(std::string_view query,
                                           std::string_view key) {
    std::size_t at = 0;
    while (at <= query.size()) {
        const auto end = std::min(query.find('&', at), query.size());
        const auto field = query.substr(at, end - at);
        if (field.size() > key.size() && field.substr(0, key.size()) == key &&
            field[key.size()] == '=') {
            return field.substr(key.size() + 1);
        }
        at = end + 1;
    }
    return std::nullopt;
}

// Whether a failed token request says the refresh token itself is no good,
// rather than the request (a malformed form, an unknown client, a hiccup).
// OAuth says `invalid_grant`; Schwab has also been seen to answer
// `refresh_token_authentication_error`, at times wrapped in another error
// whose description mentions the "refresh token". The message holds the start
// of the response body.
bool RefusesTheRefreshToken(const types::Error& error) {
    if (error.http_status != 400 && error.http_status != 401) {
        return false;
    }
    const auto& m = error.message;
    return m.find("invalid_grant") != std::string::npos ||
           m.find("refresh_token") != std::string::npos ||
           m.find("refresh token") != std::string::npos;
}

// A refusal by the token endpoint (a 4xx other than a rate limit or a
// timeout): retrying will not change it.
bool IsRefusal(const types::Error& error) {
    return error.http_status && *error.http_status >= 400 &&
           *error.http_status < 500 && *error.http_status != 408 &&
           *error.http_status != 429;
}

std::string_view Trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    return s;
}

}  // namespace

std::string SchwabAuthorizeUrl(std::string_view app_key,
                               std::string_view redirect_uri) {
    return std::string(kSchwabAuthorizeUrl) +
           "?client_id=" + util::UrlEncode(app_key) +
           "&redirect_uri=" + util::UrlEncode(redirect_uri);
}

Result<std::string> SchwabCodeFromRedirect(std::string_view pasted) {
    pasted = Trim(pasted);
    if (pasted.empty()) {
        return MakeError(ErrorCategory::kInvalidArgument, "nothing was pasted");
    }
    const auto question = pasted.find('?');
    if (question == std::string_view::npos) {
        if (pasted.find("://") != std::string_view::npos) {
            return MakeError(ErrorCategory::kInvalidArgument,
                             "the URL has no query, so no code; paste the "
                             "whole address the browser was sent to");
        }
        return util::PercentDecode(pasted);  // a bare code
    }
    auto query = pasted.substr(question + 1);
    query = query.substr(0, query.find('#'));
    if (auto error = QueryParam(query, "error")) {
        return MakeError(
            ErrorCategory::kInvalidArgument,
            "Schwab did not authorize the app: " + util::UrlDecode(*error));
    }
    const auto code = QueryParam(query, "code");
    if (!code || code->empty()) {
        return MakeError(ErrorCategory::kInvalidArgument,
                         "the URL has no code in it; paste the whole address "
                         "the browser was sent to");
    }
    return util::PercentDecode(*code);
}

SchwabAuth::SchwabAuth(config::SchwabCredentials credentials,
                       config::SchwabTokenStore store,
                       std::shared_ptr<net::HttpClient> http,
                       std::string redirect_uri, Clock clock,
                       std::string token_url)
    : mCredentials(std::move(credentials)),
      mStore(std::move(store)),
      mHttp(std::move(http)),
      mRedirectUri(std::move(redirect_uri)),
      mClock(std::move(clock)),
      mTokenUrl(std::move(token_url)) {}

Result<void> SchwabAuth::Load() {
    auto tokens = mStore.Load();
    if (!tokens) {
        config::SchwabTokenStore::LogReauthorizationInstructions();
        return std::unexpected(
            AuthError("no usable Schwab tokens: " + tokens.error().message));
    }
    if (config::SchwabTokenStore::Check(*tokens, mClock()) ==
        config::SchwabTokenHealth::kReauthorizationRequired) {
        config::SchwabTokenStore::LogReauthorizationInstructions();
        return std::unexpected(AuthError(
            "the Schwab refresh token has expired; re-authorization required"));
    }
    std::lock_guard lock(mMutex);
    Adopt(std::move(*tokens));
    NoteExpiry(mClock());
    return {};
}

Result<void> SchwabAuth::Authorize(std::string_view code) {
    const auto now = mClock();
    auto tokens = RequestTokens({{"grant_type", "authorization_code"},
                                 {"code", std::string(code)},
                                 {"redirect_uri", mRedirectUri}},
                                "", now + kSchwabRefreshLifetime, now);
    if (!tokens) {
        return std::unexpected(tokens.error());
    }
    std::lock_guard lock(mMutex);
    Adopt(std::move(*tokens));
    if (auto saved = mStore.Save(*mTokens); !saved) {
        mSavePending = true;
        mRotated = true;
        mNextSave = now + kRetryInterval;
        return std::unexpected(saved.error());
    }
    return {};
}

Result<config::SchwabTokens> SchwabAuth::RequestTokens(
    const std::map<std::string, std::string>& form,
    const std::string& old_refresh_token, types::Timestamp refresh_expires_at,
    types::Timestamp now) {
    net::HttpRequest request;
    request.method = net::HttpMethod::kPost;
    request.url = mTokenUrl;
    request.headers["Authorization"] =
        "Basic " + util::Base64Encode(mCredentials.app_key + ":" +
                                      mCredentials.app_secret);
    request.headers["Content-Type"] = "application/x-www-form-urlencoded";
    request.body = util::FormEncode(form);
    auto response = mHttp->Send(request);
    if (!response) {
        return std::unexpected(response.error());
    }
    // The body holds the tokens, so it never goes into an error message.
    nlohmann::json body;
    try {
        body = nlohmann::json::parse(response->body);
    } catch (const nlohmann::json::exception&) {
        return MakeError(ErrorCategory::kParse,
                         "Schwab's token response is not JSON");
    }
    const auto text = [&body](const char* key) -> std::string {
        const auto it = body.find(key);
        return it != body.end() && it->is_string() ? it->get<std::string>()
                                                   : std::string();
    };
    config::SchwabTokens tokens;
    tokens.access_token = text("access_token");
    if (tokens.access_token.empty()) {
        return MakeError(ErrorCategory::kParse,
                         "Schwab's token response has no access_token");
    }
    tokens.refresh_token = text("refresh_token");
    if (tokens.refresh_token.empty()) {
        tokens.refresh_token = old_refresh_token;
    }
    if (tokens.refresh_token.empty()) {
        return MakeError(ErrorCategory::kParse,
                         "Schwab's token response has no refresh_token");
    }
    // Schwab's access tokens last 30 minutes. Without a usable expires_in the
    // tokens are still taken, not refused: a refused reply may hold a rotated
    // refresh token that would then be lost. If the guess is too long, a 401
    // renews the token anyway.
    std::int64_t expires_in = 1800;
    if (const auto it = body.find("expires_in");
        it != body.end() && it->is_number() && it->get<double>() > 0) {
        expires_in = static_cast<std::int64_t>(it->get<double>());
    } else {
        spdlog::warn(
            "Schwab's token response has no usable expires_in; assuming {} s",
            expires_in);
    }
    tokens.access_expires_at = now + std::chrono::seconds(expires_in);
    tokens.refresh_expires_at = refresh_expires_at;
    return tokens;
}

Result<void> SchwabAuth::Refresh(types::Timestamp now) {
    auto tokens =
        RequestTokens({{"grant_type", "refresh_token"},
                       {"refresh_token", mTokens->refresh_token}},
                      mTokens->refresh_token, mTokens->refresh_expires_at, now);
    if (!tokens) {
        const auto& error = tokens.error();
        if (RefusesTheRefreshToken(error)) {
            mRefreshRefused = true;
            config::SchwabTokenStore::LogReauthorizationInstructions();
            return std::unexpected(
                AuthError("Schwab refused the refresh token (" + error.message +
                          "); re-authorization required"));
        }
        return std::unexpected(error);
    }
    mRotated = mRotated || tokens->refresh_token != mTokens->refresh_token;
    mTokens = std::move(*tokens);
    mSavePending = true;
    Save(now);
    config::SchwabTokenStore::Check(*mTokens, now);  // warns near the deadline
    return {};
}

void SchwabAuth::Adopt(config::SchwabTokens tokens) {
    mTokens = std::move(tokens);
    mRefreshRefused = false;
    mRefusalLogged = false;
    mLastError.reset();
    mNextAttempt = {};
    mSavePending = false;
    mRotated = false;
    mNextSave = {};
}

void SchwabAuth::AdoptFromFile(config::SchwabTokens tokens) {
    spdlog::info(
        "{} holds usable Schwab tokens from a newer save (refresh token lasts "
        "until {}); using them",
        mStore.Path().string(),
        types::FormatIso8601(tokens.refresh_expires_at));
    Adopt(std::move(tokens));
}

bool SchwabAuth::AdoptNewerAuthorization(types::Timestamp now) {
    auto file = mStore.Load();
    if (!file || file->refresh_expires_at <= now) {
        return false;
    }
    const bool newer_auth =
        file->refresh_expires_at > mTokens->refresh_expires_at;
    const bool rotated_sibling =
        mRefreshRefused &&
        file->refresh_expires_at == mTokens->refresh_expires_at &&
        file->refresh_token != mTokens->refresh_token;
    if (!newer_auth && !rotated_sibling) {
        return false;
    }
    AdoptFromFile(std::move(*file));
    return true;
}

void SchwabAuth::Save(types::Timestamp now) {
    auto saved = mStore.SaveUnlessSuperseded(*mTokens);
    if (saved && saved->has_value()) {
        // Re-authorized while this process ran: the renewal just made belongs
        // to the older authorization, so the file's tokens take its place.
        AdoptFromFile(std::move(**saved));
        return;
    }
    if (saved) {
        if (mNextSave != types::Timestamp{}) {
            spdlog::info("saved the renewed Schwab tokens to {}",
                         mStore.Path().string());
        }
        mSavePending = false;
        mRotated = false;
        mNextSave = {};
        return;
    }
    mNextSave = now + kRetryInterval;
    if (mRotated) {
        // Schwab may no longer accept the refresh token in the file, so if
        // the process stops before a save succeeds, only re-authorizing helps.
        spdlog::error(
            "could not save the renewed Schwab tokens ({}), and Schwab issued "
            "a new refresh token: the token file's may no longer work. "
            "Retrying every {} s; if Kestrel stops first, run `kestrel_app "
            "authorize`",
            saved.error().ToString(), kRetryInterval.count());
    } else {
        spdlog::warn(
            "could not save the renewed Schwab tokens ({}); the token file's "
            "refresh token still works. Retrying every {} s",
            saved.error().ToString(), kRetryInterval.count());
    }
}

void SchwabAuth::NoteExpiry(types::Timestamp now) {
    const auto deadline = mTokens->refresh_expires_at;
    if (now >= deadline) {
        if (mExpiredNotedFor != deadline) {
            mExpiredNotedFor = deadline;
            mNearNotedFor = deadline;
            spdlog::error(
                "the Schwab login expired at {}: no Schwab data or trading "
                "until you run `kestrel_app authorize`",
                types::FormatIso8601(deadline));
        }
        return;
    }
    if (deadline - now < kExpiryNotice && mNearNotedFor != deadline) {
        mNearNotedFor = deadline;
        const auto left = deadline - now;
        const auto hours = std::chrono::duration_cast<std::chrono::hours>(left);
        const auto minutes = std::max<std::int64_t>(
            1, std::chrono::duration_cast<std::chrono::minutes>(left).count());
        spdlog::error(
            "the Schwab login expires in {} (at {}): run `kestrel_app "
            "authorize` before then, or Schwab data and trading stop",
            hours.count() > 0 ? std::format("{} h", hours.count())
                              : std::format("{} min", minutes),
            types::FormatIso8601(deadline));
    }
}

Result<std::string> SchwabAuth::AccessToken() {
    std::lock_guard lock(mMutex);
    if (!mTokens) {
        return std::unexpected(
            AuthError("no Schwab tokens; run `kestrel_app authorize`"));
    }
    const auto now = mClock();
    if ((mRefreshRefused || now >= mTokens->refresh_expires_at) &&
        !AdoptNewerAuthorization(now)) {
        if (!mRefreshRefused) {
            NoteExpiry(now);
        }
        return std::unexpected(AuthError(
            "the Schwab refresh token is no longer valid; re-authorization "
            "required"));
    }
    // NoteExpiry runs only after any adoption or renewal, and never once
    // Schwab has refused the login, so neither a superseded deadline nor an
    // already-refused login is reported as "expires in N h".
    const auto finish = [&]() -> Result<std::string> {
        NoteExpiry(now);
        return mTokens->access_token;
    };
    if (mSavePending && now >= mNextSave) {
        Save(now);
    }
    bool checked_file = false;
    if (now + kRefreshMargin < mTokens->access_expires_at) {
        // Before reporting a near-expiry login while its access token is still
        // cached, check once whether `authorize` already wrote a newer one.
        if (now < mTokens->refresh_expires_at &&
            mTokens->refresh_expires_at - now < kExpiryNotice &&
            mNearNotedFor != mTokens->refresh_expires_at) {
            checked_file = true;
            AdoptNewerAuthorization(now);
        }
        if (now + kRefreshMargin < mTokens->access_expires_at) {
            return finish();
        }
    }
    // Before renewing, take up a re-authorization made since this process
    // read the file, whose access token may well still be good.
    if (!checked_file && AdoptNewerAuthorization(now) &&
        now + kRefreshMargin < mTokens->access_expires_at) {
        return finish();
    }
    for (int pass = 0; pass < 2; ++pass) {
        const bool usable = now < mTokens->access_expires_at;
        if (now < mNextAttempt) {
            if (usable) {
                return finish();
            }
            if (!mRefreshRefused && !mRefusalLogged) {
                NoteExpiry(now);
            }
            return std::unexpected(*mLastError);
        }
        const auto prev_deadline = mTokens->refresh_expires_at;
        if (auto refreshed = Refresh(now); !refreshed) {
            mLastError = refreshed.error();
            mNextAttempt = now + kRetryInterval;
            if (!mRefreshRefused && !usable && IsRefusal(refreshed.error())) {
                // Schwab's body for a revoked refresh token has not been seen
                // live, so one RefusesTheRefreshToken does not recognise still
                // ends here once the access token has run out: say what to do,
                // and fail as an authorization error (the streamer waits for
                // re-authorization on it). Renewal is still tried every
                // kRetryInterval.
                if (!mRefusalLogged) {
                    mRefusalLogged = true;
                    spdlog::error(
                        "Schwab keeps refusing to renew the access token ({}); "
                        "the refresh token has probably been revoked (or the "
                        "app key or secret was refused)",
                        refreshed.error().ToString());
                    config::SchwabTokenStore::LogReauthorizationInstructions();
                }
                mLastError =
                    AuthError("Schwab refuses to renew the access token (" +
                              refreshed.error().message +
                              "); re-authorization is probably required");
                return std::unexpected(*mLastError);
            }
            if (mRefreshRefused || !usable) {
                if (!mRefreshRefused && !mRefusalLogged) {
                    NoteExpiry(now);
                }
                return std::unexpected(refreshed.error());
            }
            spdlog::warn(
                "could not renew the Schwab access token ({}); the current one "
                "lasts until {}",
                refreshed.error().ToString(),
                types::FormatIso8601(mTokens->access_expires_at));
            return finish();
        }
        mLastError.reset();
        mNextAttempt = {};
        mRefusalLogged = false;
        // If Save() adopted a newer authorization from the file whose own
        // access token is already due for renewal, renew that one next.
        if (mTokens->refresh_expires_at <= prev_deadline ||
            now + kRefreshMargin < mTokens->access_expires_at) {
            break;
        }
    }
    return finish();
}

void SchwabAuth::Invalidate(std::string_view token) {
    std::lock_guard lock(mMutex);
    if (mTokens && mTokens->access_token == token) {
        mTokens->access_expires_at =
            std::min(mTokens->access_expires_at, mClock());
    }
}

std::optional<types::Timestamp> SchwabAuth::RefreshExpiresAt() const {
    std::lock_guard lock(mMutex);
    if (!mTokens) {
        return std::nullopt;
    }
    return mTokens->refresh_expires_at;
}

SchwabApi::SchwabApi(std::shared_ptr<SchwabAuth> auth,
                     std::shared_ptr<net::HttpClient> http, Options options)
    : mAuth(std::move(auth)),
      mHttp(std::move(http)),
      mLimiter(options.requests_per_second, options.burst) {}

Result<net::HttpResponse> SchwabApi::Send(net::HttpRequest request) {
    for (int attempt = 0;; ++attempt) {
        auto token = mAuth->AccessToken();
        if (!token) {
            return std::unexpected(token.error());
        }
        mLimiter.Acquire();
        request.headers["Authorization"] = "Bearer " + *token;
        auto response = mHttp->Send(request);
        if (response) {
            return response;
        }
        if (response.error().http_status == 429) {
            mLimiter.Penalize();
        }
        if (attempt == 0 && response.error().http_status == 401) {
            mAuth->Invalidate(*token);
            continue;
        }
        return response;
    }
}

}  // namespace kestrel::data
