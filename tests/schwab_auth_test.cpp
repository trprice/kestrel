#include "kestrel/data/schwab_auth.hpp"

#include <gtest/gtest.h>

#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>

#include "kestrel/config/configuration.hpp"
#include "kestrel/factory.hpp"
#include "kestrel/util/hmac.hpp"
#include "test_helpers.hpp"

namespace kestrel::data {
namespace {

using namespace std::chrono_literals;

// Answers from a script and keeps every request it saw.
class FakeHttp : public net::HttpClient {
   public:
    void Push(std::string body) {
        mScript.push_back(net::HttpResponse{200, std::move(body), {}});
    }
    void PushStatus(int status, std::string body) {
        mScript.push_back(std::unexpected(net::ErrorFromHttpStatus(
            net::HttpResponse{status, std::move(body), {}})));
    }
    void PushError(types::Error e) {
        mScript.push_back(std::unexpected(std::move(e)));
    }
    std::vector<net::HttpRequest> requests;
    // Runs as each request is sent, before its answer (e.g. to have another
    // process write the token file meanwhile).
    std::function<void()> on_send;

    types::Result<net::HttpResponse> Send(const net::HttpRequest& r) override {
        requests.push_back(r);
        if (auto hook = on_send) {
            hook();
        }
        if (mScript.empty()) {
            return types::MakeError(types::ErrorCategory::kInternal,
                                    "unscripted request");
        }
        auto next = std::move(mScript.front());
        mScript.pop_front();
        return next;
    }

   private:
    std::deque<types::Result<net::HttpResponse>> mScript;
};

std::string TokenReply(const std::string& access, const std::string& refresh,
                       int expires_in = 1800) {
    return R"({"expires_in":)" + std::to_string(expires_in) +
           R"(,"token_type":"Bearer","scope":"api","refresh_token":")" +
           refresh + R"(","access_token":")" + access + R"(","id_token":"id"})";
}

// The value of a field in a form body, or "" if absent.
std::string FormValue(const std::string& body, const std::string& key) {
    const std::string needle = key + "=";
    std::size_t at = 0;
    while (at < body.size()) {
        const auto end = std::min(body.find('&', at), body.size());
        if (body.compare(at, needle.size(), needle) == 0) {
            return body.substr(at + needle.size(), end - at - needle.size());
        }
        at = end + 1;
    }
    return {};
}

class SchwabAuthTest : public ::testing::Test {
   protected:
    void SetUp() override {
        mDir = std::filesystem::temp_directory_path() /
               ("kestrel-schwab-auth-" +
                std::string(::testing::UnitTest::GetInstance()
                                ->current_test_info()
                                ->name()));
        std::filesystem::remove_all(mDir);
        std::filesystem::create_directories(mDir);
        mHttp = std::make_shared<FakeHttp>();
    }
    void TearDown() override { std::filesystem::remove_all(mDir); }

    config::SchwabTokenStore Store() const {
        return config::SchwabTokenStore(mDir / "tokens.json");
    }
    std::shared_ptr<SchwabAuth> MakeAuth() {
        return std::make_shared<SchwabAuth>(
            config::SchwabCredentials{"app-key", "app-secret"}, Store(), mHttp,
            "https://127.0.0.1:8182", [this] { return mNow; },
            "https://auth.test/v1/oauth/token");
    }
    // A token file whose access token has `access_left` to go and whose
    // refresh token has `refresh_left`.
    void SaveTokens(std::chrono::seconds access_left,
                    std::chrono::hours refresh_left = 72h) {
        ASSERT_OK(Store().Save({"access-1", "refresh-1", mNow + access_left,
                                mNow + refresh_left}));
    }
    // What `authorize` in another process writes: fresh tokens whose refresh
    // token lasts the full 7 days from now.
    config::SchwabTokens Reauthorized() const {
        return {"access-new", "refresh-new", mNow + 30min,
                mNow + kSchwabRefreshLifetime};
    }

    std::filesystem::path mDir;
    std::shared_ptr<FakeHttp> mHttp;
    types::Timestamp mNow = types::Now();
};

