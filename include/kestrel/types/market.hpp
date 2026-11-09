#pragma once

#include <optional>
#include <string>

#include "kestrel/types/decimal.hpp"
#include "kestrel/types/orders.hpp"
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
    // The side that took liquidity in the last trade: a buyer lifting the ask,
    // or a seller hitting the bid. Unset when the provider does not say
    // (Kraken's ticker).
    std::optional<Side> last_taker_side;

    // Overlays every field `update` actually carries onto this quote, so
    // partial updates accumulate into the latest known state. received_time
    // always follows the update; exchange_time only when the update carries
    // one, so after a book update without it (Binance.US bookTicker) it is
    // still the last trade's time. An update that carries a trade
    // (last) also replaces last_taker_side, even with nothing, so one trade's
    // side never sticks to the next.
    void MergeFrom(const Quote& update);
};

// One public trade on the venue (anyone's, not the account's), with the side
// that took liquidity: a buyer taking the ask, or a seller hitting the bid.
struct MarketTrade {
    std::int64_t trade_id = 0;  // the venue's, increasing with time
    Timestamp time;
    Decimal price;
    Decimal quantity;
    Side taker_side = Side::kBuy;
};

struct SymbolInfo {
    Symbol symbol;
    Decimal tick_size;       // price increment
    Decimal lot_size;        // quantity increment
    Decimal min_order_size;  // minimum quantity
    // Minimum price × quantity, in the quote asset; zero when the venue sets
    // none (or it is not known).
    Decimal min_notional;
};

}  // namespace kestrel::types
