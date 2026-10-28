#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "kestrel/config/credentials.hpp"
#include "kestrel/data/data_source.hpp"
#include "kestrel/net/http_client.hpp"
#include "kestrel/trading/binanceus_api.hpp"
#include "kestrel/trading/trading_venue.hpp"

namespace kestrel::trading {

// Binance.US spot trading over the signed REST API (see BinanceUSApi for the
// keys, signing, clock and rate-limit handling).
//
// PARTLY IMPLEMENTED. The read side works: GetBalances, GetOpenOrders,
// GetOrder and GetFills. PlaceOrder, CancelOrder and CancelAll still return
// kNotImplemented, so a run cannot trade yet (HasLiveVenue in factory.cpp
// stays false until they exist).
//
// TODO(binanceus): PlaceOrder: POST /api/v3/order (symbol=BTCUSD, side,
//   type=MARKET|LIMIT|STOP_LOSS, timeInForce=GTC|IOC|FOK, quantity, price,
//   stopPrice, newClientOrderId, newOrderRespType=FULL); never retried on a
//   timeout, look the order up by client order ID instead. Cancel:
//   DELETE /api/v3/order (symbol + orderId), DELETE /api/v3/openOrders.
//
// Every call that needs a symbol (Binance has no account-wide order or trade
// query that is cheap enough to poll) goes over the `symbols` the venue was
// given, which is [general] symbols. An order or fill on any other symbol is
// invisible to it.
//
// Request weight matters: the runner polls GetFills (weight 10 per symbol)
// and GetOpenOrders (3 per symbol) every fill_poll_interval, 13 per second per
// symbol at the default 1 s, against a budget of 1200 per minute shared with
// every order. Raise fill_poll_interval for a live run, or add the user-data
// stream (see TODO.md).
//
// Positions: spot has none, and the venue cannot tell what the account held
// before it started, so GetPositions returns an empty list, as the paper venue
// does for holdings it never bought. Read GetBalances for what is held.
class BinanceUSTradingVenue : public TradingVenue {
   public:
    // `http` defaults to a timed HTTP client; tests pass a fake.
    BinanceUSTradingVenue(config::BinanceUSCredentials credentials,
                          std::shared_ptr<data::DataSource> data_source,
                          std::vector<types::Symbol> symbols,
                          std::shared_ptr<net::HttpClient> http = nullptr,
                          BinanceUSApi::Options options = {});

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
    // Remembers which symbol an order is on, since Binance looks orders up by
    // symbol and order ID but the TradingVenue interface passes only the ID.
    void Remember(const std::string& order_id, const types::Symbol& symbol);

    config::BinanceUSCredentials mCredentials;
    std::shared_ptr<data::DataSource>
        mDataSource;  // for tick/lot rules via ApplySymbolRules
    std::vector<types::Symbol> mSymbols;
    BinanceUSApi mApi;
    std::function<types::Timestamp()> mClock;

    std::mutex mMutex;
    std::map<std::string, types::Symbol> mOrderSymbols;
};

}  // namespace kestrel::trading
