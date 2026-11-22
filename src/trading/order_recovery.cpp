#include "kestrel/trading/order_recovery.hpp"

#include <algorithm>

namespace kestrel::trading {

bool PlacedLookup::AllFound() const {
    return std::ranges::all_of(
        found, [](const auto& order) { return order.has_value(); });
}

PlacedLookup LookUpPlaced(
    const std::vector<std::string>& client_ids,
    const std::function<types::Result<types::OrderStatus>(const std::string&)>&
        lookup,
    int looks, const std::function<void()>& pause) {
    PlacedLookup result;
    result.found.resize(client_ids.size());
    for (int look = 0; look < looks && !result.failure; ++look) {
        if (look > 0) {
            pause();
        }
        for (std::size_t i = 0; i < client_ids.size(); ++i) {
            if (result.found[i]) {
                continue;
            }
            auto order = lookup(client_ids[i]);
            if (order) {
                result.found[i] = std::move(*order);
            } else if (order.error().category !=
                       types::ErrorCategory::kNotFound) {
                result.failure = order.error();
                break;
            }
        }
        if (result.AllFound()) {
            break;
        }
    }
    return result;
}

void NoteLookup(types::Error& error, std::string_view what,
                const std::optional<types::Error>& failure) {
    const std::string name(what);
    error.message += failure
                         ? " (" + name + " could not be looked up either: " +
                               failure->ToString() + "; it may exist)"
                         : " (" + name +
                               " was not found afterwards, but may still "
                               "appear)";
}

}  // namespace kestrel::trading
