#pragma once

#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
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
// Orders: PlaceOrder sends POST /api/v3/order after ApplySymbolRules (tick,
// lot, minimum size and value, from the data source's symbol info):
//   market  MARKET
//   limit   LIMIT, with timeInForce GTC, IOC or FOK
//   stop    STOP_LOSS (a stop-market; no time in force)
// A DAY order is kUnsupported: Binance.US has no day orders. Every order gets
// a client order ID, the caller's if it set one, otherwise "kst-" plus a
// per-process tag and a counter, so the account's orders and fills show which
// came from Kestrel.
//
// A placement is never sent twice. When it fails in a way that leaves its fate
// unknown (a timeout, a broken connection, a 5xx), the order is looked up by
// its client order ID: found, it is acknowledged as it stands; not found after
// a second look a second later, the original error is returned with the
// client ID in the message, as the order may still turn up (it would then be
// visible in GetOpenOrders but not tracked by the caller). New orders are held
// to Binance's order rate (orders_per_second, order_burst), on top of the
// request weight.
//
// CancelOrder sends DELETE /api/v3/order. Binance answers an order that is no
// longer open with -2011, so the order is then looked up to tell kAlreadyFilled
// from kAlreadyClosed, and kNotFound means the venue does not know it at all.
// A cancel whose fate is unknown is looked up the same way. CancelAll sends
// DELETE /api/v3/openOrders per configured symbol; every symbol is tried even
// if one fails, and the error then names the failures and what was cancelled.
//
// TestOrder sends the same request to POST /api/v3/order/test, which checks
// it (signature, filters, permissions) without placing anything.
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

    // Builds and checks `request` exactly as PlaceOrder would and sends it to
    // the test endpoint: nothing is placed. Succeeds if Binance accepts it.
    types::Result<void> TestOrder(const types::OrderRequest& request);

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
    std::optional<types::Symbol> SymbolOf(const std::string& order_id);

    // The parameters of a new order: symbol rules applied, a client order ID
    // set. Fills in `native` with the Binance symbol.
    types::Result<BinanceUSApi::Params> OrderParams(
        const types::OrderRequest& request, std::string& native,
        std::string& client_id);
    std::string NextClientOrderId();
    // GET /api/v3/order by client order ID; kNotFound if Binance has none.
    types::Result<types::OrderStatus> LookupClientOrder(
        const std::string& native, const std::string& client_id);
    // What became of an order a cancel could not settle.
    types::Result<CancelOutcome> CancelOutcomeOf(const std::string& order_id);

    config::BinanceUSCredentials mCredentials;
    std::shared_ptr<data::DataSource>
        mDataSource;  // for tick/lot rules via ApplySymbolRules
    std::vector<types::Symbol> mSymbols;
    BinanceUSApi mApi;
    std::function<types::Timestamp()> mClock;
    util::Sleeper mSleep;
    util::RateLimiter mOrderLimiter;
    const std::string mClientTag;  // per process, in generated client IDs
    std::atomic<std::uint64_t> mNextClientId{1};

    std::mutex mMutex;
    std::map<std::string, types::Symbol> mOrderSymbols;
    std::deque<std::string> mRememberOrder;  // oldest first, to bound the map
};

}  // namespace kestrel::trading