TEST(SchwabAuthorizeUrl, EncodesTheRedirect) {
    EXPECT_EQ(SchwabAuthorizeUrl("KEY", "https://127.0.0.1:8182"),
              "https://api.schwabapi.com/v1/oauth/authorize?client_id=KEY"
              "&redirect_uri=https%3A%2F%2F127.0.0.1%3A8182");
}

TEST(SchwabCodeFromRedirect, ReadsAndDecodesTheCode) {
    auto code = SchwabCodeFromRedirect(
        "  https://127.0.0.1:8182/?code=C0.b2F1dGg%40&session=abc-123\n");
    ASSERT_OK(code);
    EXPECT_EQ(*code, "C0.b2F1dGg@");

    auto bare = SchwabCodeFromRedirect("C0.xyz%40");
    ASSERT_OK(bare);
    EXPECT_EQ(*bare, "C0.xyz@");

    auto plus = SchwabCodeFromRedirect("C0.a+b%2Bc");  // '+' is not a space
    ASSERT_OK(plus);
    EXPECT_EQ(*plus, "C0.a+b+c");

    auto first = SchwabCodeFromRedirect("https://h/?session=s&code=abc#frag");
    ASSERT_OK(first);
    EXPECT_EQ(*first, "abc");
}

TEST(SchwabCodeFromRedirect, RefusesAnAddressWithoutACode) {
    EXPECT_FALSE(SchwabCodeFromRedirect("").has_value());
    EXPECT_FALSE(SchwabCodeFromRedirect("https://127.0.0.1:8182/").has_value());
    EXPECT_FALSE(SchwabCodeFromRedirect("https://127.0.0.1:8182/?session=x")
                     .has_value());
    EXPECT_FALSE(SchwabCodeFromRedirect("https://h/?xcode=abc").has_value());
    auto denied =
        SchwabCodeFromRedirect("https://h/?error=access_denied&state=1");
    ASSERT_FALSE(denied.has_value());
    EXPECT_NE(denied.error().message.find("access_denied"), std::string::npos);
}

TEST_F(SchwabAuthTest, AuthorizeExchangesTheCodeAndSavesTheTokens) {
    auto auth = MakeAuth();
    mHttp->Push(TokenReply("access-1", "refresh-1"));
    ASSERT_OK(auth->Authorize("C0.abc@"));

    ASSERT_EQ(mHttp->requests.size(), 1u);
    const auto& r = mHttp->requests[0];
    EXPECT_EQ(r.method, net::HttpMethod::kPost);
    EXPECT_EQ(r.url, "https://auth.test/v1/oauth/token");
    EXPECT_EQ(r.headers.at("Authorization"),
              "Basic " + util::Base64Encode("app-key:app-secret"));
    EXPECT_EQ(r.headers.at("Content-Type"),
              "application/x-www-form-urlencoded");
    EXPECT_EQ(FormValue(r.body, "grant_type"), "authorization_code");
    EXPECT_EQ(FormValue(r.body, "code"), "C0.abc%40");
    EXPECT_EQ(FormValue(r.body, "redirect_uri"),
              "https%3A%2F%2F127.0.0.1%3A8182");

    auto saved = Store().Load();
    ASSERT_OK(saved);
    EXPECT_EQ(saved->access_token, "access-1");
    EXPECT_EQ(saved->refresh_token, "refresh-1");
    EXPECT_EQ(saved->access_expires_at, mNow + 1800s);
    EXPECT_EQ(saved->refresh_expires_at, mNow + kSchwabRefreshLifetime);
    EXPECT_EQ(auth->RefreshExpiresAt(), mNow + kSchwabRefreshLifetime);

    auto token = auth->AccessToken();
    ASSERT_OK(token);
    EXPECT_EQ(*token, "access-1");
    EXPECT_EQ(mHttp->requests.size(), 1u);  // no renewal needed
}

TEST_F(SchwabAuthTest, AFailedExchangeSavesNothing) {
    auto auth = MakeAuth();
    mHttp->PushStatus(400, R"({"error":"invalid_grant"})");
    auto ok = auth->Authorize("stale");
    ASSERT_FALSE(ok.has_value());
    EXPECT_FALSE(std::filesystem::exists(mDir / "tokens.json"));

    mHttp->Push(R"({"token_type":"Bearer"})");  // no tokens in it
    EXPECT_FALSE(auth->Authorize("code").has_value());
    EXPECT_FALSE(std::filesystem::exists(mDir / "tokens.json"));
}

