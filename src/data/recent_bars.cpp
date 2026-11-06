#include "kestrel/data/recent_bars.hpp"

#include <chrono>

namespace kestrel::data {

types::Result<std::vector<types::Bar>> RecentBars(DataSource& source,
                                                  const types::Symbol& symbol,
                                                  types::Timeframe timeframe,
                                                  std::size_t count,
                                                  types::Timestamp now) {
    using namespace std::chrono;
    if (count == 0) {
        return std::vector<types::Bar>{};
    }
    // A month for the calendar timeframes; the doubling covers the rest.
    const seconds step =
        types::FixedDuration(timeframe).value_or(hours(24 * 31));
    constexpr auto kMaxSpan = days(2 * 366);
    constexpr auto kClosure = days(7);  // longer than any market closure
    // One bar of padding in case the newest slot has no bar yet.
    auto span = duration_cast<types::Timestamp::duration>(
        step * static_cast<std::int64_t>(count + 1));
    std::size_t previous = 0;
    for (;;) {
        auto bars = source.GetBars(symbol, timeframe, now - span, now);
        if (!bars) {
            return std::unexpected(bars.error());
        }
        const bool enough = bars->size() >= count;
        const bool exhausted = span > kClosure && bars->size() <= previous;
        if (enough || exhausted || span >= kMaxSpan) {
            if (bars->size() > count) {
                bars->erase(bars->begin(),
                            bars->end() - static_cast<std::ptrdiff_t>(count));
            }
            return bars;
        }
        previous = bars->size();
        span = std::min<types::Timestamp::duration>(span * 2, kMaxSpan);
    }
}

}  // namespace kestrel::data
