#pragma once

#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "kestrel/data/data_source.hpp"
#include "kestrel/data/schwab_auth.hpp"
#include "kestrel/trading/trading_venue.hpp"
#include "kestrel/util/rate_limiter.hpp"
#include "kestrel/util/retry.hpp"

namespace kestrel::trading {

inline constexpr std::string_view kSchwabTraderBaseUrl =
    "https://api.schwabapi.com/trader/v1";

// Live trading on Schwab's Trader API, sharing the data source's tokens
// (SchwabAuth, from `kestrel_app authorize`) through its own SchwabApi. That
// has its own rate limiter, but Schwab's 120 requests a minute are the app's,
// shared with the data source.
//
// - Every account endpoint takes the account's hashValue, read once from
//   /accounts/accountNumbers.
// - Only the configured symbols are Kestrel's: GetOpenOrders, GetFills and
//   CancelAll leave orders on other symbols alone (placed by hand, say).
// - Reads are retried; placing and cancelling are not. When a placement ends
//   without an answer (a timeout, a dropped connection or a 5xx), the
//   account's recent orders are searched for it.
// - Open orders and fills come from GET /orders. Each fetch starts at the
//   oldest order working at the last fetch (or 180 days back, the longest a
//   GTC order lives), not 180 days back every time. A fetch that reaches
//   Schwab's 3000 orders is split in two.
// - Fills since before any fetch (a fresh process: `account`, the fill sync)
//   come from the orders entered since then, plus the older ones that the
//   account's TRADE transactions show trading since then, looked up by ID.
//   A transaction can lag its execution, so an old order's fill in the last
//   moments may be missed there.
// - Orders are for the regular session. Schwab has no post-only flag (such
//   an order is refused), and no client order ID: client_order_id is known
//   only for orders this instance placed.
// Known limitations are under Venues and data in TODO.md.
class SchwabTradingVenue : public TradingVenue {
   public:
    struct Options {
        std::string base_url = std::string(kSchwabTraderBaseUrl);
        // The account to trade, its number or its hashValue; empty: the
        // first linked (with a warning if there are more).
        std::string account_number;
        // The symbols whose orders and fills are Kestrel's; empty: all.
        std::vector<types::Symbol> symbols;
        // Placing and cancelling, on top of SchwabApi's own limit.
        double orders_per_second = 1.0;
        double order_burst = 5.0;
        util::RetryPolicy retry{};
        util::Sleeper sleeper = util::DefaultSleep;
        std::function<types::Timestamp()> clock = types::Now;
    };

    SchwabTradingVenue(std::shared_ptr<data::SchwabApi> api,
                       std::shared_ptr<data::DataSource> data_source,
                       Options options);
    SchwabTradingVenue(std::shared_ptr<data::SchwabApi> api,
                       std::shared_ptr<data::DataSource> data_source);

    std::string_view Name() const override { return "schwab"; }
    types::Result<types::OrderAcknowledgement> PlaceOrder(
        const types::OrderRequest& request) override;
    types::Result<types::OcoAcknowledgement> PlaceOco(
        const types::OcoRequest& request) override;
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

    // Checks `request` against the symbol's tick and lot, then has Schwab
    // check it (POST previewOrder) without placing it.
    types::Result<void> TestOrder(const types::OrderRequest& request);

   private:
    struct PreparedOrder {
        types::OrderRequest conformed;
        std::string schwab_symbol;
        std::string json_body;
    };
    // A fetch of the account's orders for GetOpenOrders or GetFills: when it
    // was made, and the oldest entry time of the symbols' orders then working
    // (the fetch time if none were).
    struct OrderScan {
        types::Timestamp time;
        types::Timestamp oldest_working;
    };
    using ClientIdLookup = std::function<std::string(const std::string&)>;