TEST_F(SchwabAuthTest, LoadFailsWithoutAUsableTokenFile) {
    auto auth = MakeAuth();
    auto missing = auth->Load();
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().category, types::ErrorCategory::kAuth);
    EXPECT_FALSE(auth->AccessToken().has_value());

    SaveTokens(-1h, -1h);  // refresh token expired an hour ago
    auto expired = auth->Load();
    ASSERT_FALSE(expired.has_value());
    EXPECT_EQ(expired.error().category, types::ErrorCategory::kAuth);
    EXPECT_TRUE(mHttp->requests.empty());
}

TEST_F(SchwabAuthTest, RenewsNearExpiryAndKeepsTheRefreshDeadline) {
    SaveTokens(30min);
    auto auth = MakeAuth();
    ASSERT_OK(auth->Load());
    const auto deadline = mNow + 72h;

    mNow += 24min;  // 6 minutes left: still outside the margin
    auto same = auth->AccessToken();
    ASSERT_OK(same);
    EXPECT_EQ(*same, "access-1");
    EXPECT_TRUE(mHttp->requests.empty());

    mNow += 2min;  // 4 minutes left
    mHttp->Push(TokenReply("access-2", "refresh-2"));
    auto renewed = auth->AccessToken();
    ASSERT_OK(renewed);
    EXPECT_EQ(*renewed, "access-2");
    ASSERT_EQ(mHttp->requests.size(), 1u);
    EXPECT_EQ(FormValue(mHttp->requests[0].body, "grant_type"),
              "refresh_token");
    EXPECT_EQ(FormValue(mHttp->requests[0].body, "refresh_token"), "refresh-1");

    auto saved = Store().Load();
    ASSERT_OK(saved);
    EXPECT_EQ(saved->access_token, "access-2");
    EXPECT_EQ(saved->refresh_token, "refresh-2");  // rotated, so saved
    EXPECT_EQ(saved->access_expires_at, mNow + 1800s);
    EXPECT_EQ(saved->refresh_expires_at, deadline);  // not extended
}

TEST_F(SchwabAuthTest, KeepsTheRefreshTokenWhenTheReplyHasNone) {
    SaveTokens(0s);
    auto auth = MakeAuth();
    ASSERT_OK(auth->Load());
    mHttp->Push(R"({"expires_in":1800,"access_token":"access-2"})");
    ASSERT_OK(auth->AccessToken());
    auto saved = Store().Load();
    ASSERT_OK(saved);
    EXPECT_EQ(saved->refresh_token, "refresh-1");
}

TEST_F(SchwabAuthTest, AFailedRenewalUsesTheCurrentTokenAndWaitsToRetry) {
    SaveTokens(3min);
    auto auth = MakeAuth();
    ASSERT_OK(auth->Load());

    mHttp->PushError(
        types::MakeError(types::ErrorCategory::kNetwork, "down").error());
    auto first = auth->AccessToken();
    ASSERT_OK(first);
    EXPECT_EQ(*first, "access-1");  // still valid for 3 minutes
    EXPECT_EQ(mHttp->requests.size(), 1u);

    mNow += 5s;  // within the retry interval: no new attempt
    ASSERT_OK(auth->AccessToken());
    EXPECT_EQ(mHttp->requests.size(), 1u);

    mNow += SchwabAuth::kRetryInterval;
    mHttp->Push(TokenReply("access-2", "refresh-1"));
    auto renewed = auth->AccessToken();
    ASSERT_OK(renewed);
    EXPECT_EQ(*renewed, "access-2");
    EXPECT_EQ(mHttp->requests.size(), 2u);
}

TEST_F(SchwabAuthTest, AnExpiredTokenWhoseRenewalFailsIsAnError) {
    SaveTokens(-1min);
    auto auth = MakeAuth();
    ASSERT_OK(auth->Load());
    mHttp->PushStatus(503, "unavailable");
    auto token = auth->AccessToken();
    ASSERT_FALSE(token.has_value());
    EXPECT_EQ(token.error().category, types::ErrorCategory::kServer);

    // The same error until the retry interval has passed, without a request.
    auto again = auth->AccessToken();
    ASSERT_FALSE(again.has_value());
    EXPECT_EQ(again.error().category, types::ErrorCategory::kServer);
    EXPECT_EQ(mHttp->requests.size(), 1u);
}

