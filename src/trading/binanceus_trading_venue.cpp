#include "kestrel/trading/binanceus_trading_venue.hpp"

#include <spdlog/spdlog.h>

#include <set>

#include "kestrel/data/symbols.hpp"
#include "kestrel/net/timed_http_client.hpp"
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

types::Error NotImplemented(const char* what) {
    return MakeError(ErrorCategory::kNotImplemented,
                     std::string("BinanceUSTradingVenue::") + what +
                         " is not implemented yet (the venue is read-only)")
        .error();
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
      mClock(options.clock) {}

void BinanceUSTradingVenue::Remember(const std::string& order_id,
                                     const types::Symbol& symbol) {
    std::lock_guard lock(mMutex);
    mOrderSymbols[order_id] = symbol;
}

Result<types::OrderAcknowledgement> BinanceUSTradingVenue::PlaceOrder(
    const types::OrderRequest&) {
    return std::unexpected(NotImplemented("PlaceOrder"));
}

Result<CancelOutcome> BinanceUSTradingVenue::CancelOrder(const std::string&) {
    return std::unexpected(NotImplemented("CancelOrder"));
}

Result<std::vector<std::string>> BinanceUSTradingVenue::CancelAll() {
    return std::unexpected(NotImplemented("CancelAll"));
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
