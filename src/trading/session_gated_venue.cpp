#include "kestrel/trading/session_gated_venue.hpp"

namespace kestrel::trading {

types::Result<types::OrderAcknowledgement> SessionGatedVenue::PlaceOrder(
    const types::OrderRequest& request) {
    const auto now = mClock();
    if (!mHours->IsOpen(now)) {
        const auto next = mHours->CurrentOrNext(now);
        return types::MakeError(
            types::ErrorCategory::kRejected,
            "market closed: " + request.symbol + " cannot be traded until " +
                (next ? types::FormatIso8601(next->start)
                      : std::string("the market reopens")) +
                (mHours->Extended() ? " (extended hours)" : ""));
    }
    return mVenue->PlaceOrder(request);
}

}  // namespace kestrel::trading