TEST_F(SchwabAuthTest, ARefusedRefreshTokenNeedsReauthorization) {
    SaveTokens(1min);
    auto auth = MakeAuth();
    ASSERT_OK(auth->Load());
    mHttp->PushStatus(400, R"({"error":"invalid_grant"})");
    auto token = auth->AccessToken();
    ASSERT_FALSE(token.has_value());
    EXPECT_EQ(token.error().category, types::ErrorCategory::kAuth);

    // Never asked again, even after the retry interval.
    mNow += 1h;
    auto later = auth->AccessToken();
    ASSERT_FALSE(later.has_value());
    EXPECT_EQ(later.error().category, types::ErrorCategory::kAuth);
    EXPECT_EQ(mHttp->requests.size(), 1u);
}

TEST_F(SchwabAuthTest, SchwabsOwnRefusalNeedsReauthorizationToo) {
    SaveTokens(1min);
    auto auth = MakeAuth();
    ASSERT_OK(auth->Load());
    mHttp->PushStatus(
        400, R"({"error":"refresh_token_authentication_error",)"
             R"("error_description":"Failed refresh token authentication"})");
    auto token = auth->AccessToken();
    ASSERT_FALSE(token.has_value());
    EXPECT_EQ(token.error().category, types::ErrorCategory::kAuth);
    mNow += 1h;
    EXPECT_FALSE(auth->AccessToken().has_value());
    EXPECT_EQ(mHttp->requests.size(), 1u);
}

TEST_F(SchwabAuthTest, OtherRefusalsAreRetried) {
    SaveTokens(1min);
    auto auth = MakeAuth();
    ASSERT_OK(auth->Load());
    mHttp->PushStatus(400, R"({"error":"invalid_request"})");
    auto first = auth->AccessToken();
    ASSERT_OK(first);  // the current token still works
    EXPECT_EQ(*first, "access-1");

    mNow += SchwabAuth::kRetryInterval;
    mHttp->PushStatus(401, R"({"error":"invalid_client"})");
    ASSERT_OK(auth->AccessToken());

    mNow += SchwabAuth::kRetryInterval;
    mHttp->Push(TokenReply("access-2", "refresh-1"));
    auto renewed = auth->AccessToken();
    ASSERT_OK(renewed);
    EXPECT_EQ(*renewed, "access-2");
    EXPECT_EQ(mHttp->requests.size(), 3u);
}

// A refusal worded in a way RefusesTheRefreshToken does not recognise: once
// the access token has run out it is an authorization error (the streamer
// waits for re-authorization on one), but renewal is still tried.
TEST_F(SchwabAuthTest, AnUnrecognisedRefusalOnceExpiredIsAnAuthError) {
    SaveTokens(-1min);
    auto auth = MakeAuth();
    ASSERT_OK(auth->Load());
    mHttp->PushStatus(400, R"({"error":"unsupported_token","detail":"x"})");
    auto token = auth->AccessToken();
    ASSERT_FALSE(token.has_value());
    EXPECT_EQ(token.error().category, types::ErrorCategory::kAuth);
    EXPECT_NE(token.error().message.find("re-authorization"),
              std::string::npos);
    // Within the retry interval: the same error, no request.
    auto again = auth->AccessToken();
    ASSERT_FALSE(again.has_value());
    EXPECT_EQ(again.error().category, types::ErrorCategory::kAuth);
    EXPECT_EQ(mHttp->requests.size(), 1u);
    // Still tried later, and a renewal then works.
    mNow += SchwabAuth::kRetryInterval;
    mHttp->Push(TokenReply("access-2", "refresh-1"));
    auto renewed = auth->AccessToken();
    ASSERT_OK(renewed);
    EXPECT_EQ(*renewed, "access-2");

    // A rate limit is not a refusal: its own error, not an auth one.
    SaveTokens(-1min);
    auto limited = MakeAuth();
    ASSERT_OK(limited->Load());
    mHttp->PushStatus(429, "slow down");
    auto busy = limited->AccessToken();
    ASSERT_FALSE(busy.has_value());
    EXPECT_NE(busy.error().category, types::ErrorCategory::kAuth);
}

