#include "kestrel/data/feed_watch.hpp"

namespace kestrel::data {

FeedWatch::FeedWatch(std::chrono::milliseconds threshold,
                     std::shared_ptr<const types::MarketHours> hours)
    : mThreshold(threshold),
      mHours(hours ? std::move(hours) : types::MarketHours::AlwaysOpen()) {}

void FeedWatch::Start(types::Timestamp now) {
    std::lock_guard lock(mMutex);
    mLast = now;
    mDown = false;
}

std::optional<types::Timestamp::duration> FeedWatch::Note(
    types::Timestamp now) {
    std::lock_guard lock(mMutex);
    std::optional<types::Timestamp::duration> lasted;
    if (mDown && mLast) {
        lasted = mHours->OpenDuration(*mLast, now);
    }
    mDown = false;
    mLast = now;
    return lasted;
}

std::optional<types::Timestamp::duration> FeedWatch::Check(
    types::Timestamp now) {
    std::lock_guard lock(mMutex);
    if (mThreshold <= std::chrono::milliseconds::zero() || !mLast || mDown) {
        return std::nullopt;
    }
    const auto quiet = mHours->OpenDuration(*mLast, now);
    if (quiet < mThreshold) {
        return std::nullopt;
    }
    mDown = true;
    return quiet;
}

}  // namespace kestrel::data
