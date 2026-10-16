#pragma once

#include <memory>

#include "kestrel/config/credentials.hpp"
#include "kestrel/data/data_source.hpp"
#include "kestrel/trading/trading_venue.hpp"

namespace kestrel::trading {

// STUB. Every call returns kNotImplemented.
//
// TODO(schwab): implement against the Schwab Trader API,
//   https://api.schwabapi.com/trader/v1.
//   Auth:     OAuth2 bearer token (`Authorization: Bearer <access_token>`),
//             refreshed with grant_type=refresh_token against
//             POST /v1/oauth/token using HTTP Basic auth (app key:secret).
//             Persist rotated tokens with config::SchwabTokenStore and call
//             SchwabTokenStore::Check on startup; once the 7-day refresh token
//             expires the user must re-authorize and no call can succeed.
//   Account:  GET /accounts/accountNumbers returns the encrypted hashValue that
//             every other account endpoint takes in its path.
//   PlaceOrder: POST /accounts/{hash}/orders with JSON {orderType: MARKET|LIMIT|
//             STOP, session: NORMAL, duration: DAY|GOOD_TILL_CANCEL|
//             FILL_OR_KILL, orderStrategyType: SINGLE, price, stopPrice,
//             orderLegCollection: [{instruction: BUY|SELL, quantity,
//             instrument: {symbol, assetType: EQUITY}}]}. 201 Created; the
//             order id is the last segment of the Location header.
//   Cancel:   DELETE /accounts/{hash}/orders/{orderId}.
//   Open orders: GET /accounts/{hash}/orders?fromEnteredTime=&toEnteredTime=
//             &status=WORKING.
//   Fills:    GET the same endpoint without a status filter and read
//             orderActivityCollection[].executionLegs[] (price, quantity, time).
//   Positions / balances: GET /accounts/{hash}?fields=positions ->
//             securitiesAccount.positions and currentBalances.
//   Limits:   roughly 120 requests/minute; order placement is throttled
//             separately.
class SchwabTradingVenue : public TradingVenue {
 public:
  SchwabTradingVenue(config::SchwabCredentials credentials, std::shared_ptr<data::DataSource> data_source);

  std::string_view Name() const override { return "schwab"; }
  types::Result<types::OrderAcknowledgement> PlaceOrder(const types::OrderRequest& request) override;
  types::Result<CancelOutcome> CancelOrder(const std::string& order_id) override;
  types::Result<std::vector<std::string>> CancelAll() override;
  types::Result<std::vector<types::OrderStatus>> GetOpenOrders() override;
  types::Result<std::vector<types::Fill>> GetFills(types::Timestamp since) override;
  types::Result<std::vector<types::Position>> GetPositions() override;
  types::Result<std::vector<types::Balance>> GetBalances() override;

 private:
  config::SchwabCredentials credentials_;
  std::shared_ptr<data::DataSource> data_source_;  // for tick/lot rules via ApplySymbolRules
};

}  // namespace kestrel::trading
