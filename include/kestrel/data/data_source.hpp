#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "kestrel/types/error.hpp"
#include "kestrel/types/market.hpp"
#include "kestrel/types/timeframe.hpp"

namespace kestrel::data {

// Handle for a live quote stream. Destroying it, or calling Stop(), closes the
// connection. Neither may be called from inside the quote callback (the
// callback runs on the thread that Stop() joins, which would deadlock).
class Subscription {
   public:
    virtual ~Subscription() = default;
    virtual void Stop() = 0;
};

// THREADING: the callback is invoked from the WebSocket library's background
// thread, never from the thread that called StreamQuotes. It must be
// thread-safe, should return quickly, and must not throw. Quotes for
// different symbols are delivered serially, in arrival order.
using QuoteCallback = std::function<void(const types::Quote&)>;

class DataSource {
   public:
    virtual ~DataSource() = default;

    virtual std::string_view Name() const = 0;  // venue name, e.g. "kraken"

    // Historical OHLCV bars with start <= open_time < end, oldest first. The
    // bar that is still forming (open_time + interval > now) is never returned.
    // Pages through the provider's API when the range exceeds one request.
    virtual types::Result<std::vector<types::Bar>> GetBars(
        const types::Symbol& symbol, types::Timeframe timeframe,
        types::Timestamp start, types::Timestamp end) = 0;

    // Streams quotes over a WebSocket. The implementation reconnects with
    // backoff and resubscribes after disconnects, and merges partial updates so
    // every delivered Quote carries the latest known value of each field.
    virtual types::Result<std::unique_ptr<Subscription>> StreamQuotes(
        const std::vector<std::string>& symbols, QuoteCallback callback) = 0;

    // Tick size, lot size and minimum order size; fetched once, then cached.
    virtual types::Result<types::SymbolInfo> GetSymbolInfo(
        const types::Symbol& symbol) = 0;
};

}  // namespace kestrel::data
