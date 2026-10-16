#pragma once

#include <algorithm>
#include <chrono>
#include <functional>
#include <thread>
#include <type_traits>

#include "kestrel/types/error.hpp"

namespace kestrel::util {

struct RetryPolicy {
  int max_attempts = 5;  // total tries, including the first
  std::chrono::milliseconds initial_delay{500};
  double multiplier = 2.0;
  std::chrono::milliseconds max_delay{30'000};
  double jitter = 0.2;  // +/- fraction of the delay, to avoid thundering herds
};

// Delay before retry number `retry` (1 = first retry): initial * multiplier^(retry-1),
// capped at max_delay, with random jitter applied.
std::chrono::milliseconds BackoffDelay(const RetryPolicy& policy, int retry);

using Sleeper = std::function<void(std::chrono::milliseconds)>;
inline void DefaultSleep(std::chrono::milliseconds d) { std::this_thread::sleep_for(d); }

// Calls fn() until it succeeds, fails with a non-transient error, or attempts
// run out. fn must return a types::Result<T>. A provider-supplied retry_after
// hint acts as a floor on the delay.
template <typename Fn>
auto Retry(const RetryPolicy& policy, Fn&& fn, const Sleeper& sleep = DefaultSleep)
    -> std::invoke_result_t<Fn&> {
  for (int attempt = 1;; ++attempt) {
    auto result = fn();
    if (result || !result.error().IsTransient() || attempt >= policy.max_attempts)
    {
      return result;
    }
    auto delay = BackoffDelay(policy, attempt);
    if (result.error().retry_after)
    {
      delay = std::max(delay, *result.error().retry_after);
    }
    sleep(delay);
  }
}

}  // namespace kestrel::util