TEST_F(SchwabAuthTest, RetriesSavingARotatedRefreshToken) {
    SaveTokens(1min);
    auto auth = MakeAuth();
    ASSERT_OK(auth->Load());
    // A directory where the file should be: the save's rename fails.
    const auto file = mDir / "tokens.json";
    std::filesystem::remove(file);
    std::filesystem::create_directory(file);

    mHttp->Push(TokenReply("access-2", "refresh-2"));
    auto renewed = auth->AccessToken();
    ASSERT_OK(renewed);
    EXPECT_EQ(*renewed, "access-2");  // in use despite the failed save

    std::filesystem::remove(file);
    mNow += 5s;  // not yet time to retry
    ASSERT_OK(auth->AccessToken());
    EXPECT_FALSE(std::filesystem::exists(file));

    mNow += SchwabAuth::kRetryInterval;
    ASSERT_OK(auth->AccessToken());
    auto saved = Store().Load();
    ASSERT_OK(saved);
    EXPECT_EQ(saved->refresh_token, "refresh-2");
    EXPECT_EQ(mHttp->requests.size(), 1u);
}

TEST_F(SchwabAuthTest, TakesUpAReauthorizationBeforeRenewing) {
    SaveTokens(30min);
    auto auth = MakeAuth();
    ASSERT_OK(auth->Load());
    mNow += 20min;
    const auto fresh = Reauthorized();
    ASSERT_OK(Store().Save(fresh));  // `authorize` ran meanwhile

    mNow += 6min;  // the old access token is due for renewal
    auto token = auth->AccessToken();
    ASSERT_OK(token);
    EXPECT_EQ(*token, "access-new");
    EXPECT_TRUE(mHttp->requests.empty());  // its access token is still good
    EXPECT_EQ(auth->RefreshExpiresAt(), fresh.refresh_expires_at);

    // Renewing from then on uses the new refresh token.
    mNow += 26min;
    mHttp->Push(TokenReply("access-3", "refresh-new"));
    ASSERT_OK(auth->AccessToken());
    ASSERT_EQ(mHttp->requests.size(), 1u);
    EXPECT_EQ(FormValue(mHttp->requests[0].body, "refresh_token"),
              "refresh-new");
}

TEST_F(SchwabAuthTest, ARenewalNeverOverwritesAReauthorization) {
    SaveTokens(1min);
    auto auth = MakeAuth();
    ASSERT_OK(auth->Load());
    const auto fresh = Reauthorized();
    // `authorize` writes the file while the renewal's request is out.
    mHttp->on_send = [&] { ASSERT_OK(Store().Save(fresh)); };
    mHttp->Push(TokenReply("access-2", "refresh-1"));

    auto token = auth->AccessToken();
    ASSERT_OK(token);
    EXPECT_EQ(*token, "access-new");  // the renewal's tokens are dropped
    auto saved = Store().Load();
    ASSERT_OK(saved);
    EXPECT_EQ(saved->access_token, "access-new");
    EXPECT_EQ(saved->refresh_token, "refresh-new");
    EXPECT_EQ(saved->refresh_expires_at, fresh.refresh_expires_at);
    EXPECT_EQ(auth->RefreshExpiresAt(), fresh.refresh_expires_at);

    // If the file adopted mid-renewal already needs renewing too, AccessToken
    // renews that authorization before returning.
    SaveTokens(1min);
    auto slow = MakeAuth();
    ASSERT_OK(slow->Load());
    config::SchwabTokens stale_access = fresh;
    stale_access.access_expires_at = mNow + 1min;
    mHttp->on_send = [&] {
        mHttp->on_send = nullptr;
        ASSERT_OK(Store().Save(stale_access));
    };
    mHttp->Push(TokenReply("access-old-renewed", "refresh-1"));
    mHttp->Push(TokenReply("access-new-renewed", "refresh-new"));
    auto renewed_new = slow->AccessToken();
    ASSERT_OK(renewed_new);
    EXPECT_EQ(*renewed_new, "access-new-renewed");
    EXPECT_EQ(FormValue(mHttp->requests.back().body, "refresh_token"),
              "refresh-new");
}

