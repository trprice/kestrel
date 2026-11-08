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
//   Auth:     build on data::SchwabAuth and data::SchwabApi
//             (data/schwab_auth.hpp), as SchwabDataSource does. SchwabAuth
//             shares the process's tokens (from `kestrel_app authorize`),
//             renews and saves them, and reports kAuth once the 7-day refresh
//             token has expired. Give the Trader API its own SchwabApi, since
//             each API product has its own rate limit; SchwabApi::Send adds
//             the bearer token and resends once after a 401. Never retry an
//             order request otherwise (a timeout may have placed it): look it
//             up instead, as BinanceUSApi does.
//   Account:  GET /accounts/accountNumbers returns the encrypted hashValue that
//             every other account endpoint takes in its path.
//   PlaceOrder: POST /accounts/{hash}/orders with JSON {orderType:
//             MARKET|LIMIT|STOP, session: NORMAL, duration:
//             DAY|GOOD_TILL_CANCEL|FILL_OR_KILL, orderStrategyType: SINGLE,
//             price, stopPrice, orderLegCollection: [{instruction: BUY|SELL,
//             quantity, instrument: {symbol, assetType: EQUITY}}]}. 201
//             Created; the order id is the last segment of the Location header.
//   Cancel:   DELETE /accounts/{hash}/orders/{orderId}.
//   Open orders: GET /accounts/{hash}/orders?fromEnteredTime=&toEnteredTime=
//             &status=WORKING.
//   Fills:    GET the same endpoint without a status filter and read
//             orderActivityCollection[].executionLegs[] (price, quantity,
//             time).
//   Positions / balances: GET /accounts/{hash}?fields=positions ->
//             securitiesAccount.positions and currentBalances.
//   Limits:   roughly 120 requests/minute (check the Trader API's own
//             figure); order placement is throttled separately.
//   Pushed executions (ACCT_ACTIVITY) would share SchwabStreamer's
//   connection: Schwab allows one streamer login per user.
class SchwabTradingVenue : public TradingVenue {
   public:
    SchwabTradingVenue(config::SchwabCredentials credentials,
                       std::shared_ptr<data::DataSource> data_source);

    std::string_view Name() const override { return "schwab"; }
    types::Result<types::OrderAcknowledgement> PlaceOrder(
        const types::OrderRequest& request) override;
    types::Result<CancelOutcome> CancelOrder(
        const std::string& order_id) override;
    types::Result<std::vector<std::string>> CancelAll() override;
    types::Result<std::vector<types::OrderStatus>> GetOpenOrders() override;
    types::Result<types::OrderStatus> GetOrder(
        const std::string& order_id) override;
    types::Result<std::vector<types::Fill>> GetFills(
        types::Timestamp since) override;
    types::Result<std::vector<types::Position>> GetPositions() override;
    types::Result<std::vector<types::Balance>> GetBalances() override;

   private:
    config::SchwabCredentials mCredentials;
    std::shared_ptr<data::DataSource>
        mDataSource;  // for tick/lot rules via ApplySymbolRules
};

}  // namespace kestrel::trading
