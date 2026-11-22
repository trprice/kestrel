#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "kestrel/types/error.hpp"
#include "kestrel/types/orders.hpp"

namespace kestrel::trading {

// Recovery after placing orders whose outcome is unknown (the send's error
// MayHaveTakenEffect, or a 2xx answer that could not be read): look each up
// by the client ID it was sent with, rather than report a refusal or send it
// again. Venues that take client IDs (Binance.US, Kraken) use it; Schwab,
// which takes none, matches its recent orders by content instead.
struct PlacedLookup {
    // Per client ID, in the order given: what the venue holds, if found.
    std::vector<std::optional<types::OrderStatus>> found;
    // Set if a lookup failed with anything but kNotFound: the rest were not
    // looked for again, and any not found may still exist.
    std::optional<types::Error> failure;

    bool AllFound() const;
};

// Looks up each of `client_ids` with `lookup`, up to `looks` times, calling
// `pause` between rounds (an order can take a moment to show), and asking
// again only for those still not found. Stops at the first lookup failure
// other than kNotFound.
PlacedLookup LookUpPlaced(
    const std::vector<std::string>& client_ids,
    const std::function<types::Result<types::OrderStatus>(const std::string&)>&
        lookup,
    int looks, const std::function<void()>& pause);

// Says in `error`'s message what the lookup of `what` (e.g. "order kst12" or
// "OCO abc") found: that it could not be looked up and may exist, or that it
// was not found but may still appear.
void NoteLookup(types::Error& error, std::string_view what,
                const std::optional<types::Error>& failure);

}  // namespace kestrel::trading
