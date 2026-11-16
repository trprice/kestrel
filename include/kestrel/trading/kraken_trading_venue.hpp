#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "kestrel/config/credentials.hpp"
#include "kestrel/data/data_source.hpp"
#include "kestrel/net/http_client.hpp"
#include "kestrel/trading/trading_venue.hpp"
#include "kestrel/util/rate_limiter.hpp"
#include "kestrel/util/retry.hpp"

namespace kestrel::trading {

// Computes Kraken's API-Sign header value:
//   base64(HMAC-SHA512(base64decode(secret), uri_path + SHA256(nonce + body)))
// Fails with kAuth when `base64_secret` is empty or not valid base64.
types::Result<std::string> SignKrakenRequest(std::string_view uri_path,
                                             std::string_view nonce,
                                             std::string_view post_data,
                                             std::string_view base64_secret);

// Kraken spot trading over the private REST API (https://api.kraken.com).
//
// Authentication: every private call is a POST to /0/private/<Method>
// (application/x-www-form-urlencoded) carrying a strictly increasing `nonce`
// in the body and `API-Key` / `API-Sign` headers. Nonce assignment and HTTP
// transmission are serialized under one lock so concurrent callers in this
// process (the runner thread and the fill poller) never deliver nonces out of
// order when the API key's nonce window is zero; a slow poll can hold up a
// placement for one round trip. Running a second process (`account`, `order`)
// at the same time needs a separate API key or a non-zero nonce window on
// Kraken.
//
// Orders: PlaceOrder sends /0/private/AddOrder after ApplySymbolRules:
//   market  ordertype=market
//   limit   ordertype=limit, timeinforce=GTC|IOC|FOK, oflags=post when
//           post_only is set (Kraken usually accepts a crossing post-only
//           order with a txid and cancels it at once with reason "Post only
//           order": seen on the next poll, as kRejected; a synchronous
//           EOrder:Post only maps to kWouldTake)
//   stop    ordertype=stop-loss (a stop-market; GTC only, trigger in `price`)
// Every order also carries oflags=fciq, so the fee is taken in the quote
// asset, as fills report it (Kraken's default takes a sell's fee in the base
// asset). Day orders are kUnsupported. Each order carries `cl_ord_id` (the
// caller's, or a generated "kst<tag><n>" alphanumeric ID of at most 18
// characters). TestOrder sends the same request with `validate=true`, so Kraken
// checks parameters, permissions and funds without placing the order.
//
// PlaceOco is not implemented: Kraken's conditional close attaches one closing
// order to an entry, not a take-profit and stop-loss pair on a held position.
//
// A placement or cancel is never retried. When AddOrder ends with an unknown
// outcome (timeout, network error, 5xx, or an unparseable 2xx), the order is
// looked up by `cl_ord_id` in OpenOrders and ClosedOrders before reporting an
// error.
//
// Only the configured `symbols` ([general] symbols) belong to Kestrel:
// GetOpenOrders, GetFills and CancelAll filter to them (when non-empty),
// leaving orders on other pairs alone.
//
// Rate limits: private account calls draw from `counter_per_second` /
// `counter_burst` (sized for Kraken's Starter tier: max 15, decay 0.33/s;
// OpenOrders, QueryOrders and BalanceEx cost 1, TradesHistory and
// ClosedOrders cost 2). Order placements and cancels cost 0 on the account
// counter and are paced by `orders_per_second` / `order_burst`.
//
// Positions: spot has none, so GetPositions returns an empty list; read
// GetBalances (/0/private/BalanceEx) for what is held.
class KrakenTradingVenue : public TradingVenue {
   public:
    struct Options {
        std::string rest_base_url = "https://api.kraken.com";
        util::RetryPolicy retry;
        // Account call counter: Starter tier is max 15, decaying 0.33/s.
        double counter_per_second = 0.3;
        double counter_burst = 12.0;
        // Matching-engine pacing for AddOrder and CancelOrder.
        double orders_per_second = 1.0;
        double order_burst = 5.0;
        std::function<types::Timestamp()> clock = types::Now;
        util::Sleeper sleeper = util::DefaultSleep;
    };

    using Params = std::vector<std::pair<std::string, std::string>>;

    KrakenTradingVenue(config::KrakenCredentials credentials,
                       std::shared_ptr<data::DataSource> data_source,
                       std::vector<types::Symbol> symbols,
                       std::shared_ptr<net::HttpClient> http, Options options);
    KrakenTradingVenue(config::KrakenCredentials credentials,
                       std::shared_ptr<data::DataSource> data_source,
                       std::vector<types::Symbol> symbols,
                       std::shared_ptr<net::HttpClient> http = nullptr);
    KrakenTradingVenue(config::KrakenCredentials credentials,
                       std::shared_ptr<data::DataSource> data_source);

