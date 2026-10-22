#pragma once

#include <optional>
#include <string>

#include "kestrel/types/decimal.hpp"
#include "kestrel/types/symbol.hpp"
#include "kestrel/types/time.hpp"
#include "kestrel/types/timeframe.hpp"

namespace kestrel::types {

struct Bar {
    Timestamp open_time;
    Decimal open;
    Decimal high;
    Decimal low;
    Decimal close;
    Decimal volume;
};

// A top-of-book / last-trade snapshot. Providers rarely send every field in
// every message, so everything beyond identity and receive time is optional.
struct Quote {
    Symbol symbol;
    std::string venue;  // e.g. "kraken"
    // Arrival order within (venue, symbol), assigned by QuoteWriter and kept
    // across runs; received_time alone can tie within a millisecond.
    std::int64_t seq = 0;
    std::optional<Timestamp> exchange_time;  // provider's timestamp
    Timestamp received_time;                 // when we got it
    std::optional<Decimal> bid;
    std::optional<Decimal> bid_size;
    std::optional<Decimal> ask;
    std::optional<Decimal> ask_size;
    std::optional<Decimal> last;
    std::optional<Decimal> last_size;

    // Overlays every field `update` actually carries onto this quote, so
    // partial updates accumulate into the latest known state. received_time and
    // exchange_time always follow the update.
    void MergeFrom(const Quote& update);
};

struct SymbolInfo {
    Symbol symbol;
    Decimal tick_size;       // price increment
    Decimal lot_size;        // quantity increment
    Decimal min_order_size;  // minimum quantity
};

}  // namespace kestrel::types
