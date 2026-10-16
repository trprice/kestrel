#include <thread>

#include "kestrel/util/hmac.hpp"
#include "kestrel/util/json.hpp"
#include "kestrel/util/rate_limiter.hpp"
#include "kestrel/util/retry.hpp"
#include "test_helpers.hpp"

namespace {

using namespace kestrel;
using namespace std::chrono_literals;

TEST(Hmac, Rfc4231Vectors) {
  EXPECT_EQ(util::HexEncode(util::HmacSha256("Jefe", "what do ya want for nothing?")),
            "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
  EXPECT_EQ(util::HexEncode(util::HmacSha512("Jefe", "what do ya want for nothing?")),
            "164b7a7bfcf819e2e395fbe73b56e0a387bd64222e831fd610270cd7ea250554"
            "9758bf75c05a994a6d034f65f8f0e6fdcaeab1a34d4a6b4b636e070a38bce737");
  EXPECT_EQ(util::HexEncode(util::Sha256("abc")),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(Base64, RoundTrip) {
  for (const std::string s : std::vector<std::string>{"", "f", "fo", "foo", "foob", std::string("\x00\xff\x10", 3)}) {
    auto decoded = util::Base64Decode(util::Base64Encode(s));
    ASSERT_OK(decoded);
    EXPECT_EQ(*decoded, s);
  }
  EXPECT_EQ(util::Base64Encode("foobar"), "Zm9vYmFy");
  EXPECT_FALSE(util::Base64Decode("abc").has_value());
}

TEST(Json, KeepsFractionalNumbersExact) {
  auto j = util::ParseJson(R"({"p":0.10025,"q":740.0,"n":5,"e":1e-8,"s":"x","a":[1.5,[2]],"b":true,"z":null})");
  ASSERT_OK(j);
  EXPECT_EQ(util::JsonDecimal((*j)["p"])->ToString(), "0.10025");
  EXPECT_EQ(util::JsonDecimal((*j)["q"])->ToString(), "740.0");
  EXPECT_EQ(util::JsonDecimal((*j)["n"])->ToString(), "5");
  EXPECT_EQ(util::JsonDecimal((*j)["e"])->ToString(), "0.00000001");
  EXPECT_EQ((*j)["a"][1][0], 2);
  EXPECT_TRUE((*j)["b"].get<bool>());
  EXPECT_FALSE(util::JsonDecimal((*j)["z"]).has_value());
  // A double could not hold this value exactly.
  auto precise = util::ParseJson(R"({"v":0.1234567890123456789})");
  ASSERT_OK(precise);
  EXPECT_EQ(util::JsonDecimal((*precise)["v"])->ToString(), "0.123456789012345679");
}

TEST(Json, RejectsMalformed) {
  EXPECT_FALSE(util::ParseJson("{").has_value());
  EXPECT_FALSE(util::ParseJson("").has_value());
}

TEST(Retry, BackoffGrowsAndCaps) {
  util::RetryPolicy p;
  p.initial_delay = 100ms;
  p.multiplier = 2.0;
  p.max_delay = 500ms;
  p.jitter = 0.0;
  EXPECT_EQ(util::BackoffDelay(p, 1), 100ms);
  EXPECT_EQ(util::BackoffDelay(p, 2), 200ms);
  EXPECT_EQ(util::BackoffDelay(p, 3), 400ms);
  EXPECT_EQ(util::BackoffDelay(p, 4), 500ms);
  p.jitter = 0.5;
  for (int i = 0; i < 100; ++i) {
    const auto d = util::BackoffDelay(p, 1);
    EXPECT_GE(d, 50ms);
    EXPECT_LE(d, 150ms);
  }
}

TEST(Retry, RetriesTransientErrorsThenSucceeds) {
  util::RetryPolicy p;
  p.max_attempts = 5;
  p.jitter = 0.0;
  int calls = 0;
  std::vector<std::chrono::milliseconds> sleeps;
  auto result = util::Retry(
      p,
      [&]() -> types::Result<int> {
        if (++calls < 3) return types::MakeError(types::ErrorCategory::kNetwork, "down");
        return 42;
      },
      [&](std::chrono::milliseconds d) { sleeps.push_back(d); });
  ASSERT_OK(result);
  EXPECT_EQ(*result, 42);
  EXPECT_EQ(calls, 3);
  EXPECT_EQ(sleeps, (std::vector{500ms, 1000ms}));
}

TEST(Retry, DoesNotRetryPermanentErrors) {
  int calls = 0;
  auto result = util::Retry({}, [&]() -> types::Result<int> {
    ++calls;
    return types::MakeError(types::ErrorCategory::kRejected, "no");
  }, [](auto) { FAIL() << "should not sleep"; });
  EXPECT_FALSE(result.has_value());
  EXPECT_EQ(calls, 1);
}

TEST(Retry, GivesUpAfterMaxAttemptsAndHonoursRetryAfter) {
  util::RetryPolicy p;
  p.max_attempts = 3;
  p.jitter = 0.0;
  p.initial_delay = 10ms;
  int calls = 0;
  std::vector<std::chrono::milliseconds> sleeps;
  auto result = util::Retry(
      p,
      [&]() -> types::Result<int> {
        ++calls;
        auto e = types::MakeError(types::ErrorCategory::kRateLimit, "slow down");
        e.error().retry_after = 2000ms;
        return e;
      },
      [&](std::chrono::milliseconds d) { sleeps.push_back(d); });
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().category, types::ErrorCategory::kRateLimit);
  EXPECT_EQ(calls, 3);
  EXPECT_EQ(sleeps, (std::vector{2000ms, 2000ms}));
}

TEST(RateLimiter, BurstThenRefill) {
  util::RateLimiter limiter(50.0, 2.0);  // 50/s -> a token every 20 ms
  EXPECT_TRUE(limiter.TryAcquire());
  EXPECT_TRUE(limiter.TryAcquire());
  EXPECT_FALSE(limiter.TryAcquire());
  const auto start = std::chrono::steady_clock::now();
  limiter.Acquire();  // blocks until a token is available
  EXPECT_GE(std::chrono::steady_clock::now() - start, 10ms);
  limiter.Penalize();
  EXPECT_FALSE(limiter.TryAcquire());
}

}  // namespace