    std::string_view Name() const override { return "kraken"; }

    // Sends AddOrder with validate=true: Kraken validates the order without
    // placing it.
    types::Result<void> TestOrder(const types::OrderRequest& request);

    types::Result<types::OrderAcknowledgement> PlaceOrder(
        const types::OrderRequest& request) override;
    types::Result<CancelOutcome> CancelOrder(
        const std::string& order_id) override;
    types::Result<std::vector<std::string>> CancelAll() override;
    types::Result<std::vector<types::OrderStatus>> GetOpenOrders() override;
    types::Result<types::OrderStatus> GetOrder(
        const std::string& order_id) override;
    // Pages /0/private/TradesHistory (`with_cursor=true`, `limit=100`,
    // newest-first) until reaching `since` or the final page. Fails the whole
    // call (never returning a partial page) if a later page fails or the cursor
    // stalls, so callers do not advance their `since` cursor past unread fills.
    types::Result<std::vector<types::Fill>> GetFills(
        types::Timestamp since) override;
    types::Result<std::vector<types::Position>> GetPositions() override;
    types::Result<std::vector<types::Balance>> GetBalances() override;

   private:
    enum class Bucket { kCounter, kOrder };
    struct CallCost {
        Bucket bucket;
        int tokens;
    };
    static constexpr CallCost kBalanceCall{Bucket::kCounter, 1};
    static constexpr CallCost kOpenOrdersCall{Bucket::kCounter, 1};
    static constexpr CallCost kQueryOrdersCall{Bucket::kCounter, 1};
    static constexpr CallCost kClosedOrdersCall{Bucket::kCounter, 2};
    static constexpr CallCost kTradesHistoryCall{Bucket::kCounter, 2};
    static constexpr CallCost kOrderCall{Bucket::kOrder, 1};

    types::Result<nlohmann::json> PostPrivate(const std::string& method,
                                              const Params& params,
                                              CallCost cost, bool idempotent);
    types::Result<nlohmann::json> SendOnce(const std::string& method,
                                           const Params& params, CallCost cost);

    types::Result<Params> OrderParams(const types::OrderRequest& request,
                                      std::string& client_id);
    std::string NextClientOrderId();
    types::Result<types::OrderStatus> LookupClientOrder(
        const std::string& client_id,
        std::optional<types::Timestamp> not_before);
    types::Result<types::OrderStatus> LookupClientOrderIn(
        const std::string& method, const char* bucket, const Params& params,
        CallCost cost, const std::string& client_id,
        std::optional<types::Timestamp> not_before);
    types::Result<CancelOutcome> CancelOutcomeOf(const std::string& order_id);

    types::Result<types::Symbol> ResolveSymbol(std::string_view native) const;
    // Maps a Kraken pair to a configured symbol: returns the normalized symbol
    // when it is ours, `nullopt` to skip an unconfigured symbol (or an
    // unrecognised pair when `symbols` is non-empty), or `kParse` when the
    // venue is unscoped (`symbols` empty) and the pair cannot be parsed.
    types::Result<std::optional<types::Symbol>> OurSymbol(
        std::string_view native, std::string_view source) const;
    bool IsOurs(const types::Symbol& symbol) const;

    void Remember(const std::string& order_id, const std::string& client_id);
    std::string ClientIdOf(const std::string& order_id);

    types::Result<types::OrderStatus> ParseOrder(
        const std::string& order_id, const nlohmann::json& order,
        std::optional<types::Symbol> known_symbol = std::nullopt);
    types::Result<types::Fill> ParseTrade(const std::string& trade_id,
                                          const nlohmann::json& trade,
                                          const types::Symbol& symbol,
                                          types::Timestamp time);

    config::KrakenCredentials mCredentials;
    types::Result<std::string> mDecodedSecret;
    std::shared_ptr<data::DataSource> mDataSource;
    std::vector<types::Symbol> mSymbols;
    std::set<types::Symbol> mSymbolSet;
    std::map<std::string, types::Symbol> mNativeSymbols;
    std::shared_ptr<net::HttpClient> mHttp;
    Options mOptions;

    util::RateLimiter mCounterLimiter;
    util::RateLimiter mOrderLimiter;

    std::mutex mSendMutex;
    std::uint64_t mLastNonce = 0;

    std::string mClientTag;
    std::atomic<std::uint64_t> mNextClientId{1};

    std::mutex mRememberMutex;
    std::map<std::string, std::string> mClientIds;
    std::deque<std::string> mRememberOrder;
};

}  // namespace kestrel::trading
