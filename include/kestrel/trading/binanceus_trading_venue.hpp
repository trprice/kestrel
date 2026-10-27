#pragma once

#include <memory>

#include "kestrel/config/credentials.hpp"
#include "kestrel/data/data_source.hpp"
#include "kestrel/trading/trading_venue.hpp"

namespace kestrel::trading {

// STUB. Every call returns kNotImplemented.
//
// TODO(binanceus): implement against https://api.binance.us.
//   Auth:     header X-MBX-APIKEY: <key>; signed endpoints also take
//             `timestamp` (ms), optional `recvWindow`, and `signature` =
//             hex(HMAC-SHA256(secret, query string + body)) as the last param.
//             util::HmacSha256 + HexEncode already provide this.
//   PlaceOrder: POST /api/v3/order (symbol=BTCUSD, side, type=MARKET|LIMIT|
//             STOP_LOSS|STOP_LOSS_LIMIT, timeInForce=GTC|IOC|FOK, quantity,
//             price, stopPrice, newClientOrderId, newOrderRespType=FULL).
//   Cancel:   DELETE /api/v3/order (symbol + orderId); DELETE
//   /api/v3/openOrders
//             (symbol). Error -2011 / -2013 means unknown or already filled.
//   Open orders: GET /api/v3/openOrders.
//   Fills:    GET /api/v3/myTrades (symbol required, startTime=<ms>); iterate
//             the configured symbols.
//   Balances: GET /api/v3/account -> balances[{asset, free, locked}].
//   Positions: spot has none; derive them from balances and trade history.
//   Limits:   1200 request-weight/minute and 10 orders/second; HTTP 429 means
//             back off, 418 means the IP was banned.
class BinanceUSTradingVenue : public TradingVenue {
   public:
    BinanceUSTradingVenue(config::BinanceUSCredentials credentials,
                          std::shared_ptr<data::DataSource> data_source);

    std::string_view Name() const override { return "binanceus"; }
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
    config::BinanceUSCredentials mCredentials;
    std::shared_ptr<data::DataSource>
        mDataSource;  // for tick/lot rules via ApplySymbolRules
};

}  // namespace kestrel::trading
