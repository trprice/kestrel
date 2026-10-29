#include "kestrel/trading/binanceus_trading_venue.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <random>
#include <set>

#include "kestrel/data/symbols.hpp"
#include "kestrel/net/timed_http_client.hpp"
#include "kestrel/trading/order_rules.hpp"
#include "kestrel/util/json.hpp"

namespace kestrel::trading {

using nlohmann::json;
using types::ErrorCategory;
using types::MakeError;
using types::Result;

namespace {

// Request weights from the Binance.US API documentation.
constexpr int kAccountWeight = 10;
constexpr int kOpenOrdersWeight = 3;  // with a symbol
constexpr int kOrderWeight = 4;
constexpr int kMyTradesWeight = 10;
constexpr int kNewOrderWeight = 1;  // also the test endpoint
constexpr int kCancelWeight = 1;
constexpr int kCancelAllWeight = 1;  // with a symbol

// How long to wait before the second look for an order whose placement or
// cancel ended without a clear answer.
constexpr auto kLookupPause = std::chrono::seconds(1);

// How many order -> symbol entries are kept. A market maker places tens of
// thousands of orders a day; an order that has dropped out is still found, by
// asking each configured symbol.
constexpr std::size_t kRememberedOrders = 10'000;

// Binance accepts client order IDs matching ^[a-zA-Z0-9-_]{1,36}$.
bool ValidClientOrderId(const std::string& id) {
    if (id.empty() || id.size() > 36) {
        return false;
    }
    return std::ranges::all_of(id, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}

// A failure after which the request may or may not have taken effect.
bool OutcomeUnknown(const types::Error& error) {
    return error.category == ErrorCategory::kNetwork ||
           error.category == ErrorCategory::kTimeout ||
           error.category == ErrorCategory::kServer;
}

// `value`, a multiple of `increment`, written with the increment's decimals
// and no more: Binance refuses a price or quantity with more decimals than the
// asset allows (-1111), however many of them are zeros.
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
    for (int i = 0; i < 8; ++i) {
        tag += kAlphabet[pick(device)];
    }
    return tag;
}

// myTrades accepts a startTime/endTime range of at most 24 hours.
constexpr auto kTradeWindow = std::chrono::hours(24);
constexpr std::size_t kTradePage = 1000;

types::Error Malformed(const std::string& what) {
    return MakeError(ErrorCategory::kParse, "binanceus: " + what).error();
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

Result<std::int64_t> IntegerMember(const json& object, const char* key) {
    auto value = Member(object, key);
    if (!value) {
        return std::unexpected(value.error());
    }
    if (!value->is_number_integer()) {
        return std::unexpected(
            Malformed(std::string("\"") + key + "\" is not an integer"));
    }
    return value->get<std::int64_t>();
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

types::OrderType OrderTypeFrom(const std::string& type) {
    if (type == "MARKET") {
        return types::OrderType::kMarket;
    }
    if (type == "STOP_LOSS" || type == "STOP_LOSS_LIMIT" ||
        type == "TAKE_PROFIT" || type == "TAKE_PROFIT_LIMIT") {
        return types::OrderType::kStop;
    }
    return types::OrderType::kLimit;  // LIMIT, LIMIT_MAKER
}

types::TimeInForce TimeInForceFrom(const std::string& tif) {
    if (tif == "IOC") {
        return types::TimeInForce::kIoc;
    }
    if (tif == "FOK") {
        return types::TimeInForce::kFok;
    }
    return types::TimeInForce::kGtc;
}

// A PENDING_CANCEL order is still working, so it reports by what it has
// filled.
types::OrderState OrderStateFrom(const std::string& status,
                                 const types::Decimal& filled) {
    if (status == "FILLED") {
        return types::OrderState::kFilled;
    }
    if (status == "CANCELED") {
        return types::OrderState::kCancelled;
    }
    if (status == "REJECTED") {
        return types::OrderState::kRejected;
    }
    if (status == "EXPIRED" || status == "EXPIRED_IN_MATCH") {
        return types::OrderState::kExpired;
    }
    if (status == "PARTIALLY_FILLED" || !filled.IsZero()) {
        return types::OrderState::kPartiallyFilled;
    }
    return types::OrderState::kNew;
}

Result<types::OrderStatus> ParseOrder(const json& order) {
    auto native = TextMember(order, "symbol");
    auto id = IntegerMember(order, "orderId");
    auto status = TextMember(order, "status");
    auto side = TextMember(order, "side");
    auto quantity = DecimalMember(order, "origQty");
    auto filled = DecimalMember(order, "executedQty");
    if (!native) return std::unexpected(native.error());
    if (!id) return std::unexpected(id.error());
    if (!status) return std::unexpected(status.error());
    if (!side) return std::unexpected(side.error());
    if (!quantity) {
        return std::unexpected(quantity.error());
    }
    if (!filled) {
        return std::unexpected(filled.error());
    }
    auto symbol = data::FromBinanceUSSymbol(*native);
    if (!symbol) {
        return std::unexpected(symbol.error());
    }

    types::OrderStatus out;
    out.order_id = std::to_string(*id);
    out.client_order_id = OptionalText(order, "clientOrderId");
    out.symbol = *symbol;
    out.side = *side == "SELL" ? types::Side::kSell : types::Side::kBuy;
    out.type = OrderTypeFrom(OptionalText(order, "type"));
    out.time_in_force = TimeInForceFrom(OptionalText(order, "timeInForce"));
    out.quantity = *quantity;
    out.filled_quantity = *filled;
    out.state = OrderStateFrom(*status, *filled);

    if (auto price = DecimalMember(order, "price"); price && !price->IsZero()) {
        out.limit_price = *price;
    }
    if (auto stop = DecimalMember(order, "stopPrice");
        stop && !stop->IsZero()) {
        out.stop_price = *stop;
    }
    if (!filled->IsZero()) {
        if (auto quote = DecimalMember(order, "cummulativeQuoteQty"); quote) {
            out.average_fill_price = types::Decimal::Divide(
                *quote, *filled, 8, types::Rounding::kNearest);
        }
    }
    if (auto time = IntegerMember(order, "time"); time) {
        out.created_time = types::FromEpochMillis(*time);
    }
    out.updated_time = out.created_time;
    if (auto updated = IntegerMember(order, "updateTime"); updated) {
        out.updated_time = types::FromEpochMillis(*updated);
    }
    return out;
}

Result<types::Fill> ParseTrade(const json& trade, const std::string& native,
                               const types::Symbol& symbol) {
    auto id = IntegerMember(trade, "id");
    auto order_id = IntegerMember(trade, "orderId");
    auto price = DecimalMember(trade, "price");
    auto quantity = DecimalMember(trade, "qty");
    auto fee = DecimalMember(trade, "commission");
    auto time = IntegerMember(trade, "time");
    auto buyer = Member(trade, "isBuyer");
    auto maker = Member(trade, "isMaker");
    if (!id) return std::unexpected(id.error());
    if (!order_id) return std::unexpected(order_id.error());
    if (!price) return std::unexpected(price.error());
    if (!quantity) return std::unexpected(quantity.error());
    if (!fee) return std::unexpected(fee.error());
    if (!time) return std::unexpected(time.error());
    if (!buyer || !buyer->is_boolean()) {
        return std::unexpected(Malformed("\"isBuyer\" is not a boolean"));
    }
    if (!maker || !maker->is_boolean()) {
        return std::unexpected(Malformed("\"isMaker\" is not a boolean"));
    }

    types::Fill fill;
    // Trade IDs are only unique within a symbol.
    fill.fill_id = native + ":" + std::to_string(*id);
    fill.order_id = std::to_string(*order_id);
    fill.symbol = symbol;
    fill.side = buyer->get<bool>() ? types::Side::kBuy : types::Side::kSell;
    fill.quantity = *quantity;
    fill.price = *price;
    fill.fee = *fee;
    fill.fee_asset = OptionalText(trade, "commissionAsset");
    fill.liquidity = maker->get<bool>() ? types::Liquidity::kMaker
                                        : types::Liquidity::kTaker;
    fill.time = types::FromEpochMillis(*time);
    return fill;
}

}  // namespace

BinanceUSTradingVenue::BinanceUSTradingVenue(
    config::BinanceUSCredentials credentials,
    std::shared_ptr<data::DataSource> data_source,
    std::vector<types::Symbol> symbols, std::shared_ptr<net::HttpClient> http,
    BinanceUSApi::Options options)
    : mCredentials(credentials),
      mDataSource(std::move(data_source)),
      mSymbols(std::move(symbols)),
      mApi(std::move(credentials),
           http ? std::move(http) : net::MakeTimedHttpClient(), options),
      mClock(options.clock),
      mSleep(options.sleeper),
      mOrderLimiter(options.orders_per_second, options.order_burst),
      mClientTag(RandomTag()) {}

void BinanceUSTradingVenue::Remember(const std::string& order_id,
                                     const types::Symbol& symbol) {
    std::lock_guard lock(mMutex);
    if (!mOrderSymbols.insert_or_assign(order_id, symbol).second) {
        return;  // already known
    }
    mRememberOrder.push_back(order_id);
    if (mRememberOrder.size() > kRememberedOrders) {
        mOrderSymbols.erase(mRememberOrder.front());
        mRememberOrder.pop_front();
    }
}

std::optional<types::Symbol> BinanceUSTradingVenue::SymbolOf(
    const std::string& order_id) {
    std::lock_guard lock(mMutex);
    if (auto it = mOrderSymbols.find(order_id); it != mOrderSymbols.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::string BinanceUSTradingVenue::NextClientOrderId() {
    return "kst-" + mClientTag + "-" + std::to_string(mNextClientId++);
}

Result<BinanceUSApi::Params> BinanceUSTradingVenue::OrderParams(
    const types::OrderRequest& raw, std::string& native,
    std::string& client_id) {
    if (raw.time_in_force == types::TimeInForce::kDay) {
        return MakeError(ErrorCategory::kUnsupported,
                         "binanceus has no day orders; use GTC, IOC or FOK");
    }
    if (raw.type == types::OrderType::kStop &&
        raw.time_in_force != types::TimeInForce::kGtc) {
        return MakeError(ErrorCategory::kUnsupported,
                         "binanceus stop orders take no time in force");
    }
    auto symbol = types::NormalizeSymbol(raw.symbol);
    if (!symbol) {
        return std::unexpected(symbol.error());
    }
    if (!mDataSource) {
        return MakeError(ErrorCategory::kInternal,
                         "binanceus venue has no data source for symbol rules");
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
    auto binance = data::ToBinanceUSSymbol(*symbol);
    if (!binance) {
        return std::unexpected(binance.error());
    }
    native = *binance;
    client_id =
        raw.client_order_id.empty() ? NextClientOrderId() : raw.client_order_id;
    if (!ValidClientOrderId(client_id)) {
        return MakeError(ErrorCategory::kInvalidArgument,
                         "binanceus client order IDs are 1-36 letters, "
                         "digits, '-' or '_': '" +
                             client_id + "'");
    }

    BinanceUSApi::Params params = {
        {"symbol", native},
        {"side", ruled->side == types::Side::kBuy ? "BUY" : "SELL"}};
    switch (ruled->type) {
        case types::OrderType::kMarket:
            params.emplace_back("type", "MARKET");
            break;
        case types::OrderType::kLimit:
            if (ruled->post_only) {
                // Refused with -2010 if it would match at once; no time in
                // force (it is always GTC).
                params.emplace_back("type", "LIMIT_MAKER");
                break;
            }
            params.emplace_back("type", "LIMIT");
            params.emplace_back(
                "timeInForce",
                ruled->time_in_force == types::TimeInForce::kIoc   ? "IOC"
                : ruled->time_in_force == types::TimeInForce::kFok ? "FOK"
                                                                   : "GTC");
            break;
        case types::OrderType::kStop:
            params.emplace_back("type", "STOP_LOSS");
            break;
    }
    params.emplace_back("quantity", AtScaleOf(ruled->quantity, info->lot_size));
    if (ruled->type == types::OrderType::kLimit) {
        params.emplace_back("price",
                            AtScaleOf(*ruled->limit_price, info->tick_size));
    }
    if (ruled->type == types::OrderType::kStop) {
        params.emplace_back("stopPrice",
                            AtScaleOf(*ruled->stop_price, info->tick_size));
    }
    params.emplace_back("newClientOrderId", client_id);
    params.emplace_back("newOrderRespType", "RESULT");
    return params;
}

Result<types::OrderStatus> BinanceUSTradingVenue::LookupClientOrder(
    const std::string& native, const std::string& client_id) {
    auto body =
        mApi.Signed(net::HttpMethod::kGet, "/api/v3/order",
                    {{"symbol", native}, {"origClientOrderId", client_id}},
                    kOrderWeight, true);
    if (!body) {
        return std::unexpected(body.error());
    }
    auto order = ParseOrder(*body);
    if (order) {
        Remember(order->order_id, order->symbol);
    }
    return order;
}

Result<void> BinanceUSTradingVenue::TestOrder(
    const types::OrderRequest& request) {
    std::string native;
    std::string client_id;
    auto params = OrderParams(request, native, client_id);
    if (!params) {
        return std::unexpected(params.error());
    }
    // Whether Binance counts test orders against the order rate is not
    // documented clearly; counting them here can only err on the safe side.
    mOrderLimiter.Acquire();
    auto body = mApi.Signed(net::HttpMethod::kPost, "/api/v3/order/test",
                            *params, kNewOrderWeight, false);
    if (!body) {
        return std::unexpected(body.error());
    }
    return {};
}

Result<types::OrderAcknowledgement> BinanceUSTradingVenue::PlaceOrder(
    const types::OrderRequest& request) {
    std::string native;
    std::string client_id;
    auto params = OrderParams(request, native, client_id);
    if (!params) {
        return std::unexpected(params.error());
    }
    mOrderLimiter.Acquire();
    auto body = mApi.Signed(net::HttpMethod::kPost, "/api/v3/order", *params,
                            kNewOrderWeight, false);
    if (!body) {
        if (!OutcomeUnknown(body.error())) {
            return std::unexpected(body.error());  // refused: nothing placed
        }
        // It may have been placed: find out rather than send it again.
        spdlog::warn(
            "binanceus: placing {} ended without an answer ({}); "
            "looking it up",
            client_id, body.error().ToString());
        std::optional<types::Error> lookup_failure;
        for (int look = 0; look < 2; ++look) {
            if (look > 0) {
                mSleep(kLookupPause);
            }
            auto found = LookupClientOrder(native, client_id);
            if (found) {
                // Its creation time is the transactTime a direct answer
                // would have carried.
                return types::OrderAcknowledgement{
                    found->order_id, found->client_order_id, found->state,
                    found->created_time};
            }
            if (found.error().category != ErrorCategory::kNotFound) {
                lookup_failure = found.error();
                break;
            }
        }
        types::Error error = body.error();
        error.message +=
            lookup_failure
                ? " (order " + client_id + " could not be looked up either: " +
                      lookup_failure->ToString() + "; it may exist)"
                : " (order " + client_id +
                      " was not found afterwards, but may still appear)";
        return std::unexpected(std::move(error));
    }
    auto order = ParseOrder(*body);
    if (!order) {
        return std::unexpected(order.error());
    }
    Remember(order->order_id, order->symbol);
    types::OrderAcknowledgement ack{order->order_id, order->client_order_id,
                                    order->state, order->updated_time};
    if (auto time = IntegerMember(*body, "transactTime"); time) {
        ack.time = types::FromEpochMillis(*time);
    }
    return ack;
}

Result<CancelOutcome> BinanceUSTradingVenue::CancelOutcomeOf(
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
            return MakeError(ErrorCategory::kServer,
                             "binanceus: order " + order_id +
                                 " is still open after a cancel");
    }
}

Result<CancelOutcome> BinanceUSTradingVenue::CancelOrder(
    const std::string& order_id) {
    auto symbol = SymbolOf(order_id);
    if (!symbol) {
        // Placed before this process started: find its symbol first.
        auto order = GetOrder(order_id);
        if (!order) {
            return std::unexpected(order.error());
        }
        symbol = order->symbol;
    }
    auto native = data::ToBinanceUSSymbol(*symbol);
    if (!native) {
        return std::unexpected(native.error());
    }
    auto body = mApi.Signed(net::HttpMethod::kDelete, "/api/v3/order",
                            {{"symbol", *native}, {"orderId", order_id}},
                            kCancelWeight, false);
    if (body) {
        return CancelOutcome::kCancelled;
    }
    if (body.error().category == ErrorCategory::kNotFound) {
        // -2011: not open any more (or never was). Which is it?
        return CancelOutcomeOf(order_id);
    }
    if (OutcomeUnknown(body.error())) {
        auto order = GetOrder(order_id);
        if (order && order->state == types::OrderState::kCancelled) {
            return CancelOutcome::kCancelled;
        }
        if (order && order->state == types::OrderState::kFilled) {
            return CancelOutcome::kAlreadyFilled;
        }
    }
    return std::unexpected(body.error());
}

Result<std::vector<std::string>> BinanceUSTradingVenue::CancelAll() {
    // Every symbol is tried even when one fails: this is what a kill switch
    // calls, and one bad symbol must not leave the others' orders working.
    std::vector<std::string> cancelled;
    std::optional<types::Error> first_failure;
    std::string failures;
    const auto fail = [&](const types::Symbol& symbol, types::Error error) {
        failures +=
            (failures.empty() ? "" : "; ") + symbol + ": " + error.ToString();
        if (!first_failure) {
            first_failure = std::move(error);
        }
    };
    for (const auto& symbol : mSymbols) {
        auto native = data::ToBinanceUSSymbol(symbol);
        if (!native) {
            fail(symbol, native.error());
            continue;
        }
        auto body = mApi.Signed(net::HttpMethod::kDelete, "/api/v3/openOrders",
                                {{"symbol", *native}}, kCancelAllWeight, false);
        if (!body) {
            if (body.error().category != ErrorCategory::kNotFound) {
                fail(symbol, body.error());
            }
            continue;  // kNotFound is -2011: nothing open on this symbol
        }
        if (!body->is_array()) {
            fail(symbol, Malformed("openOrders cancel is not an array"));
            continue;
        }
        for (const auto& row : *body) {
            // Order lists come back with their own shape; only plain orders
            // are counted.
            if (auto id = IntegerMember(row, "orderId"); id) {
                cancelled.push_back(std::to_string(*id));
            }
        }
    }
    if (first_failure) {
        // The category of the first failure, and everything in the message:
        // what failed, and what was cancelled regardless.
        types::Error error = std::move(*first_failure);
        error.message = "binanceus: cancel all failed for " + failures +
                        " (cancelled " + std::to_string(cancelled.size()) +
                        " order(s) on the other symbols";
        for (const auto& id : cancelled) {
            error.message += " " + id;
        }
        error.message += ")";
        return std::unexpected(std::move(error));
    }
    return cancelled;
}

Result<std::vector<types::OrderStatus>> BinanceUSTradingVenue::GetOpenOrders() {
    std::vector<types::OrderStatus> out;
    for (const auto& symbol : mSymbols) {
        auto native = data::ToBinanceUSSymbol(symbol);
        if (!native) {
            return std::unexpected(native.error());
        }
        auto body = mApi.Signed(net::HttpMethod::kGet, "/api/v3/openOrders",
                                {{"symbol", *native}}, kOpenOrdersWeight, true);
        if (!body) {
            return std::unexpected(body.error());
        }
        if (!body->is_array()) {
            return std::unexpected(Malformed("openOrders is not an array"));
        }
        for (const auto& row : *body) {
            auto order = ParseOrder(row);
            if (!order) {
                return std::unexpected(order.error());
            }
            Remember(order->order_id, order->symbol);
            out.push_back(std::move(*order));
        }
    }
    return out;
}

Result<types::OrderStatus> BinanceUSTradingVenue::GetOrder(
    const std::string& order_id) {
    std::vector<types::Symbol> candidates;
    {
        std::lock_guard lock(mMutex);
        if (auto it = mOrderSymbols.find(order_id); it != mOrderSymbols.end()) {
            candidates.push_back(it->second);
        }
    }
    // An order this process never saw (placed before a restart, say) could be
    // on any symbol it trades.
    if (candidates.empty()) {
        candidates = mSymbols;
    }

    for (const auto& symbol : candidates) {
        auto native = data::ToBinanceUSSymbol(symbol);
        if (!native) {
            return std::unexpected(native.error());
        }
        auto body = mApi.Signed(net::HttpMethod::kGet, "/api/v3/order",
                                {{"symbol", *native}, {"orderId", order_id}},
                                kOrderWeight, true);
        if (!body) {
            if (body.error().category == ErrorCategory::kNotFound) {
                continue;  // not on this symbol
            }
            return std::unexpected(body.error());
        }
        auto order = ParseOrder(*body);
        if (!order) {
            return std::unexpected(order.error());
        }
        Remember(order->order_id, order->symbol);
        return order;
    }
    return MakeError(ErrorCategory::kNotFound,
                     "binanceus: unknown order " + order_id);
}

Result<std::vector<types::Fill>> BinanceUSTradingVenue::GetFills(
    types::Timestamp since) {
    const auto now = mClock();
    std::vector<types::Fill> out;

    for (const auto& symbol : mSymbols) {
        auto native = data::ToBinanceUSSymbol(symbol);
        if (!native) {
            return std::unexpected(native.error());
        }
        std::set<std::int64_t> seen;
        types::Timestamp start = since;
        while (start <= now) {
            const auto end =
                std::min(now, types::Timestamp(start + kTradeWindow -
                                               std::chrono::milliseconds(1)));
            auto body = mApi.Signed(
                net::HttpMethod::kGet, "/api/v3/myTrades",
                {{"symbol", *native},
                 {"startTime", std::to_string(types::ToEpochMillis(start))},
                 {"endTime", std::to_string(types::ToEpochMillis(end))},
                 {"limit", std::to_string(kTradePage)}},
                kMyTradesWeight, true);
            if (!body) {
                return std::unexpected(body.error());
            }
            if (!body->is_array()) {
                return std::unexpected(Malformed("myTrades is not an array"));
            }

            types::Timestamp last_time = start;
            for (const auto& row : *body) {
                auto id = IntegerMember(row, "id");
                if (!id) {
                    return std::unexpected(id.error());
                }
                auto fill = ParseTrade(row, *native, symbol);
                if (!fill) {
                    return std::unexpected(fill.error());
                }
                last_time = std::max(last_time, fill->time);
                if (seen.insert(*id).second) {
                    out.push_back(std::move(*fill));
                }
            }

            if (body->size() >= kTradePage && last_time > start) {
                // A full page: the window has more. Continue from the last
                // trade's time (ties are dropped by `seen`).
                start = last_time;
            } else {
                start = end + std::chrono::milliseconds(1);
            }
        }
    }
    std::ranges::stable_sort(out,
                             [](const types::Fill& a, const types::Fill& b) {
                                 return a.time < b.time;
                             });
    return out;
}

Result<std::vector<types::Position>> BinanceUSTradingVenue::GetPositions() {
    return std::vector<types::Position>{};
}

Result<std::vector<types::Balance>> BinanceUSTradingVenue::GetBalances() {
    auto body = mApi.Signed(net::HttpMethod::kGet, "/api/v3/account", {},
                            kAccountWeight, true);
    if (!body) {
        return std::unexpected(body.error());
    }
    auto balances = Member(*body, "balances");
    if (!balances) {
        return std::unexpected(balances.error());
    }
    if (!balances->is_array()) {
        return std::unexpected(Malformed("balances is not an array"));
    }
    std::vector<types::Balance> out;
    for (const auto& row : *balances) {
        auto asset = TextMember(row, "asset");
        auto free = DecimalMember(row, "free");
        auto locked = DecimalMember(row, "locked");
        if (!asset) return std::unexpected(asset.error());
        if (!free) return std::unexpected(free.error());
        if (!locked) return std::unexpected(locked.error());
        types::Balance balance;
        balance.asset = *asset;
        balance.available = *free;
        balance.locked = *locked;
        balance.total = *free + *locked;
        if (!balance.total.IsZero()) {
            out.push_back(std::move(balance));
        }
    }
    return out;
}

}  // namespace kestrel::trading
