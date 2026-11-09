#include "kestrel/data/recent_bars.hpp"

#include <chrono>
#include <optional>

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
    const auto newest = [count](std::vector<types::Bar> bars) {
        if (bars.size() > count) {
            bars.erase(bars.begin(),
                       bars.end() - static_cast<std::ptrdiff_t>(count));
        }
        return bars;
    };
    const auto one = duration_cast<types::Timestamp::duration>(step);
    std::size_t previous = 0;
    // The longest span the source answered, with its bars, and the shortest
    // it refused as past the history it keeps (kUnsupported, as Kraken does
    // past its latest 720 bars).
    std::optional<
        std::pair<types::Timestamp::duration, std::vector<types::Bar>>>
        good;
    std::optional<types::Timestamp::duration> refused;
    for (;;) {
        if (refused && good && *refused - good->first <= one) {
            return newest(std::move(good->second));  // as far back as it goes
        }
        auto bars = source.GetBars(symbol, timeframe, now - span, now);
        if (!bars) {
            if (bars.error().category != types::ErrorCategory::kUnsupported ||
                span <= one) {
                return std::unexpected(bars.error());
            }
            refused = span;
        } else if (refused) {
            if (bars->size() >= count) {
                return newest(std::move(*bars));
            }
            good.emplace(span, std::move(*bars));
        } else {
            const bool enough = bars->size() >= count;
            const bool exhausted = span > kClosure && bars->size() <= previous;
            if (enough || exhausted || span >= kMaxSpan) {
                return newest(std::move(*bars));
            }
            previous = bars->size();
            good.emplace(span, std::move(*bars));
            span = std::min<types::Timestamp::duration>(span * 2, kMaxSpan);
            continue;
        }
        // Past the source's history: halve until a span fits, then search
        // between the two for the longest that does.
        // In whole bars, so the search ends on the last bar that fits.
        const auto steps = [&](types::Timestamp::duration d) {
            return d / one;
        };
        span = good ? good->first +
                          one * ((steps(*refused) - steps(good->first)) / 2)
                    : one * std::max<std::int64_t>(steps(span) / 2, 1);
    }
}

}  // namespace kestrel::data