TEST_F(SchwabAuthTest, AReauthorizationEndsARefusal) {
    SaveTokens(1min);
    auto auth = MakeAuth();
    ASSERT_OK(auth->Load());
    mHttp->PushStatus(400, R"({"error":"invalid_grant"})");
    auto refused = auth->AccessToken();
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().category, types::ErrorCategory::kAuth);

    // Another process on the same authorization rotated the refresh token.
    ASSERT_OK(Store().Save(
        {"access-sib", "refresh-rotated", mNow + 30min, mNow + 72h}));
    auto rotated = auth->AccessToken();
    ASSERT_OK(rotated);
    EXPECT_EQ(*rotated, "access-sib");

    // Refused again, then ended by a fresh `authorize`.
    mNow += 26min;
    mHttp->PushStatus(400, R"({"error":"invalid_grant"})");
    ASSERT_FALSE(auth->AccessToken().has_value());
    ASSERT_OK(Store().Save(Reauthorized()));
    auto token = auth->AccessToken();
    ASSERT_OK(token);
    EXPECT_EQ(*token, "access-new");
    EXPECT_EQ(mHttp->requests.size(), 2u);
}

TEST_F(SchwabAuthTest, AReauthorizationEndsAnExpiredRefreshToken) {
    SaveTokens(30min, 1h);
    auto auth = MakeAuth();
    ASSERT_OK(auth->Load());
    mNow += 2h;
    ASSERT_FALSE(auth->AccessToken().has_value());

    // A later file whose own refresh token has also expired is not adopted.
    ASSERT_OK(
        Store().Save({"access-stale", "refresh-stale", mNow - 1h, mNow - 1s}));
    auto stale = auth->AccessToken();
    ASSERT_FALSE(stale.has_value());
    EXPECT_EQ(stale.error().category, types::ErrorCategory::kAuth);
    EXPECT_TRUE(mHttp->requests.empty());

    ASSERT_OK(Store().Save(Reauthorized()));
    auto token = auth->AccessToken();
    ASSERT_OK(token);
    EXPECT_EQ(*token, "access-new");
    EXPECT_TRUE(mHttp->requests.empty());
}

TEST_F(SchwabAuthTest, AnOlderFileIsOverwrittenAsBefore) {
    SaveTokens(1min);
    auto auth = MakeAuth();
    ASSERT_OK(auth->Load());
    // Another process from the same authorization saved its own renewal: same
    // deadline, so this renewal still saves over it.
    ASSERT_OK(
        Store().Save({"access-other", "refresh-1", mNow + 10min, mNow + 72h}));
    mHttp->Push(TokenReply("access-2", "refresh-1"));
    auto token = auth->AccessToken();
    ASSERT_OK(token);
    EXPECT_EQ(*token, "access-2");
    auto saved = Store().Load();
    ASSERT_OK(saved);
    EXPECT_EQ(saved->access_token, "access-2");
}

TEST_F(SchwabAuthTest, PastTheRefreshDeadlineNothingIsRequested) {
    SaveTokens(30min, 1h);
    auto auth = MakeAuth();
    ASSERT_OK(auth->Load());
    mNow += 2h;
    auto token = auth->AccessToken();
    ASSERT_FALSE(token.has_value());
    EXPECT_EQ(token.error().category, types::ErrorCategory::kAuth);
    EXPECT_TRUE(mHttp->requests.empty());
}

TEST_F(SchwabAuthTest, InvalidateRenewsOnlyIfTheTokenIsStillCurrent) {
    SaveTokens(30min);
    auto auth = MakeAuth();
    ASSERT_OK(auth->Load());
    auth->Invalidate("some-older-token");
    ASSERT_OK(auth->AccessToken());
    EXPECT_TRUE(mHttp->requests.empty());

    auth->Invalidate("access-1");
    mHttp->Push(TokenReply("access-2", "refresh-1"));
    auto token = auth->AccessToken();
    ASSERT_OK(token);
    EXPECT_EQ(*token, "access-2");
}

