#include "kestrel/trading/session_gated_venue.hpp"

namespace kestrel::trading {

std::optional<types::Error> SessionGatedVenue::Closed(
    const types::Symbol& symbol) const {
    const auto now = mClock();
    if (mHours->IsOpen(now)) {
        return std::nullopt;
    }
    const auto next = mHours->CurrentOrNext(now);
    return types::MakeError(types::ErrorCategory::kRejected,
                            "market closed: " + symbol +
                                " cannot be traded until " +
                                (next ? types::FormatIso8601(next->start)
                                      : std::string("the market reopens")) +
                                (mHours->Extended() ? " (extended hours)" : ""))
        .error();
}

types::Result<types::OrderAcknowledgement> SessionGatedVenue::PlaceOrder(
    const types::OrderRequest& request) {
    if (auto closed = Closed(request.symbol)) {
        return std::unexpected(std::move(*closed));
    }
    return mVenue->PlaceOrder(request);
}

types::Result<types::OcoAcknowledgement> SessionGatedVenue::PlaceOco(
    const types::OcoRequest& request) {
    if (auto closed = Closed(request.symbol)) {
        return std::unexpected(std::move(*closed));
    }
    return mVenue->PlaceOco(request);
}

}  // namespace kestrel::trading
