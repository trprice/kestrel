#include "kestrel/util/rate_limiter.hpp"

#include <algorithm>
#include <thread>

namespace kestrel::util {

RateLimiter::RateLimiter(double rate_per_second, double burst)
    : mRate(rate_per_second), mBurst(burst), mTokens(burst), mLast(Clock::now()) {}

void RateLimiter::Refill(Clock::time_point now) {
  const std::chrono::duration<double> elapsed = now - mLast;
  mTokens = std::min(mBurst, mTokens + elapsed.count() * mRate);
  mLast = now;
}

void RateLimiter::Acquire() {
  for (;;) {
    std::chrono::duration<double> wait{0};
    {
      std::lock_guard lock(mMutex);
      Refill(Clock::now());
      if (mTokens >= 1.0)
      {
        mTokens -= 1.0;
        return;
      }
      wait = std::chrono::duration<double>((1.0 - mTokens) / mRate);
    }
    std::this_thread::sleep_for(wait);
  }
}

bool RateLimiter::TryAcquire() {
  std::lock_guard lock(mMutex);
  Refill(Clock::now());
  if (mTokens < 1.0)
  {
    return false;
  }
  mTokens -= 1.0;
  return true;
}

void RateLimiter::Penalize() {
  std::lock_guard lock(mMutex);
  Refill(Clock::now());
  mTokens = 0.0;
}

}  // namespace kestrel::util
