#pragma once

#include <chrono>
#include <mutex>

namespace kestrel::util {

// Thread-safe token bucket. Acquire() blocks until a token is available.
class RateLimiter {
   public:
    // `rate_per_second` tokens are added per second, up to `burst` stored.
    RateLimiter(double rate_per_second, double burst);

    void Acquire();
    bool TryAcquire();

    // Called when the provider says we are over the limit: drains the bucket so
    // the next Acquire waits a full refill interval.
    void Penalize();

   private:
    using Clock = std::chrono::steady_clock;
    void Refill(Clock::time_point now);  // requires mMutex

    std::mutex mMutex;
    const double mRate;
    const double mBurst;
    double mTokens;
    Clock::time_point mLast;
};

}  // namespace kestrel::util
