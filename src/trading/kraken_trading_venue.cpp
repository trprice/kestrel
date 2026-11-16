#include "kestrel/trading/kraken_trading_venue.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <random>

#include "kestrel/data/kraken_data_source.hpp"
#include "kestrel/data/symbols.hpp"
#include "kestrel/net/timed_http_client.hpp"
#include "kestrel/trading/order_rules.hpp"
#include "kestrel/util/hmac.hpp"
#include "kestrel/util/json.hpp"
#include "kestrel/util/url.hpp"

namespace kestrel::trading {

using nlohmann::json;
using types::ErrorCategory;
using types::MakeError;
using types::Result;

namespace {

constexpr auto kLookupPause = std::chrono::seconds(1);
constexpr auto kClosedLookupSlack = std::chrono::minutes(5);
// Asked for as `limit` alongside `with_cursor=true` (Kraken's maximum is 100).
constexpr std::size_t kTradesPageSize = 100;
constexpr std::size_t kRememberedOrders = 10'000;

// After a write has been sent, a transport failure, a 5xx or an unparseable
// 2xx body leaves whether Kraken acted on it unknown.
bool OutcomeUnknown(const types::Error& error) {
    return error.category == ErrorCategory::kNetwork ||
           error.category == ErrorCategory::kTimeout ||
           error.category == ErrorCategory::kServer ||
           error.category == ErrorCategory::kParse;
}

bool IsUnknownOrderError(const types::Error& error) {
    return error.category == ErrorCategory::kNotFound ||
           (error.provider_code &&
            error.provider_code->starts_with("EOrder:Invalid order"));
}

std::string AtScaleOf(const types::Decimal& value,
                      const types::Decimal& increment) {
    return value
        .Rescale(increment.Normalized().Scale(), types::Rounding::kNearest)
        .ToString();
}

std::string RandomTag() {
    static constexpr char kAlphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    std::random_device device;
    std::uniform_int_distribution<int> pick(0, sizeof kAlphabet - 2);
    std::string tag;
    for (int i = 0; i < 5; ++i) {
        tag += kAlphabet[pick(device)];
    }
    return tag;
}

bool IsHex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

bool IsUuid(std::string_view s) {
    if (s.size() == 32) {
        return std::ranges::all_of(s, IsHex);
    }
    if (s.size() != 36) {
        return false;
    }
    for (std::size_t i = 0; i < s.size(); ++i) {
        const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash ? s[i] != '-' : !IsHex(s[i])) {
            return false;
        }
    }
    return true;
}

// Kraken accepts cl_ord_id as a long UUID (36 chars), a short UUID (32 hex
// chars), or free-form ASCII text up to 18 characters.
bool ValidClientOrderId(const std::string& id) {
    if (id.empty()) {
        return false;
    }
    if (IsUuid(id)) {
        return true;
    }
    if (id.size() > 18) {
        return false;
    }
    return std::ranges::all_of(id, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}

// QueryOrders accepts a comma-delimited list of txids; reject empty or
// multi-ID strings so GetOrder and CancelOrder address a single order.
bool ValidOrderId(std::string_view id) {
    if (id.empty()) {
        return false;
    }
    return std::ranges::all_of(
        id, [](unsigned char c) { return c > ' ' && c < 0x7f && c != ','; });
}

std::string FormEncode(const KrakenTradingVenue::Params& params) {
    std::string out;
    for (const auto& [key, value] : params) {
        if (!out.empty()) {
            out += '&';
        }
        out += util::UrlEncode(key) + '=' + util::UrlEncode(value);
    }
    return out;
}

types::Error Malformed(const std::string& what) {
    return MakeError(ErrorCategory::kParse, "kraken: " + what).error();
}

Result<json> Member(const json& object, const char* key) {
    if (!object.is_object() || !object.contains(key)) {
        return std::unexpected(
            Malformed(std::string("missing \"") + key + '"'));
    }
    return object[key];
}

Result<types::Decimal> DecimalMember(const json& object, const char* key) {
    auto value = Member(object, key);
    if (!value) {
        return std::unexpected(value.error());
    }
    auto number = util::JsonDecimal(*value);
    if (!number) {
        return std::unexpected(Malformed(std::string("bad number in \"") + key +
                                         "\": " + number.error().message));
    }
    return *number;
}

Result<types::Decimal> OptionalDecimal(const json& object, const char* key) {
    if (!object.is_object()) {
        return std::unexpected(Malformed("expected an object"));
    }
    const auto it = object.find(key);
    if (it == object.end() || it->is_null()) {
        return types::Decimal();
    }
    auto number = util::JsonDecimal(*it);
    if (!number) {
        return std::unexpected(Malformed(std::string("bad number in \"") + key +
                                         "\": " + number.error().message));
    }
    return *number;
}

Result<std::string> TextMember(const json& object, const char* key) {
    auto value = Member(object, key);
    if (!value) {
        return std::unexpected(value.error());
    }
    if (!value->is_string()) {
        return std::unexpected(
            Malformed(std::string("\"") + key + "\" is not a string"));
    }
    return value->get<std::string>();
}

std::string OptionalText(const json& object, const char* key) {
    if (object.is_object() && object.contains(key) && object[key].is_string()) {
        return object[key].get<std::string>();
    }
    return {};
}

// Kraken timestamps are floating-point epoch seconds (which util::ParseJson
// preserves as exact decimal strings, or integers when whole).
Result<types::Timestamp> TimestampFromSeconds(const json& value,
                                              const char* key) {
    auto seconds = util::JsonDecimal(value);
    if (!seconds || seconds->IsNegative()) {
        return std::unexpected(
            Malformed(std::string("bad timestamp in \"") + key + '"'));
    }
    const std::int64_t ms = (*seconds * types::Decimal::FromInt(1000))
                                .Rescale(0, types::Rounding::kNearest)
                                .Mantissa();
    return types::FromEpochMillis(ms);
}

Result<types::Side> SideFrom(const std::string& side) {
    if (side == "buy") {
        return types::Side::kBuy;
    }
    if (side == "sell") {
        return types::Side::kSell;
    }
    return std::unexpected(Malformed("unknown order side \"" + side + "\""));
}

// Kestrel places only `market`, `limit` and `stop-loss`; the remaining Kraken
// types (`iceberg`, `settle-position`, `take-profit*`, `trailing-stop*`) map
// here so external orders on the account do not fail `GetOpenOrders` /
// `GetOrder` with `kParse`.
Result<types::OrderType> OrderTypeFrom(const std::string& type) {
    if (type == "market") {
        return types::OrderType::kMarket;
    }
    if (type == "limit" || type == "iceberg" || type == "settle-position") {
        return types::OrderType::kLimit;
    }
    if (type == "stop-loss" || type == "stop-loss-limit" ||
        type == "take-profit" || type == "take-profit-limit" ||
        type == "trailing-stop" || type == "trailing-stop-limit") {
        return types::OrderType::kStop;
    }
    return std::unexpected(Malformed("unknown order type \"" + type + "\""));
}

Result<types::TimeInForce> TimeInForceFrom(const std::string& tif) {
    if (tif.empty() || tif == "GTC" || tif == "GTD") {
        return types::TimeInForce::kGtc;
    }
    if (tif == "IOC") {
        return types::TimeInForce::kIoc;
    }
    if (tif == "FOK") {
        return types::TimeInForce::kFok;
    }
    return std::unexpected(Malformed("unknown time in force \"" + tif + "\""));
}

Result<types::OrderState> OrderStateFrom(const std::string& status,
                                         const types::Decimal& filled) {
    if (status == "closed") {
        return !filled.IsZero() ? types::OrderState::kFilled
                                : types::OrderState::kExpired;
    }
    if (status == "canceled" || status == "cancelled") {
        return types::OrderState::kCancelled;
    }
    if (status == "expired") {
        return types::OrderState::kExpired;
    }
    if (status == "open" || status == "pending") {
        return !filled.IsZero() ? types::OrderState::kPartiallyFilled
                                : types::OrderState::kNew;
    }
    return std::unexpected(
        Malformed("unknown order status \"" + status + "\""));
}

Result<std::string> DecodeSecret(std::string_view base64_secret) {
    if (base64_secret.empty()) {
        return MakeError(ErrorCategory::kAuth, "kraken: empty API secret");
    }
    auto decoded = util::Base64Decode(base64_secret);
    if (!decoded || decoded->empty()) {
        return MakeError(ErrorCategory::kAuth,
                         "kraken: invalid API secret (expected base64)");
    }
    return decoded;
}

std::string SignWithDecodedSecret(std::string_view uri_path,
                                  std::string_view nonce,
                                  std::string_view post_data,
                                  std::string_view decoded_secret) {
    std::string nonce_and_body;
    nonce_and_body.reserve(nonce.size() + post_data.size());
    nonce_and_body.append(nonce);
    nonce_and_body.append(post_data);
    const std::string sha = util::Sha256(nonce_and_body);
    std::string message;
    message.reserve(uri_path.size() + sha.size());
    message.append(uri_path);
    message.append(sha);
    return util::Base64Encode(util::HmacSha512(decoded_secret, message));
}

// Kraken's TradesHistory returns `cursor: {"next": "<token>"}` when more pages
// follow, and omits `cursor` or sets `cursor` / `cursor.next` to null or empty
// on the final page.
Result<std::string> NextTradesCursor(const json& result) {
    const auto it = result.find("cursor");
    if (it == result.end() || it->is_null()) {
        return std::string{};
    }
    if (!it->is_object()) {
        return std::unexpected(
            Malformed("TradesHistory: \"cursor\" is not an object"));
    }
    const auto next = it->find("next");
    if (next == it->end() || next->is_null()) {
        return std::string{};
    }
    if (!next->is_string()) {
        return std::unexpected(
            Malformed("TradesHistory: \"cursor.next\" is not a string"));
    }
    return next->get<std::string>();
}

}  // namespace

Result<std::string> SignKrakenRequest(std::string_view uri_path,
                                      std::string_view nonce,
                                      std::string_view post_data,
                                      std::string_view base64_secret) {
    auto secret = DecodeSecret(base64_secret);
    if (!secret) {
        return std::unexpected(secret.error());
    }
    return SignWithDecodedSecret(uri_path, nonce, post_data, *secret);
}

KrakenTradingVenue::KrakenTradingVenue(
    config::KrakenCredentials credentials,
    std::shared_ptr<data::DataSource> data_source,
    std::vector<types::Symbol> symbols, std::shared_ptr<net::HttpClient> http,
    Options options)
    : mCredentials(std::move(credentials)),
      mDecodedSecret(DecodeSecret(mCredentials.api_secret)),
      mDataSource(std::move(data_source)),
      mHttp(http ? std::move(http) : net::MakeTimedHttpClient()),
      mOptions(std::move(options)),
      mCounterLimiter(mOptions.counter_per_second, mOptions.counter_burst),
      mOrderLimiter(mOptions.orders_per_second, mOptions.order_burst),
      mClientTag(RandomTag()) {
    for (const auto& raw : symbols) {
        auto normalized = types::NormalizeSymbol(raw);
        const types::Symbol sym = normalized ? *normalized : raw;
        mSymbols.push_back(sym);
        mSymbolSet.insert(sym);
        if (const auto parts = types::SplitSymbol(sym); parts && parts->quote) {
            mNativeSymbols.emplace(parts->base + *parts->quote, sym);
            mNativeSymbols.emplace(parts->base + "/" + *parts->quote, sym);
        }
    }
}

KrakenTradingVenue::KrakenTradingVenue(
    config::KrakenCredentials credentials,
    std::shared_ptr<data::DataSource> data_source,
    std::vector<types::Symbol> symbols, std::shared_ptr<net::HttpClient> http)
    : KrakenTradingVenue(std::move(credentials), std::move(data_source),
                         std::move(symbols), std::move(http), Options{}) {}

KrakenTradingVenue::KrakenTradingVenue(
    config::KrakenCredentials credentials,
    std::shared_ptr<data::DataSource> data_source)
    : KrakenTradingVenue(std::move(credentials), std::move(data_source), {},
                         nullptr, Options{}) {}

Result<types::Symbol> KrakenTradingVenue::ResolveSymbol(
    std::string_view native) const {
    if (const auto it = mNativeSymbols.find(std::string(native));
        it != mNativeSymbols.end()) {
        return it->second;
    }
    return data::FromKrakenSymbol(native);
}

Result<std::optional<types::Symbol>> KrakenTradingVenue::OurSymbol(
    std::string_view native, std::string_view source) const {
    auto symbol = ResolveSymbol(native);
    if (!symbol) {
        if (!mSymbolSet.empty()) {
            spdlog::debug("kraken {}: skipping unrecognised pair {}", source,
                          native);
            return std::optional<types::Symbol>{};
        }
        return std::unexpected(Malformed(symbol.error().message));
    }
    if (!IsOurs(*symbol)) {
        return std::optional<types::Symbol>{};
    }
    return std::optional<types::Symbol>{std::move(*symbol)};
}

bool KrakenTradingVenue::IsOurs(const types::Symbol& symbol) const {
    return mSymbolSet.empty() || mSymbolSet.contains(symbol);
}

void KrakenTradingVenue::Remember(const std::string& order_id,
                                  const std::string& client_id) {
    if (order_id.empty() || client_id.empty()) {
        return;
    }
    std::lock_guard lock(mRememberMutex);
    if (!mClientIds.insert_or_assign(order_id, client_id).second) {
        return;
    }
    mRememberOrder.push_back(order_id);
    if (mRememberOrder.size() > kRememberedOrders) {
        mClientIds.erase(mRememberOrder.front());
        mRememberOrder.pop_front();
    }
}

std::string KrakenTradingVenue::ClientIdOf(const std::string& order_id) {
    std::lock_guard lock(mRememberMutex);
    if (const auto it = mClientIds.find(order_id); it != mClientIds.end()) {
        return it->second;
    }
    return {};
}

std::string KrakenTradingVenue::NextClientOrderId() {
    return "kst" + mClientTag + std::to_string(mNextClientId++);
}

Result<json> KrakenTradingVenue::SendOnce(const std::string& method,
                                          const Params& params, CallCost cost) {
    if (mCredentials.api_key.empty()) {
        return MakeError(ErrorCategory::kAuth, "kraken: empty API key");
    }
    if (!mDecodedSecret) {
        return std::unexpected(mDecodedSecret.error());
    }
    util::RateLimiter& limiter =
        cost.bucket == Bucket::kOrder ? mOrderLimiter : mCounterLimiter;
    for (int i = 0; i < cost.tokens; ++i) {
        limiter.Acquire();
    }

    const std::string uri_path = "/0/private/" + method;
    Result<net::HttpResponse> response =
        MakeError(ErrorCategory::kInternal, "not sent");
    {
        std::lock_guard lock(mSendMutex);
        const auto micros = static_cast<std::uint64_t>(std::max<std::int64_t>(
            1, std::chrono::duration_cast<std::chrono::microseconds>(
                   mOptions.clock().time_since_epoch())
                   .count()));
        mLastNonce = std::max(micros, mLastNonce + 1);
        const std::string nonce = std::to_string(mLastNonce);

        Params all;
        all.reserve(params.size() + 1);
        all.emplace_back("nonce", nonce);
        all.insert(all.end(), params.begin(), params.end());
        const std::string post_data = FormEncode(all);
        const std::string sign =
            SignWithDecodedSecret(uri_path, nonce, post_data, *mDecodedSecret);

        net::HttpRequest request;
        request.method = net::HttpMethod::kPost;
        request.url = mOptions.rest_base_url + uri_path;
        request.body = post_data;
        request.headers["Content-Type"] = "application/x-www-form-urlencoded";
        request.headers["API-Key"] = mCredentials.api_key;
        request.headers["API-Sign"] = sign;
        response = mHttp->Send(request);
    }

    if (!response) {
        if (response.error().category == ErrorCategory::kRateLimit) {
            limiter.Penalize();
        }
        spdlog::debug("kraken {}: {}", method, response.error().ToString());
        return std::unexpected(response.error());
    }
    auto body = util::ParseJson(response->body);
    if (!body) {
        return std::unexpected(body.error());
    }
    if (!body->is_object()) {
        return std::unexpected(Malformed("response is not an object"));
    }
    if (const auto errors = body->find("error");
        errors != body->end() && errors->is_array()) {
        for (const auto& entry : *errors) {
            if (!entry.is_string()) {
                continue;
            }
            const auto code = entry.get<std::string>();
            if (code.empty() || code[0] != 'E') {
                continue;  // 'W' entries are warnings
            }
            const auto category = data::ClassifyKrakenError(code);
            if (category == ErrorCategory::kRateLimit) {
                (code.starts_with("EOrder:") ? mOrderLimiter : limiter)
                    .Penalize();
            }
            return MakeError(category, "kraken " + method + ": " + code,
                             response->status, code);
        }
    }
    const auto result = body->find("result");
    if (result == body->end()) {
        return std::unexpected(Malformed("response has no result"));
    }
    return *result;
}

Result<json> KrakenTradingVenue::PostPrivate(const std::string& method,
                                             const Params& params,
                                             CallCost cost, bool idempotent) {
    auto call = [&] { return SendOnce(method, params, cost); };
    if (!idempotent) {
        return call();
    }
    return util::Retry(mOptions.retry, call, mOptions.sleeper);
}

Result<KrakenTradingVenue::Params> KrakenTradingVenue::OrderParams(
    const types::OrderRequest& raw, std::string& client_id) {
    if (raw.time_in_force == types::TimeInForce::kDay) {
        return MakeError(ErrorCategory::kUnsupported,
                         "kraken has no day orders; use GTC, IOC or FOK");
    }
    if (raw.type == types::OrderType::kStop &&
        raw.time_in_force != types::TimeInForce::kGtc) {
        return MakeError(ErrorCategory::kUnsupported,
                         "kraken stop orders take no time in force");
    }
    auto symbol = types::NormalizeSymbol(raw.symbol);
    if (!symbol) {
        return std::unexpected(symbol.error());
    }
    if (!mDataSource) {
        return MakeError(ErrorCategory::kInternal,
                         "kraken venue has no data source for symbol rules");
    }
    auto info = mDataSource->GetSymbolInfo(*symbol);
    if (!info) {
        return std::unexpected(info.error());
    }
    types::OrderRequest request = raw;
    request.symbol = *symbol;
    auto ruled = ApplySymbolRules(request, *info);
    if (!ruled) {
        return std::unexpected(ruled.error());
    }
    auto native = data::ToKrakenSymbol(*symbol);
    if (!native) {
        return std::unexpected(native.error());
    }
    client_id =
        raw.client_order_id.empty() ? NextClientOrderId() : raw.client_order_id;
    if (!ValidClientOrderId(client_id)) {
        return MakeError(
            ErrorCategory::kInvalidArgument,
            "kraken client order IDs are a UUID or 1-18 letters, digits, '-' "
            "or '_': '" +
                client_id + "'");
    }

    Params params = {
        {"pair", *native},
        {"type", ruled->side == types::Side::kBuy ? "buy" : "sell"}};
    switch (ruled->type) {
        case types::OrderType::kMarket:
            params.emplace_back("ordertype", "market");
            break;
        case types::OrderType::kLimit:
            params.emplace_back("ordertype", "limit");
            params.emplace_back(
                "price", AtScaleOf(*ruled->limit_price, info->tick_size));
            params.emplace_back(
                "timeinforce",
                ruled->time_in_force == types::TimeInForce::kIoc   ? "IOC"
                : ruled->time_in_force == types::TimeInForce::kFok ? "FOK"
                                                                   : "GTC");
            break;
        case types::OrderType::kStop:
            params.emplace_back("ordertype", "stop-loss");
            params.emplace_back("price",
                                AtScaleOf(*ruled->stop_price, info->tick_size));
            break;
    }
    params.emplace_back("volume", AtScaleOf(ruled->quantity, info->lot_size));
    // fciq: the fee in the quote asset on a sell too, where Kraken's default
    // takes it in the base asset. Fills report the fee in the quote asset
    // either way, so this keeps what they say true of the holdings.
    params.emplace_back(
        "oflags", ruled->type == types::OrderType::kLimit && ruled->post_only
                      ? "post,fciq"
                      : "fciq");
    params.emplace_back("cl_ord_id", client_id);
    return params;
}

Result<void> KrakenTradingVenue::TestOrder(const types::OrderRequest& request) {
    std::string client_id;
    auto params = OrderParams(request, client_id);
    if (!params) {
        return std::unexpected(params.error());
    }
    params->emplace_back("validate", "true");
    auto result = PostPrivate("AddOrder", *params, kOrderCall, false);
    if (!result) {
        return std::unexpected(result.error());
    }
    return {};
}

Result<types::OrderStatus> KrakenTradingVenue::LookupClientOrderIn(
    const std::string& method, const char* bucket, const Params& params,
    CallCost cost, const std::string& client_id,
    std::optional<types::Timestamp> not_before) {
    auto result = PostPrivate(method, params, cost, true);
    if (!result) {
        return std::unexpected(result.error());
    }
    const auto map = result->find(bucket);
    if (!result->is_object() || map == result->end() || !map->is_object()) {
        return std::unexpected(
            Malformed(std::string(method) + ": missing \"" + bucket + '"'));
    }
    for (const auto& [order_id, entry] : map->items()) {
        // Require an exact cl_ord_id match so a manual order without cl_ord_id
        // (or a response that ignored the filter) is never mistaken for ours.
        if (OptionalText(entry, "cl_ord_id") != client_id) {
            continue;
        }
        auto parsed = ParseOrder(order_id, entry);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        // Kraken keeps cl_ord_id unique only among open orders; skip an older
        // closed order that reused a caller-supplied client_order_id.
        if (not_before && parsed->created_time.time_since_epoch().count() > 0 &&
            parsed->created_time < *not_before) {
            continue;
        }
        Remember(parsed->order_id, parsed->client_order_id);
        return parsed;
    }
    return MakeError(ErrorCategory::kNotFound,
                     "kraken: no order with cl_ord_id " + client_id);
}

Result<types::OrderStatus> KrakenTradingVenue::LookupClientOrder(
    const std::string& client_id, std::optional<types::Timestamp> not_before) {
    auto open =
        LookupClientOrderIn("OpenOrders", "open", {{"cl_ord_id", client_id}},
                            kOpenOrdersCall, client_id, not_before);
    if (open || open.error().category != ErrorCategory::kNotFound) {
        return open;
    }
    Params closed_params = {{"cl_ord_id", client_id}};
    if (not_before) {
        const std::int64_t start_sec =
            std::max<std::int64_t>(0, types::ToEpochSeconds(*not_before));
        closed_params.emplace_back("start", std::to_string(start_sec));
    }
    return LookupClientOrderIn("ClosedOrders", "closed", closed_params,
                               kClosedOrdersCall, client_id, not_before);
}

Result<types::OrderAcknowledgement> KrakenTradingVenue::PlaceOrder(
    const types::OrderRequest& request) {
    std::string client_id;
    auto params = OrderParams(request, client_id);
    if (!params) {
        return std::unexpected(params.error());
    }
    // Generated `kst<tag><n>` IDs carry a per-process tag and monotonic counter
    // so they cannot collide with an older closed order; bound ClosedOrders by
    // time only when the caller supplied `client_order_id`, avoiding false
    // misses if the local clock runs ahead of Kraken's.
    const std::optional<types::Timestamp> not_before =
        request.client_order_id.empty()
            ? std::nullopt
            : std::optional<types::Timestamp>(mOptions.clock() -
                                              kClosedLookupSlack);
    auto result = PostPrivate("AddOrder", *params, kOrderCall, false);
    std::optional<types::Error> send_error;
    if (!result) {
        if (!OutcomeUnknown(result.error())) {
            return std::unexpected(result.error());
        }
        send_error = result.error();
    } else {
        auto txids = Member(*result, "txid");
        if (txids && txids->is_array() && !txids->empty() &&
            (*txids)[0].is_string()) {
            const std::string order_id = (*txids)[0].get<std::string>();
            Remember(order_id, client_id);
            return types::OrderAcknowledgement{
                order_id, client_id, types::OrderState::kNew, mOptions.clock()};
        }
        // HTTP 2xx, so Kraken may have placed the order even though its body
        // lacked txid[0]: look it up before reporting a parse error.
        send_error =
            txids ? Malformed("AddOrder: missing txid[0]") : txids.error();
    }

    spdlog::warn(
        "kraken: placing {} ended without an answer ({}); looking it up",
        client_id, send_error->ToString());
    std::optional<types::Error> lookup_failure;
    for (int look = 0; look < 2; ++look) {
        if (look > 0) {
            mOptions.sleeper(kLookupPause);
        }
        auto found = LookupClientOrder(client_id, not_before);
        if (found) {
            return types::OrderAcknowledgement{
                found->order_id, found->client_order_id, found->state,
                types::IsActive(found->state) ? found->created_time
                                              : found->updated_time};
        }
        if (found.error().category != ErrorCategory::kNotFound) {
            lookup_failure = found.error();
            break;
        }
    }
    types::Error error = std::move(*send_error);
    error.message +=
        lookup_failure
            ? " (order " + client_id + " could not be looked up either: " +
                  lookup_failure->ToString() + "; it may exist)"
            : " (order " + client_id +
                  " was not found afterwards, but may still appear)";
    return std::unexpected(std::move(error));
}

Result<types::OrderStatus> KrakenTradingVenue::ParseOrder(
    const std::string& order_id, const json& order,
    std::optional<types::Symbol> known_symbol) {
    auto status_str = TextMember(order, "status");
    auto quantity = DecimalMember(order, "vol");
    auto filled = DecimalMember(order, "vol_exec");
    auto descr = Member(order, "descr");
    if (!status_str) return std::unexpected(status_str.error());
    if (!quantity) return std::unexpected(quantity.error());
    if (!filled) return std::unexpected(filled.error());
    if (!descr || !descr->is_object()) {
        return std::unexpected(Malformed("order \"descr\" is not an object"));
    }

    auto raw_side = TextMember(*descr, "type");
    auto raw_type = TextMember(*descr, "ordertype");
    if (!raw_side) return std::unexpected(raw_side.error());
    if (!raw_type) return std::unexpected(raw_type.error());

    types::Symbol symbol;
    if (known_symbol) {
        symbol = std::move(*known_symbol);
    } else {
        auto raw_pair = TextMember(*descr, "pair");
        if (!raw_pair) return std::unexpected(raw_pair.error());
        auto resolved = ResolveSymbol(*raw_pair);
        if (!resolved) {
            return std::unexpected(resolved.error());
        }
        symbol = std::move(*resolved);
    }
    auto side = SideFrom(*raw_side);
    if (!side) {
        return std::unexpected(side.error());
    }
    auto type = OrderTypeFrom(*raw_type);
    if (!type) {
        return std::unexpected(type.error());
    }
    auto tif = TimeInForceFrom(OptionalText(order, "timeinforce"));
    if (!tif) {
        return std::unexpected(tif.error());
    }
    auto state = OrderStateFrom(*status_str, *filled);
    if (!state) {
        return std::unexpected(state.error());
    }

    types::OrderStatus out;
    out.order_id = order_id;
    out.client_order_id = OptionalText(order, "cl_ord_id");
    if (out.client_order_id.empty()) {
        out.client_order_id = ClientIdOf(order_id);
    } else {
        Remember(order_id, out.client_order_id);
    }
    out.symbol = std::move(symbol);
    out.side = *side;
    out.type = *type;
    out.time_in_force = *tif;
    out.quantity = *quantity;
    out.filled_quantity = *filled;
    out.state = *state;

    const auto descr_price = OptionalDecimal(*descr, "price");
    const auto descr_price2 = OptionalDecimal(*descr, "price2");
    const auto root_stop = OptionalDecimal(order, "stopprice");
    if (!descr_price) return std::unexpected(descr_price.error());
    if (!descr_price2) return std::unexpected(descr_price2.error());
    if (!root_stop) return std::unexpected(root_stop.error());

    if (*type == types::OrderType::kLimit && descr_price->Sign() > 0) {
        out.limit_price = *descr_price;
    } else if (*type == types::OrderType::kStop) {
        if (descr_price->Sign() > 0) {
            out.stop_price = *descr_price;
        } else if (root_stop->Sign() > 0) {
            out.stop_price = *root_stop;
        }
        if ((*raw_type == "stop-loss-limit" ||
             *raw_type == "take-profit-limit") &&
            descr_price2->Sign() > 0) {
            out.limit_price = *descr_price2;
        }
    }

    if (!filled->IsZero()) {
        const auto root_price = OptionalDecimal(order, "price");
        const auto avg_price = OptionalDecimal(order, "avg_price");
        const auto cost = OptionalDecimal(order, "cost");
        if (!root_price) return std::unexpected(root_price.error());
        if (!avg_price) return std::unexpected(avg_price.error());
        if (!cost) return std::unexpected(cost.error());
        if (root_price->Sign() > 0) {
            out.average_fill_price = *root_price;
        } else if (avg_price->Sign() > 0) {
            out.average_fill_price = *avg_price;
        } else if (cost->Sign() > 0) {
            out.average_fill_price = types::Decimal::Divide(
                *cost, *filled, 8, types::Rounding::kNearest);
        }
    }

    if (order.contains("opentm") && !order["opentm"].is_null()) {
        auto opened = TimestampFromSeconds(order["opentm"], "opentm");
        if (!opened) {
            return std::unexpected(opened.error());
        }
        out.created_time = *opened;
    }
    out.updated_time = out.created_time;
    // Kraken's order response field is `closetm` (while the ClosedOrders
    // request parameter is `closetime`); accept `closetime` as a fallback.
    for (const char* close_key : {"closetm", "closetime"}) {
        if (order.contains(close_key) && !order[close_key].is_null()) {
            auto closed = TimestampFromSeconds(order[close_key], close_key);
            if (!closed) {
                return std::unexpected(closed.error());
            }
            if (closed->time_since_epoch().count() > 0) {
                out.updated_time = *closed;
                break;
            }
        }
    }
    if (const std::string reason = OptionalText(order, "reason");
        !reason.empty()) {
        out.reason = reason;
        // A post-only order that would have taken is accepted and then
        // cancelled by Kraken: refused, as other venues report it, not a
        // cancel anyone asked for.
        if (out.state == types::OrderState::kCancelled && filled->IsZero() &&
            reason.starts_with("Post only")) {
            out.state = types::OrderState::kRejected;
        }
    }
    return out;
}

Result<CancelOutcome> KrakenTradingVenue::CancelOutcomeOf(
    const std::string& order_id) {
    auto order = GetOrder(order_id);
    if (!order) {
        return std::unexpected(order.error());
    }
    switch (order->state) {
        case types::OrderState::kFilled:
            return CancelOutcome::kAlreadyFilled;
        case types::OrderState::kCancelled:
        case types::OrderState::kRejected:
        case types::OrderState::kExpired:
            return CancelOutcome::kAlreadyClosed;
        default:
            return MakeError(
                ErrorCategory::kServer,
                "kraken: order " + order_id + " is still open after a cancel");
    }
}

Result<CancelOutcome> KrakenTradingVenue::CancelOrder(
    const std::string& order_id) {
    if (!ValidOrderId(order_id)) {
        return MakeError(ErrorCategory::kInvalidArgument,
                         "kraken: invalid order ID '" + order_id + "'");
    }
    auto result =
        PostPrivate("CancelOrder", {{"txid", order_id}}, kOrderCall, false);
    if (result) {
        if (!result->is_object()) {
            return CancelOutcomeOf(order_id);
        }
        const bool pending = result->contains("pending") &&
                             (*result)["pending"].is_boolean() &&
                             (*result)["pending"].get<bool>();
        if (result->contains("count") && !pending) {
            auto count = util::JsonDecimal((*result)["count"]);
            if (!count || count->IsZero()) {
                return CancelOutcomeOf(order_id);
            }
        }
        return CancelOutcome::kCancelled;
    }
    if (IsUnknownOrderError(result.error())) {
        return CancelOutcomeOf(order_id);
    }
    if (OutcomeUnknown(result.error())) {
        auto order = GetOrder(order_id);
        if (order && order->state == types::OrderState::kCancelled) {
            return CancelOutcome::kCancelled;
        }
        if (order && order->state == types::OrderState::kFilled) {
            return CancelOutcome::kAlreadyFilled;
        }
        if (order && !types::IsActive(order->state)) {
            return CancelOutcome::kAlreadyClosed;
        }
    }
    return std::unexpected(result.error());
}

Result<std::vector<std::string>> KrakenTradingVenue::CancelAll() {
    auto open = GetOpenOrders();
    if (!open) {
        return std::unexpected(open.error());
    }
    std::vector<std::string> cancelled;
    std::optional<types::Error> first_error;
    std::size_t failed = 0;
    for (const auto& order : *open) {
        auto outcome = CancelOrder(order.order_id);
        if (!outcome) {
            ++failed;
            if (!first_error) {
                first_error = outcome.error();
            }
            continue;
        }
        if (*outcome == CancelOutcome::kCancelled) {
            cancelled.push_back(order.order_id);
        }
    }
    if (first_error) {
        first_error->message +=
            " (" + std::to_string(failed) + " of " +
            std::to_string(open->size()) + " cancels failed; " +
            std::to_string(cancelled.size()) + " cancelled)";
        return std::unexpected(std::move(*first_error));
    }
    return cancelled;
}

Result<std::vector<types::OrderStatus>> KrakenTradingVenue::GetOpenOrders() {
    auto result = PostPrivate("OpenOrders", {}, kOpenOrdersCall, true);
    if (!result) {
        return std::unexpected(result.error());
    }
    auto open = Member(*result, "open");
    if (!open) {
        return std::unexpected(open.error());
    }
    if (!open->is_object()) {
        return std::unexpected(Malformed("OpenOrders: \"open\" is not a map"));
    }
    std::vector<types::OrderStatus> out;
    for (const auto& [order_id, entry] : open->items()) {
        auto descr = Member(entry, "descr");
        if (!descr || !descr->is_object()) {
            return std::unexpected(
                Malformed("order \"descr\" is not an object"));
        }
        auto raw_pair = TextMember(*descr, "pair");
        if (!raw_pair) {
            return std::unexpected(raw_pair.error());
        }
        auto symbol = OurSymbol(*raw_pair, "OpenOrders");
        if (!symbol) {
            return std::unexpected(symbol.error());
        }
        if (!*symbol) {
            continue;
        }
        auto parsed = ParseOrder(order_id, entry, std::move(**symbol));
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        out.push_back(std::move(*parsed));
    }
    return out;
}

Result<types::OrderStatus> KrakenTradingVenue::GetOrder(
    const std::string& order_id) {
    if (!ValidOrderId(order_id)) {
        return MakeError(ErrorCategory::kInvalidArgument,
                         "kraken: invalid order ID '" + order_id + "'");
    }
    auto result = PostPrivate("QueryOrders", {{"txid", order_id}},
                              kQueryOrdersCall, true);
    if (!result) {
        if (IsUnknownOrderError(result.error())) {
            return MakeError(ErrorCategory::kNotFound,
                             "kraken: unknown order " + order_id);
        }
        return std::unexpected(result.error());
    }
    if (!result->is_object()) {
        return std::unexpected(Malformed("QueryOrders is not an object"));
    }
    const auto it = result->find(order_id);
    if (it == result->end()) {
        return MakeError(ErrorCategory::kNotFound,
                         "kraken: unknown order " + order_id);
    }
    return ParseOrder(order_id, *it);
}

Result<types::Fill> KrakenTradingVenue::ParseTrade(const std::string& trade_id,
                                                   const json& trade,
                                                   const types::Symbol& symbol,
                                                   types::Timestamp time) {
    auto order_id = TextMember(trade, "ordertxid");
    auto raw_side = TextMember(trade, "type");
    auto price = DecimalMember(trade, "price");
    auto quantity = DecimalMember(trade, "vol");
    auto fee = DecimalMember(trade, "fee");
    if (!order_id) return std::unexpected(order_id.error());
    if (!raw_side) return std::unexpected(raw_side.error());
    if (!price) return std::unexpected(price.error());
    if (!quantity) return std::unexpected(quantity.error());
    if (!fee) return std::unexpected(fee.error());

    auto side = SideFrom(*raw_side);
    if (!side) {
        return std::unexpected(side.error());
    }
    types::Liquidity liquidity = types::Liquidity::kTaker;
    if (trade.contains("maker") && !trade["maker"].is_null()) {
        if (!trade["maker"].is_boolean()) {
            return std::unexpected(Malformed("\"maker\" is not a boolean"));
        }
        liquidity = trade["maker"].get<bool>() ? types::Liquidity::kMaker
                                               : types::Liquidity::kTaker;
    }

    // TradesHistory gives `fee` in the pair's quote currency. Kestrel's orders
    // carry `fciq`, so the fee is also taken in it (an order placed elsewhere
    // that sold with Kraken's default paid it in the base asset).
    std::string fee_asset;
    if (const auto parts = types::SplitSymbol(symbol); parts && parts->quote) {
        fee_asset = *parts->quote;
    }

    types::Fill fill;
    fill.fill_id = trade_id;
    fill.order_id = *order_id;
    fill.client_order_id = ClientIdOf(*order_id);
    fill.symbol = symbol;
    fill.side = *side;
    fill.quantity = *quantity;
    fill.price = *price;
    fill.fee = *fee;
    fill.fee_asset = std::move(fee_asset);
    fill.liquidity = liquidity;
    fill.time = time;
    return fill;
}

Result<std::vector<types::Fill>> KrakenTradingVenue::GetFills(
    types::Timestamp since) {
    // TradesHistory `start` is in whole seconds (and exclusive for txids); step
    // back 1 second so fractional-second fills at `since` are never cut off,
    // then filter precisely by `fill.time >= since`.
    const std::int64_t start_sec =
        std::max<std::int64_t>(0, types::ToEpochSeconds(since) - 1);

    std::vector<types::Fill> out;
    std::set<std::string> seen_trades;
    std::set<std::string> seen_cursors;
    std::string cursor;
    for (;;) {
        Params params = {{"start", std::to_string(start_sec)},
                         {"limit", std::to_string(kTradesPageSize)},
                         {"with_cursor", "true"}};
        if (!cursor.empty()) {
            params.emplace_back("cursor", cursor);
        }
        auto result =
            PostPrivate("TradesHistory", params, kTradesHistoryCall, true);
        if (!result) {
            return std::unexpected(result.error());
        }
        auto trades = Member(*result, "trades");
        if (!trades) {
            return std::unexpected(trades.error());
        }
        if (!trades->is_object()) {
            return std::unexpected(
                Malformed("TradesHistory: \"trades\" is not a map"));
        }
        // An empty page means no more trades match `(start_sec, ...)`, even if
        // a response still included a trailing `cursor` field.
        if (trades->empty()) {
            break;
        }

        bool any_new = false;
        bool any_at_or_after_since = false;
        for (const auto& [trade_id, row] : trades->items()) {
            const bool is_new = seen_trades.insert(trade_id).second;
            any_new |= is_new;
            auto time_json = Member(row, "time");
            if (!time_json) {
                return std::unexpected(time_json.error());
            }
            auto trade_time = TimestampFromSeconds(*time_json, "time");
            if (!trade_time) {
                return std::unexpected(trade_time.error());
            }
            if (*trade_time < since) {
                continue;
            }
            any_at_or_after_since = true;
            if (!is_new) {
                continue;
            }

            auto raw_pair = TextMember(row, "pair");
            if (!raw_pair) {
                return std::unexpected(raw_pair.error());
            }
            auto symbol = OurSymbol(*raw_pair, "TradesHistory");
            if (!symbol) {
                return std::unexpected(symbol.error());
            }
            if (!*symbol) {
                continue;
            }
            auto fill = ParseTrade(trade_id, row, **symbol, *trade_time);
            if (!fill) {
                return std::unexpected(fill.error());
            }
            out.push_back(std::move(*fill));
        }

        // TradesHistory is newest-first: once an entire page is older than
        // `since`, every later page is older too.
        if (!any_at_or_after_since) {
            break;
        }
        auto next = NextTradesCursor(*result);
        if (!next) {
            return std::unexpected(next.error());
        }
        if (next->empty()) {
            break;
        }
        // Never return a partial `Ok` when more pages remain: callers advance
        // their `since` cursor to the newest fill returned, which would orphan
        // the unread older pages.
        if (!any_new || !seen_cursors.insert(*next).second) {
            return std::unexpected(
                Malformed("TradesHistory cursor did not advance"));
        }
        cursor = std::move(*next);
    }

    std::ranges::sort(out, [](const types::Fill& a, const types::Fill& b) {
        return a.time != b.time ? a.time < b.time : a.fill_id < b.fill_id;
    });
    return out;
}

Result<std::vector<types::Position>> KrakenTradingVenue::GetPositions() {
    return std::vector<types::Position>{};
}

Result<std::vector<types::Balance>> KrakenTradingVenue::GetBalances() {
    auto result = PostPrivate("BalanceEx", {}, kBalanceCall, true);
    if (!result) {
        return std::unexpected(result.error());
    }
    if (!result->is_object()) {
        return std::unexpected(Malformed("BalanceEx is not an object"));
    }

    std::vector<types::Balance> out;
    for (const auto& [raw_asset, row] : result->items()) {
        if (!row.is_object()) {
            return std::unexpected(Malformed("BalanceEx entry for " +
                                             raw_asset + " is not an object"));
        }
        auto total = DecimalMember(row, "balance");
        auto hold = OptionalDecimal(row, "hold_trade");
        auto credit = OptionalDecimal(row, "credit");
        auto credit_used = OptionalDecimal(row, "credit_used");
        if (!total) return std::unexpected(total.error());
        if (!hold) return std::unexpected(hold.error());
        if (!credit) return std::unexpected(credit.error());
        if (!credit_used) return std::unexpected(credit_used.error());

        types::Balance balance;
        balance.asset = data::FromKrakenAsset(raw_asset);
        balance.total = *total;
        balance.locked = *hold;
        balance.available =
            std::max(types::Decimal(), *total + *credit - *credit_used - *hold);
        if (!balance.total.IsZero() || !balance.locked.IsZero() ||
            !balance.available.IsZero()) {
            out.push_back(std::move(balance));
        }
    }
    std::ranges::sort(out, {}, &types::Balance::asset);
    return out;
}

}  // namespace kestrel::trading
