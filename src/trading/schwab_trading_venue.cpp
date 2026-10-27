#include "kestrel/trading/schwab_trading_venue.hpp"

namespace kestrel::trading {

using types::ErrorCategory;
using types::MakeError;

namespace {
types::Error NotImplemented(const char* what) {
    return MakeError(ErrorCategory::kNotImplemented,
                     std::string("SchwabTradingVenue::") + what +
                         " is not implemented yet")
        .error();
}
}  // namespace

SchwabTradingVenue::SchwabTradingVenue(
    config::SchwabCredentials credentials,
    std::shared_ptr<data::DataSource> data_source)
    : mCredentials(std::move(credentials)),
      mDataSource(std::move(data_source)) {}

types::Result<types::OrderAcknowledgement> SchwabTradingVenue::PlaceOrder(
    const types::OrderRequest&) {
    return std::unexpected(NotImplemented("PlaceOrder"));
}
types::Result<CancelOutcome> SchwabTradingVenue::CancelOrder(
    const std::string&) {
    return std::unexpected(NotImplemented("CancelOrder"));
}
types::Result<std::vector<std::string>> SchwabTradingVenue::CancelAll() {
    return std::unexpected(NotImplemented("CancelAll"));
}
types::Result<std::vector<types::OrderStatus>>
SchwabTradingVenue::GetOpenOrders() {
    return std::unexpected(NotImplemented("GetOpenOrders"));
}
types::Result<types::OrderStatus> SchwabTradingVenue::GetOrder(
    const std::string&) {
    return std::unexpected(NotImplemented("GetOrder"));
}
types::Result<std::vector<types::Fill>> SchwabTradingVenue::GetFills(
    types::Timestamp) {
    return std::unexpected(NotImplemented("GetFills"));
}
types::Result<std::vector<types::Position>> SchwabTradingVenue::GetPositions() {
    return std::unexpected(NotImplemented("GetPositions"));
}
types::Result<std::vector<types::Balance>> SchwabTradingVenue::GetBalances() {
    return std::unexpected(NotImplemented("GetBalances"));
}

}  // namespace kestrel::trading
