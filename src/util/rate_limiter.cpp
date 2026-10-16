#include "kestrel/util/rate_limiter.hpp"

#include <algorithm>
#include <thread>

namespace kestrel::util {

RateLimiter::RateLimiter(double rate_per_second, double burst)
    : rate_(rate_per_second), burst_(burst), tokens_(burst), last_(Clock::now()) {}

void RateLimiter::Refill(Clock::time_point now) {
  const std::chrono::duration<double> elapsed = now - last_;
  tokens_ = std::min(burst_, tokens_ + elapsed.count() * rate_);
  last_ = now;
}

void RateLimiter::Acquire() {
  for (;;) {
    std::chrono::duration<double> wait{0};
    {
      std::lock_guard lock(mutex_);
      Refill(Clock::now());
      if (tokens_ >= 1.0) {
        tokens_ -= 1.0;
        return;
      }
      wait = std::chrono::duration<double>((1.0 - tokens_) / rate_);
    }
    std::this_thread::sleep_for(wait);
  }
}

bool RateLimiter::TryAcquire() {
  std::lock_guard lock(mutex_);
  Refill(Clock::now());
  if (tokens_ < 1.0) return false;
  tokens_ -= 1.0;
  return true;
}

void RateLimiter::Penalize() {
  std::lock_guard lock(mutex_);
  Refill(Clock::now());
  tokens_ = 0.0;
}

}  // namespace kestrel::util
