#pragma once

#include <cstddef>
#include <vector>

#include "kestrel/data/data_source.hpp"

namespace kestrel::data {

// The newest `count` closed bars of a series before `now`, oldest first (fewer
// if the history has fewer). A span of `count` bars is asked for first; while
// closed time (nights, weekends, holidays) leaves fewer than `count` in it,
// the span doubles. It stops at two years, or once a span of over a week
// brought no more bars than the one before (the history is that short, or
// the symbol unknown), so a short history costs a few requests, not a
// thousand. A source that refuses a span as reaching past the history it keeps
// (kUnsupported, as Kraken does past its latest 720 bars) gets the bars of a
// span that fits.
types::Result<std::vector<types::Bar>> RecentBars(DataSource& source,
                                                  const types::Symbol& symbol,
                                                  types::Timeframe timeframe,
                                                  std::size_t count,
                                                  types::Timestamp now);

}  // namespace kestrel::data