TEST_F(SchwabAuthTest, InvalidateKeepsTheRetryInterval) {
    SaveTokens(1min);
    auto auth = MakeAuth();
    ASSERT_OK(auth->Load());
    mHttp->PushStatus(503, "unavailable");
    ASSERT_OK(auth->AccessToken());  // renewal failed; access-1 still valid
    EXPECT_EQ(mHttp->requests.size(), 1u);

    auth->Invalidate("access-1");
    auto token = auth->AccessToken();
    ASSERT_FALSE(token.has_value());  // the failed renewal's error
    EXPECT_EQ(token.error().category, types::ErrorCategory::kServer);
    EXPECT_EQ(mHttp->requests.size(), 1u);  // no new attempt yet

    mNow += SchwabAuth::kRetryInterval;
    mHttp->Push(TokenReply("access-2", "refresh-1"));
    auto renewed = auth->AccessToken();
    ASSERT_OK(renewed);
    EXPECT_EQ(*renewed, "access-2");
}

TEST_F(SchwabAuthTest, ApiSendsTheBearerTokenAndRetriesOnceAfter401) {
    SaveTokens(30min);
    auto auth = MakeAuth();
    ASSERT_OK(auth->Load());
    auto api_http = std::make_shared<FakeHttp>();
    SchwabApi api(auth, api_http, SchwabApi::Options{100, 100});

    net::HttpRequest request;
    request.url = "https://api.test/marketdata/v1/quotes";
    api_http->Push("{}");
    ASSERT_OK(api.Send(request));
    ASSERT_EQ(api_http->requests.size(), 1u);
    EXPECT_EQ(api_http->requests[0].headers.at("Authorization"),
              "Bearer access-1");

    // A 401 renews the token and sends again, once.
    api_http->PushStatus(401, "unauthorized");
    mHttp->Push(TokenReply("access-2", "refresh-1"));
    api_http->Push("{}");
    ASSERT_OK(api.Send(request));
    ASSERT_EQ(api_http->requests.size(), 3u);
    EXPECT_EQ(api_http->requests[2].headers.at("Authorization"),
              "Bearer access-2");

    api_http->PushStatus(401, "unauthorized");
    mHttp->Push(TokenReply("access-3", "refresh-1"));
    api_http->PushStatus(401, "unauthorized");
    auto refused = api.Send(request);
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().category, types::ErrorCategory::kAuth);
    EXPECT_EQ(api_http->requests.size(), 5u);
}

TEST_F(SchwabAuthTest, ApiSendsNothingWithoutAToken) {
    auto auth = MakeAuth();  // never loaded
    auto api_http = std::make_shared<FakeHttp>();
    SchwabApi api(auth, api_http, SchwabApi::Options{});
    auto r = api.Send(net::HttpRequest{});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().category, types::ErrorCategory::kAuth);
    EXPECT_TRUE(api_http->requests.empty());
}

// The factory builds Schwab's data source only with credentials and tokens,
// in paper mode as in live.
TEST_F(SchwabAuthTest, FactoryNeedsCredentialsAndTokensEvenInPaperMode) {
    config::LoadOptions options;
    std::map<std::string, std::string> env;
    options.getenv =
        [&env](const std::string& k) -> std::optional<std::string> {
        if (auto it = env.find(k); it != env.end()) {
            return it->second;
        }
        return std::nullopt;
    };
    options.base_dir = mDir;
    const std::string toml =
        "[general]\nprovider='schwab'\nmode='paper'\nsymbols=['AAPL']\n"
        "[schwab]\ntoken_file='tokens.json'\n";

    auto bare = config::SchwabConfiguration::Parse(toml, options);
    ASSERT_OK(bare);
    auto no_creds = CreateDataSource(*bare);
    ASSERT_FALSE(no_creds.has_value());
    EXPECT_EQ(no_creds.error().category, types::ErrorCategory::kAuth);
    EXPECT_FALSE(CreateComponents(*bare).has_value());

    env = {{"KESTREL_SCHWAB_APP_KEY", "k"}, {"KESTREL_SCHWAB_APP_SECRET", "s"}};
    auto cfg = config::SchwabConfiguration::Parse(toml, options);
    ASSERT_OK(cfg);
    auto no_tokens = CreateDataSource(*cfg);
    ASSERT_FALSE(no_tokens.has_value());
    EXPECT_EQ(no_tokens.error().category, types::ErrorCategory::kAuth);

    SaveTokens(30min);
    auto ds = CreateDataSource(*cfg);
    ASSERT_OK(ds);
    EXPECT_EQ((*ds)->Name(), "schwab");
}

}  // namespace
}  // namespace kestrel::data
