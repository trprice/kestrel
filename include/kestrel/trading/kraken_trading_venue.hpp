#pragma once

#include <memory>

#include "kestrel/config/credentials.hpp"
#include "kestrel/data/data_source.hpp"
#include "kestrel/trading/trading_venue.hpp"

namespace kestrel::trading {

// STUB. Every call returns kNotImplemented.
//
// TODO(kraken): implement against the Kraken Spot REST API (https://api.kraken.com).
//   Auth:     private endpoints are POSTs (application/x-www-form-urlencoded)
//             with a strictly increasing `nonce` in the body and headers
//               API-Key:  the API key
//               API-Sign: base64(HMAC-SHA512(base64decode(secret),
//                                  uri_path + SHA256(nonce + postdata)))
//             util::HmacSha512 / Sha256 / Base64* already provide the pieces.
//   PlaceOrder: /0/private/AddOrder (pair, type=buy|sell, ordertype=
//             market|limit|stop-loss, volume, price, price2, timeinforce=
//             GTC|IOC|GTD, cl_ord_id, validate). Returns result.txid[].
//   Cancel:   /0/private/CancelOrder (txid); /0/private/CancelAll. Cancelling an
//             order that already filled returns EOrder:Unknown order -> look it
//             up with /0/private/QueryOrders to report kAlreadyFilled.
//   Open orders: /0/private/OpenOrders.   Fills: /0/private/TradesHistory (start=
//             <epoch>, paged with ofs).   Balances: /0/private/BalanceEx
//             (balance + hold_trade gives the locked amount).
//   Positions: spot has none; derive them from balances and trade history.
//   Limits:   a per-account call counter (starter tier: max 15, decays 0.33/s;
//             TradesHistory costs 2); order placement has a separate matching
//             engine limiter. EAPI:Rate limit exceeded -> back off and retry.
class KrakenTradingVenue : public TradingVenue {
 public:
  KrakenTradingVenue(config::KrakenCredentials credentials, std::shared_ptr<data::DataSource> data_source);

  std::string_view Name() const override { return "kraken"; }
  types::Result<types::OrderAcknowledgement> PlaceOrder(const types::OrderRequest& request) override;
  types::Result<CancelOutcome> CancelOrder(const std::string& order_id) override;
  types::Result<std::vector<std::string>> CancelAll() override;
  types::Result<std::vector<types::OrderStatus>> GetOpenOrders() override;
  types::Result<std::vector<types::Fill>> GetFills(types::Timestamp since) override;
  types::Result<std::vector<types::Position>> GetPositions() override;
  types::Result<std::vector<types::Balance>> GetBalances() override;

 private:
  config::KrakenCredentials mCredentials;
  std::shared_ptr<data::DataSource> mDataSource;  // for tick/lot rules via ApplySymbolRules
};

}  // namespace kestrel::trading
