#include "kestrel/trading/kraken_trading_venue.hpp"

namespace kestrel::trading {

using types::ErrorCategory;
using types::MakeError;

namespace {
types::Error NotImplemented(const char* what) {
    return MakeError(ErrorCategory::kNotImplemented,
                     std::string("KrakenTradingVenue::") + what +
                         " is not implemented yet")
        .error();
}
}  // namespace

KrakenTradingVenue::KrakenTradingVenue(
    config::KrakenCredentials credentials,
    std::shared_ptr<data::DataSource> data_source)
    : mCredentials(std::move(credentials)),
      mDataSource(std::move(data_source)) {}

types::Result<types::OrderAcknowledgement> KrakenTradingVenue::PlaceOrder(
    const types::OrderRequest&) {
    return std::unexpected(NotImplemented("PlaceOrder"));
}
types::Result<CancelOutcome> KrakenTradingVenue::CancelOrder(
    const std::string&) {
    return std::unexpected(NotImplemented("CancelOrder"));
}
types::Result<std::vector<std::string>> KrakenTradingVenue::CancelAll() {
    return std::unexpected(NotImplemented("CancelAll"));
}
types::Result<std::vector<types::OrderStatus>>
KrakenTradingVenue::GetOpenOrders() {
    return std::unexpected(NotImplemented("GetOpenOrders"));
}
types::Result<std::vector<types::Fill>> KrakenTradingVenue::GetFills(
    types::Timestamp) {
    return std::unexpected(NotImplemented("GetFills"));
}
types::Result<std::vector<types::Position>> KrakenTradingVenue::GetPositions() {
    return std::unexpected(NotImplemented("GetPositions"));
}
types::Result<std::vector<types::Balance>> KrakenTradingVenue::GetBalances() {
    return std::unexpected(NotImplemented("GetBalances"));
}

}  // namespace kestrel::trading
