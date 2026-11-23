#pragma once

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>

#include "kestrel/types/market_hours.hpp"
#include "kestrel/types/time.hpp"

namespace kestrel::data {

// Whether a stream has gone quiet for too long: no message at all, for
// `threshold` of open market (closed time, a night or a weekend, does not
// count). It reports an outage once, and its end once, for the caller to
// log: an error, so the alerts send it, and a line when it is over.
// Note may come from the stream's thread while another calls Check.
class FeedWatch {
   public:
    // A zero threshold turns the check off. Null hours count as always open.
    FeedWatch(std::chrono::milliseconds threshold,
              std::shared_ptr<const types::MarketHours> hours);

    // From `now`, as if a message had just come (the stream's start).
    void Start(types::Timestamp now);
    // A message came at `now`. If an outage had been reported, how much
    // open time it lasted, for the caller to log its end.
    std::optional<types::Timestamp::duration> Note(types::Timestamp now);
    // The open time since the last message, once it reaches the threshold
    // (only once, until Note): an outage to report. Nothing before Start or
    // Note.
    std::optional<types::Timestamp::duration> Check(types::Timestamp now);

   private:
    const std::chrono::milliseconds mThreshold;
    const std::shared_ptr<const types::MarketHours> mHours;
    std::mutex mMutex;
    std::optional<types::Timestamp> mLast;
    bool mDown = false;
};

}  // namespace kestrel::data