    types::Result<std::string> AccountHash();
    // base_url/accounts/{hash}, followed by `path`.
    std::string AccountUrl(const std::string& hash,
                           std::string_view path = {}) const;
    types::Result<net::HttpResponse> SendGet(
        const std::string& url,
        std::map<std::string, std::string> query = {}) const;
    // Places, previews or cancels an order: held to the order rate limit and
    // never retried.
    types::Result<net::HttpResponse> SendOrderRequest(net::HttpMethod method,
                                                      std::string url,
                                                      std::string body = {});
    types::Result<nlohmann::json> FetchOrders(const std::string& hash,
                                              types::Timestamp from,
                                              types::Timestamp to) const;
    // The orders entered from shortly before `placed_at` until now, for
    // finding an order whose placement ended without an answer. The second
    // attempt waits a moment first.
    types::Result<nlohmann::json> RecentOrders(const std::string& hash,
                                               types::Timestamp placed_at,
                                               int attempt) const;
    types::Result<nlohmann::json> FetchOrder(const std::string& hash,
                                             const std::string& order_id) const;
    // The IDs of our symbols' orders that traded from `since` to `now`, from
    // the TRADE transactions; nullopt if some may be missing (Schwab's cap,
    // or a trade without an order ID).
    types::Result<std::optional<std::vector<std::string>>> TradedOrderIds(
        const std::string& hash, types::Timestamp since,
        types::Timestamp now) const;
    // Fills from the orders entered since `since` and the older ones traded
    // since then; nullopt (for FillsFromOrders instead) when those are not
    // all known: the transactions or a lookup failed, or too many lookups.
    std::optional<types::Result<std::vector<types::Fill>>> FillsInWindow(
        const std::string& hash, types::Timestamp since, types::Timestamp now);
    // Fills from a fetch starting at OrdersFrom, recorded as a scan.
    types::Result<std::vector<types::Fill>> FillsFromOrders(
        const std::string& hash, types::Timestamp since, types::Timestamp now);
    // `fills` on our symbols, oldest first.
    std::vector<types::Fill> OurFillsInOrder(
        std::vector<types::Fill> fills) const;
    // The account's securitiesAccount object, with its positions.
    types::Result<nlohmann::json> SecuritiesAccount(std::string_view what);
    types::Result<PreparedOrder> PrepareOrder(
        const types::OrderRequest& request) const;

    bool IsOurs(const types::Symbol& symbol) const;
    // The oldest order working at the last fetch at or before `since` and at
    // every fetch since; nullopt if there was no fetch by then.
    std::optional<types::Timestamp> OldestWorkingSince(
        types::Timestamp since) const;
    // Where GetOpenOrders (since = now) and GetFills fetch orders from.
    types::Timestamp OrdersFrom(types::Timestamp since,
                                types::Timestamp now) const;
    void RecordScan(types::Timestamp time, types::Timestamp oldest_working);

    void RememberOrder(const std::string& order_id,
                       const std::string& client_order_id);
    void ClaimOrder(const std::string& order_id,
                    const std::string& client_order_id);
    // Claims `order_id` unless a placement or recovery here already has.
    // Claims are kept apart from the known orders, so that a poll seeing the
    // new order during a timed-out placement does not stop recovery from
    // claiming it.
    bool TryClaimRecoveredOrder(const std::string& order_id,
                                const std::string& client_order_id);
    bool TryClaimRecoveredOco(const std::string& parent_id,
                              const std::string& limit_id,
                              const std::string& stop_id);
    std::string ClientOrderIdFor(const std::string& order_id) const;
    ClientIdLookup ClientIds() const;
    std::unordered_set<std::string> KnownOrdersSnapshot() const;
    std::unordered_set<std::string> ClaimedOrdersSnapshot() const;

    // Cancels `order_id` while recovering from a failure; if that fails, logs
    // it and adds to `error_message` that the order may still be working.
    void CancelOnFailure(const std::string& order_id,
                         std::string& error_message);
    // The error for an OCO whose legs did not both go in: cancels the leg
    // still working.
    types::Error FailOco(const types::OrderStatus& limit,
                         const types::OrderStatus& stop);

    types::Result<std::optional<types::OrderStatus>> RecoverPlacedOrder(
        const std::string& hash, const types::OrderRequest& conformed,
        types::Timestamp placed_at,
        const std::unordered_set<std::string>& known_before);
    types::Result<std::optional<types::OcoAcknowledgement>> RecoverPlacedOco(
        const std::string& hash, const types::OrderRequest& limit_leg,
        const types::OrderRequest& stop_leg, types::Timestamp placed_at,
        const std::unordered_set<std::string>& known_before,
        std::string_view expected_parent_id = {});

    void Sleep(std::chrono::milliseconds duration) const;

    std::shared_ptr<data::SchwabApi> mApi;
    std::shared_ptr<data::DataSource> mDataSource;
    Options mOptions;
    std::unordered_set<types::Symbol> mSymbols;  // empty: all
    mutable util::RateLimiter mOrderLimiter;

    mutable std::mutex mMutex;
    std::optional<std::string> mAccountHash;
    std::unordered_set<std::string> mKnownOrders;
    std::unordered_set<std::string> mClaimedOrders;
    std::unordered_map<std::string, std::string> mClientOrderIds;
    std::deque<OrderScan> mScans;  // oldest first
};

}  // namespace kestrel::trading
