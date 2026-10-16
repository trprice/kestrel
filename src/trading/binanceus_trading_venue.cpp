#include "kestrel/trading/binanceus_trading_venue.hpp"

namespace kestrel::trading {

using types::ErrorCategory;
using types::MakeError;

namespace {
types::Error NotImplemented(const char* what) {
  return MakeError(ErrorCategory::kNotImplemented, std::string("BinanceUSTradingVenue::") + what + " is not implemented yet").error();
}
}  // namespace

BinanceUSTradingVenue::BinanceUSTradingVenue(config::BinanceUSCredentials credentials, std::shared_ptr<data::DataSource> data_source)
    : mCredentials(std::move(credentials)), mDataSource(std::move(data_source)) {}

types::Result<types::OrderAcknowledgement> BinanceUSTradingVenue::PlaceOrder(const types::OrderRequest&) {
  return std::unexpected(NotImplemented("PlaceOrder"));
}
types::Result<CancelOutcome> BinanceUSTradingVenue::CancelOrder(const std::string&) {
  return std::unexpected(NotImplemented("CancelOrder"));
}
types::Result<std::vector<std::string>> BinanceUSTradingVenue::CancelAll() {
  return std::unexpected(NotImplemented("CancelAll"));
}
types::Result<std::vector<types::OrderStatus>> BinanceUSTradingVenue::GetOpenOrders() {
  return std::unexpected(NotImplemented("GetOpenOrders"));
}
types::Result<std::vector<types::Fill>> BinanceUSTradingVenue::GetFills(types::Timestamp) {
  return std::unexpected(NotImplemented("GetFills"));
}
types::Result<std::vector<types::Position>> BinanceUSTradingVenue::GetPositions() {
  return std::unexpected(NotImplemented("GetPositions"));
}
types::Result<std::vector<types::Balance>> BinanceUSTradingVenue::GetBalances() {
  return std::unexpected(NotImplemented("GetBalances"));
}

}  // namespace kestrel::trading
