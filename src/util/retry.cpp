#include "kestrel/util/retry.hpp"

#include <cmath>
#include <mutex>
#include <random>

namespace kestrel::util {
namespace {

double Uniform(double lo, double hi) {
    static std::mutex m;
    static std::mt19937_64 rng{std::random_device{}()};
    std::lock_guard lock(m);
    return std::uniform_real_distribution<double>(lo, hi)(rng);
}

}  // namespace

std::chrono::milliseconds BackoffDelay(const RetryPolicy& policy, int retry) {
    double ms = static_cast<double>(policy.initial_delay.count()) *
                std::pow(policy.multiplier, std::max(0, retry - 1));
    ms = std::min(ms, static_cast<double>(policy.max_delay.count()));
    if (policy.jitter > 0.0) {
        ms *= 1.0 + Uniform(-policy.jitter, policy.jitter);
    }
    return std::chrono::milliseconds(static_cast<long long>(std::max(0.0, ms)));
}

}  // namespace kestrel::util
