#include "kestrel/trading/schwab_trading_venue.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cctype>
#include <nlohmann/json.hpp>

#include "kestrel/data/symbols.hpp"
#include "kestrel/trading/order_rules.hpp"
#include "kestrel/util/json.hpp"
#include "kestrel/util/url.hpp"

namespace kestrel::trading {

using nlohmann::json;
using types::Decimal;
using types::ErrorCategory;
using types::MakeError;
using types::OrderAcknowledgement;
using types::OrderState;
using types::OrderStatus;
using types::OrderType;
using types::Result;
using types::Rounding;
using types::Side;
using types::TimeInForce;

namespace {

// Allowed for the difference between our clock and Schwab's when asking for
// orders by entry time.
constexpr std::chrono::minutes kQueryClockSkew{5};
// How far before a placement that ended without an answer its order is
// looked for: close, so that an older order is not taken for it.
constexpr std::chrono::seconds kRecoveryClockSkew{60};
// The wait before the second look for such an order.
constexpr std::chrono::milliseconds kLookupPause{200};
// How far back orders are fetched when nothing narrower is known: a GTC
// order lives at most 180 days.
constexpr std::chrono::days kOrderLookback{180};
// Schwab allows at most a year between fromEnteredTime and toEnteredTime.
constexpr std::chrono::days kMaxOrderQuerySpan{364};
// The most orders GET /orders returns; a fetch that gets this many is split
// in two, down to this span.
constexpr std::size_t kSchwabOrdersPageCap = 3000;
constexpr std::chrono::minutes kMinSplitSpan{1};
// Order fetches remembered for narrowing the next; past this the oldest two
// are merged.
constexpr std::size_t kMaxScans = 1024;

// Whether Schwab may have acted on a request that failed.
bool OutcomeUnknown(ErrorCategory category) {
    return category == ErrorCategory::kNetwork ||
           category == ErrorCategory::kTimeout ||
           category == ErrorCategory::kServer;
}

std::string MaskAccountNumber(std::string_view account_number) {
    if (account_number.size() <= 4) {
        return "****";
    }
    return "****" +
           std::string(account_number.substr(account_number.size() - 4));
}

bool IsNumericId(std::string_view s) {
    return !s.empty() && std::ranges::all_of(s, [](unsigned char c) {
        return std::isdigit(c) != 0;
    });
}

// A stock ("EQUITY") or an ETF ("COLLECTIVE_INVESTMENT", or "EQUITY").
bool IsTradableEquityAsset(std::string_view asset_type) {
    return asset_type.empty() || asset_type == "EQUITY" ||
           asset_type == "COLLECTIVE_INVESTMENT";
}

// The tick for the order's price: $0.01 from $1.00 up, $0.0001 under it (SEC
// rule 612), where GetSymbolInfo set it from an earlier price.
types::SymbolInfo EffectiveSymbolInfo(
    types::SymbolInfo info, const std::optional<Decimal>& order_price) {
    if (!order_price || order_price->Sign() <= 0) {
        return info;
    }
    const Decimal one_dollar = Decimal::FromInt(1);
    const Decimal cent(1, 2);
    const Decimal sub_penny(1, 4);
    if (*order_price >= one_dollar && info.tick_size == sub_penny) {
        info.tick_size = cent;
    } else if (*order_price < one_dollar && info.tick_size == cent) {
        info.tick_size = sub_penny;
    }
    return info;
}

OrderAcknowledgement AckFrom(const OrderStatus& status,
                             types::Timestamp fallback_time,
                             std::string client_order_id = {}) {
    const auto ack_time = status.created_time != types::Timestamp{}
                              ? status.created_time
                              : fallback_time;
    if (client_order_id.empty()) {
        client_order_id = status.client_order_id;
    }
    return OrderAcknowledgement{status.order_id, std::move(client_order_id),
                                status.state, ack_time};
}

// The messages in a Schwab error body (`{"message", "errors": [...]}`, or
// the gateway's `{"errors": [{"title", "detail"}]}`), or the body itself.
std::string ExtractErrorDetail(std::string_view raw) {
    const auto brace = raw.find('{');
    if (brace == std::string_view::npos) {
        return std::string(raw);
    }
    json body = json::parse(raw.substr(brace), nullptr, false);
    if (body.is_discarded() || !body.is_object()) {
        return std::string(raw);
    }
    std::string detail;
    const auto append = [&detail](std::string_view part) {
        if (part.empty()) {
            return;
        }
        if (!detail.empty()) {
            detail += "; ";
        }
        detail += part;
    };
    if (const auto msg = body.find("message");
        msg != body.end() && msg->is_string()) {
        append(msg->get_ref<const std::string&>());
    }
    if (const auto errors = body.find("errors");
        errors != body.end() && errors->is_array()) {
        for (const auto& item : *errors) {
            if (item.is_string()) {
                append(item.get_ref<const std::string&>());
            } else if (item.is_object()) {
                const auto d = item.find("detail");
                const auto t = item.find("title");
                if (d != item.end() && d->is_string() &&
                    !d->get_ref<const std::string&>().empty()) {
                    append(d->get_ref<const std::string&>());
                } else if (t != item.end() && t->is_string()) {
                    append(t->get_ref<const std::string&>());
                }
            }
        }
    }
    if (detail.empty()) {
        for (const char* key : {"error_description", "error"}) {
            if (const auto it = body.find(key);
                it != body.end() && it->is_string()) {
                append(it->get_ref<const std::string&>());
            }
        }
    }
    return detail.empty() ? std::string(raw) : detail;
}

types::Error FormatSchwabError(types::Error error, std::string_view where) {
    const std::string detail = ExtractErrorDetail(error.message);
    std::string prefix = "schwab " + std::string(where);
    if (error.http_status) {
        prefix += " (HTTP " + std::to_string(*error.http_status) + ")";
    }
    error.message = prefix + ": " + detail;
    return error;
}

std::string Lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string_view Trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    return s;
}

Result<std::string> TextMember(const json& obj, const char* key) {
    if (!obj.is_object()) {
        return MakeError(ErrorCategory::kParse,
                         std::string("schwab: expected object for ") + key);
    }
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) {
        return MakeError(
            ErrorCategory::kParse,
            std::string("schwab: missing or non-string '") + key + "'");
    }
    return it->get<std::string>();
}

std::string OptionalText(const json& obj, const char* key) {
    if (!obj.is_object()) {
        return {};
    }
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) {
        return {};
    }
    return it->get<std::string>();
}

Result<std::string> IdMember(const json& obj, const char* key) {
    if (!obj.is_object()) {
        return MakeError(ErrorCategory::kParse,
                         std::string("schwab: expected object for ") + key);
    }
    const auto it = obj.find(key);
    if (it == obj.end()) {
        return MakeError(ErrorCategory::kParse,
                         std::string("schwab: missing '") + key + "'");
    }
    if (it->is_number_integer()) {
        const auto v = it->get<std::int64_t>();
        if (v < 0) {
            return MakeError(ErrorCategory::kParse,
                             std::string("schwab: negative '") + key + "'");
        }
        return std::to_string(v);
    }
    if (it->is_number_unsigned()) {
        return std::to_string(it->get<std::uint64_t>());
    }
    if (it->is_string() && !it->get_ref<const std::string&>().empty()) {
        return it->get<std::string>();
    }
    return MakeError(ErrorCategory::kParse,
                     std::string("schwab: invalid '") + key + "'");
}

Result<Decimal> DecimalMember(const json& obj, const char* key) {
    if (!obj.is_object()) {
        return MakeError(ErrorCategory::kParse,
                         std::string("schwab: expected object for ") + key);
    }
    const auto it = obj.find(key);
    if (it == obj.end()) {
        return MakeError(ErrorCategory::kParse,
                         std::string("schwab: missing '") + key + "'");
    }
    return util::JsonDecimal(*it);
}

Result<std::optional<Decimal>> OptionalDecimalMember(const json& obj,
                                                     const char* key) {
    if (!obj.is_object()) {
        return MakeError(ErrorCategory::kParse,
                         std::string("schwab: expected object for ") + key);
    }
    const auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) {
        return std::optional<Decimal>{};
    }
    auto value = util::JsonDecimal(*it);
    if (!value) {
        return std::unexpected(value.error());
    }
    return std::optional<Decimal>(*value);
}

// The order ID at the end of a 201's Location header, or `orderId` in its
// body; empty if neither is a number (".../orders" is not an ID).
std::string ExtractOrderId(const net::HttpResponse& response) {
    for (const auto& [name, value] : response.headers) {
        if (Lower(name) == "location") {
            std::string_view v = Trim(value);
            while (!v.empty() && v.back() == '/') {
                v.remove_suffix(1);
            }
            const auto slash = v.rfind('/');
            const std::string_view id =
                slash == std::string_view::npos ? v : v.substr(slash + 1);
            if (IsNumericId(id)) {
                return std::string(id);
            }
        }
    }
    if (!response.body.empty()) {
        if (auto parsed = util::ParseJson(response.body);
            parsed && parsed->is_object()) {
            if (auto id = IdMember(*parsed, "orderId");
                id && IsNumericId(*id) && *id != "0") {
                return *id;
            }
        }
    }
    return {};
}

Result<std::string_view> SchwabOrderType(OrderType type) {
    switch (type) {
        case OrderType::kMarket:
            return "MARKET";
        case OrderType::kLimit:
            return "LIMIT";
        case OrderType::kStop:
            return "STOP";
    }
    return MakeError(ErrorCategory::kInvalidArgument, "unknown order type");
}

Result<std::string_view> SchwabDuration(OrderType type, TimeInForce tif) {
    if (type == OrderType::kMarket) {
        if (tif == TimeInForce::kDay || tif == TimeInForce::kGtc) {
            return "DAY";
        }
        return MakeError(ErrorCategory::kUnsupported,
                         "schwab market orders only support DAY time-in-force");
    }
    if (type == OrderType::kStop) {
        if (tif == TimeInForce::kDay) {
            return "DAY";
        }
        if (tif == TimeInForce::kGtc) {
            return "GOOD_TILL_CANCEL";
        }
        return MakeError(
            ErrorCategory::kUnsupported,
            "schwab stop orders only support DAY or GTC time-in-force");
    }
    switch (tif) {
        case TimeInForce::kGtc:
            return "GOOD_TILL_CANCEL";
        case TimeInForce::kDay:
            return "DAY";
        case TimeInForce::kIoc:
            return "IMMEDIATE_OR_CANCEL";
        case TimeInForce::kFok:
            return "FILL_OR_KILL";
    }
    return MakeError(ErrorCategory::kInvalidArgument, "unknown time in force");
}

// Written by hand so that prices and quantities go from Decimal to JSON
// without passing through a double.
Result<std::string> BuildSingleOrderJson(const types::OrderRequest& conformed,
                                         std::string_view schwab_symbol) {
    auto order_type = SchwabOrderType(conformed.type);
    if (!order_type) {
        return std::unexpected(order_type.error());
    }
    auto duration = SchwabDuration(conformed.type, conformed.time_in_force);
    if (!duration) {
        return std::unexpected(duration.error());
    }
    const std::string_view instruction =
        conformed.side == Side::kBuy ? "BUY" : "SELL";

    std::string body = R"({"orderType":")" + std::string(*order_type) +
                       R"(","session":"NORMAL","duration":")" +
                       std::string(*duration) +
                       R"(","orderStrategyType":"SINGLE")";
    if (conformed.type == OrderType::kLimit) {
        body += R"(,"price":)" + conformed.limit_price->Normalized().ToString();
    } else if (conformed.type == OrderType::kStop) {
        body +=
            R"(,"stopPrice":)" + conformed.stop_price->Normalized().ToString();
    }
    body += R"(,"orderLegCollection":[{"instruction":")" +
            std::string(instruction) + R"(","quantity":)" +
            conformed.quantity.Normalized().ToString() +
            R"(,"instrument":{"symbol":)" + json(schwab_symbol).dump() +
            R"(,"assetType":"EQUITY"}}]})";
    return body;
}

Result<Side> SideFrom(std::string_view instruction) {
    if (instruction == "BUY" || instruction == "BUY_TO_COVER" ||
        instruction == "BUY_TO_OPEN" || instruction == "BUY_TO_CLOSE") {
        return Side::kBuy;
    }
    if (instruction == "SELL" || instruction == "SELL_SHORT" ||
        instruction == "SELL_SHORT_EXEMPT" || instruction == "SELL_TO_OPEN" ||
        instruction == "SELL_TO_CLOSE") {
        return Side::kSell;
    }
    if (instruction == "EXCHANGE") {
        return MakeError(ErrorCategory::kUnsupported,
                         "schwab: unsupported order instruction 'EXCHANGE'");
    }
    return MakeError(
        ErrorCategory::kUnsupported,
        "schwab: unknown order instruction '" + std::string(instruction) + "'");
}

Result<OrderType> OrderTypeFrom(std::string_view type) {
    if (type == "MARKET" || type == "MARKET_ON_CLOSE") {
        return OrderType::kMarket;
    }
    if (type == "LIMIT" || type == "LIMIT_ON_CLOSE") {
        return OrderType::kLimit;
    }
    if (type == "STOP" || type == "STOP_LIMIT" || type == "TRAILING_STOP" ||
        type == "TRAILING_STOP_LIMIT") {
        return OrderType::kStop;
    }
    return MakeError(
        ErrorCategory::kUnsupported,
        "schwab: unsupported orderType '" + std::string(type) + "'");
}

Result<TimeInForce> TimeInForceFrom(std::string_view duration) {
    if (duration.empty() || duration == "DAY") {
        return TimeInForce::kDay;
    }
    if (duration == "GOOD_TILL_CANCEL" || duration == "END_OF_WEEK" ||
        duration == "END_OF_MONTH" || duration == "NEXT_END_OF_MONTH") {
        return TimeInForce::kGtc;
    }
    if (duration == "IMMEDIATE_OR_CANCEL") {
        return TimeInForce::kIoc;
    }
    if (duration == "FILL_OR_KILL") {
        return TimeInForce::kFok;
    }
    return MakeError(
        ErrorCategory::kUnsupported,
        "schwab: unsupported order duration '" + std::string(duration) + "'");
}

Result<OrderState> OrderStateFrom(std::string_view status,
                                  const Decimal& filled) {
    if (status == "FILLED") {
        return OrderState::kFilled;
    }
    if (status == "CANCELED" || status == "CANCELLED" || status == "REPLACED") {
        return OrderState::kCancelled;
    }
    if (status == "REJECTED") {
        return OrderState::kRejected;
    }
    if (status == "EXPIRED") {
        return OrderState::kExpired;
    }
    if (status == "WORKING" || status == "QUEUED" || status == "ACCEPTED" ||
        status == "AWAITING_STOP_CONDITION" ||
        status == "AWAITING_PARENT_ORDER" || status == "AWAITING_CONDITION" ||
        status == "AWAITING_MANUAL_REVIEW" || status == "AWAITING_UR_OUT" ||
        status == "AWAITING_RELEASE_TIME" || status == "PENDING_ACTIVATION" ||
        status == "PENDING_ACKNOWLEDGEMENT" || status == "PENDING_RECALL" ||
        status == "PENDING_CANCEL" || status == "PENDING_REPLACE" ||
        status == "NEW") {
        return filled.Sign() > 0 ? OrderState::kPartiallyFilled
                                 : OrderState::kNew;
    }
    if (status == "UNKNOWN") {
        // Taken as working, so that nothing ignores an order that may be.
        spdlog::warn("schwab: order has status 'UNKNOWN'; treating as active");
        return filled.Sign() > 0 ? OrderState::kPartiallyFilled
                                 : OrderState::kNew;
    }
    return MakeError(
        ErrorCategory::kUnsupported,
        "schwab: unsupported order status '" + std::string(status) + "'");
}

// Whether every leg is a stock or ETF (not an option, fund, bond...).
Result<bool> HasOnlyEquityLegs(const json& legs) {
    if (!legs.is_array() || legs.empty()) {
        return MakeError(ErrorCategory::kParse,
                         "schwab: orderLegCollection must be a non-empty "
                         "array");
    }
    for (const auto& leg : legs) {
        if (!leg.is_object()) {
            return MakeError(ErrorCategory::kParse,
                             "schwab: order leg is not an object");
        }
        const auto inst = leg.find("instrument");
        if (inst == leg.end() || !inst->is_object()) {
            return MakeError(ErrorCategory::kParse,
                             "schwab: order leg has no instrument");
        }
        const std::string asset_type = OptionalText(*inst, "assetType");
        if (!IsTradableEquityAsset(asset_type)) {
            return false;
        }
    }
    return true;
}

Result<OrderStatus> ParseSingleOrder(const json& order,
                                     std::string client_order_id) {
    auto id = IdMember(order, "orderId");
    auto status_str = TextMember(order, "status");
    auto type_str = TextMember(order, "orderType");
    if (!id) return std::unexpected(id.error());
    if (!status_str) return std::unexpected(status_str.error());
    if (!type_str) return std::unexpected(type_str.error());

    const auto legs_it = order.find("orderLegCollection");
    if (legs_it == order.end()) {
        return MakeError(ErrorCategory::kParse,
                         "schwab: order " + *id + " has no orderLegCollection");
    }
    auto equity_legs = HasOnlyEquityLegs(*legs_it);
    if (!equity_legs) {
        return std::unexpected(equity_legs.error());
    }
    if (!*equity_legs) {
        return MakeError(ErrorCategory::kUnsupported,
                         "schwab: order " + *id + " is not an equity order");
    }

    const json& leg = (*legs_it)[0];
    const json& inst = leg["instrument"];
    auto native_symbol = TextMember(inst, "symbol");
    auto instruction = TextMember(leg, "instruction");
    if (!native_symbol) return std::unexpected(native_symbol.error());
    if (!instruction) return std::unexpected(instruction.error());

    auto symbol = data::FromSchwabSymbol(*native_symbol);
    auto side = SideFrom(*instruction);
    auto type = OrderTypeFrom(*type_str);
    auto tif = TimeInForceFrom(OptionalText(order, "duration"));
    if (!symbol) return std::unexpected(symbol.error());
    if (!side) return std::unexpected(side.error());
    if (!type) return std::unexpected(type.error());
    if (!tif) return std::unexpected(tif.error());

    auto qty_opt = OptionalDecimalMember(order, "quantity");
    if (!qty_opt) return std::unexpected(qty_opt.error());
    if (!*qty_opt) {
        qty_opt = OptionalDecimalMember(leg, "quantity");
        if (!qty_opt) return std::unexpected(qty_opt.error());
    }
    if (!*qty_opt) {
        return MakeError(ErrorCategory::kParse,
                         "schwab: order " + *id + " has no quantity");
    }

    auto filled_opt = OptionalDecimalMember(order, "filledQuantity");
    if (!filled_opt) return std::unexpected(filled_opt.error());
    const Decimal filled = filled_opt->value_or(Decimal());

    auto state = OrderStateFrom(*status_str, filled);
    if (!state) return std::unexpected(state.error());

    auto price_opt = OptionalDecimalMember(order, "price");
    auto stop_opt = OptionalDecimalMember(order, "stopPrice");
    if (!price_opt) return std::unexpected(price_opt.error());
    if (!stop_opt) return std::unexpected(stop_opt.error());

    OrderStatus out;
    out.order_id = std::move(*id);
    out.client_order_id = std::move(client_order_id);
    out.symbol = std::move(*symbol);
    out.side = *side;
    out.type = *type;
    out.time_in_force = *tif;
    out.quantity = **qty_opt;
    out.filled_quantity = filled;
    out.state = *state;
    if (*price_opt && !(*price_opt)->IsZero()) {
        out.limit_price = **price_opt;
    }
    if (*stop_opt && !(*stop_opt)->IsZero()) {
        out.stop_price = **stop_opt;
    }

    if (const auto entered = order.find("enteredTime");
        entered != order.end() && !entered->is_null()) {
        if (!entered->is_string()) {
            return MakeError(ErrorCategory::kParse,
                             "schwab: enteredTime must be a string");
        }
        auto ts = types::ParseIso8601(entered->get_ref<const std::string&>());
        if (!ts) return std::unexpected(ts.error());
        out.created_time = *ts;
    }
    out.updated_time = out.created_time;
    if (const auto closed = order.find("closeTime");
        closed != order.end() && !closed->is_null()) {
        if (!closed->is_string()) {
            return MakeError(ErrorCategory::kParse,
                             "schwab: closeTime must be a string");
        }
        auto ts = types::ParseIso8601(closed->get_ref<const std::string&>());
        if (!ts) return std::unexpected(ts.error());
        out.updated_time = std::max(out.updated_time, *ts);
    }

    if (const auto activities = order.find("orderActivityCollection");
        activities != order.end() && !activities->is_null()) {
        if (!activities->is_array()) {
            return MakeError(
                ErrorCategory::kParse,
                "schwab: orderActivityCollection must be an array");
        }
        Decimal total_notional;
        Decimal total_qty;
        for (const auto& act : *activities) {
            if (!act.is_object()) {
                return MakeError(ErrorCategory::kParse,
                                 "schwab: order activity must be an object");
            }
            if (OptionalText(act, "activityType") != "EXECUTION") {
                continue;
            }
            const std::string exec_type = OptionalText(act, "executionType");
            if (!exec_type.empty() && exec_type != "FILL") {
                continue;
            }
            const auto exec_legs = act.find("executionLegs");
            if (exec_legs == act.end() || exec_legs->is_null()) {
                continue;
            }
            if (!exec_legs->is_array()) {
                return MakeError(ErrorCategory::kParse,
                                 "schwab: executionLegs must be an array");
            }
            for (const auto& eleg : *exec_legs) {
                auto eqty = DecimalMember(eleg, "quantity");
                auto eprice = DecimalMember(eleg, "price");
                if (!eqty) return std::unexpected(eqty.error());
                if (!eprice) return std::unexpected(eprice.error());
                total_qty += *eqty;
                total_notional += *eqty * *eprice;
                if (const auto etime = eleg.find("time");
                    etime != eleg.end() && etime->is_string()) {
                    if (auto ts = types::ParseIso8601(
                            etime->get_ref<const std::string&>())) {
                        out.updated_time = std::max(out.updated_time, *ts);
                    }
                }
            }
        }
        if (total_qty.Sign() > 0) {
            out.average_fill_price = Decimal::Divide(total_notional, total_qty,
                                                     8, Rounding::kNearest)
                                         .Normalized();
        }
    }

    if (const std::string desc = OptionalText(order, "statusDescription");
        !desc.empty()) {
        out.reason = desc;
    }
    return out;
}

// The stock orders in `order`, its childOrderStrategies and its
// replacingOrderCollection. With `skip_unsupported`, an order Kestrel cannot
// read (an option, an unknown status) is skipped, so that one such order does
// not stop the rest being read; except `target_order_id`, which then fails.
Result<void> CollectOrdersFromJson(
    const json& order,
    const std::function<std::string(const std::string&)>& client_id_lookup,
    bool skip_unsupported, std::vector<OrderStatus>& out,
    std::unordered_set<std::string>& seen_ids,
    std::string_view target_order_id = {}) {
    if (!order.is_object()) {
        return MakeError(ErrorCategory::kParse,
                         "schwab: order entry is not a JSON object");
    }
    const auto legs_it = order.find("orderLegCollection");
    const bool has_legs =
        legs_it != order.end() && legs_it->is_array() && !legs_it->empty();
    const auto children_it = order.find("childOrderStrategies");
    const bool has_children = children_it != order.end() &&
                              children_it->is_array() && !children_it->empty();
    const auto replacing_it = order.find("replacingOrderCollection");
    const bool has_replacing = replacing_it != order.end() &&
                               replacing_it->is_array() &&
                               !replacing_it->empty();

    if (!has_legs && !has_children && !has_replacing) {
        if (!IdMember(order, "orderId") || !TextMember(order, "status")) {
            return MakeError(
                ErrorCategory::kParse,
                "schwab: order has neither legs nor childOrderStrategies");
        }
    }

    if (has_legs) {
        auto id = IdMember(order, "orderId");
        if (!id) {
            return std::unexpected(id.error());
        }
        const bool is_target =
            !target_order_id.empty() && *id == target_order_id;
        const bool skip_this = skip_unsupported && !is_target;
        auto equity = HasOnlyEquityLegs(*legs_it);
        if (!equity) {
            return std::unexpected(equity.error());
        }
        if (*equity || !skip_this) {
            auto parsed = ParseSingleOrder(order, client_id_lookup(*id));
            if (!parsed) {
                if (skip_this &&
                    parsed.error().category == ErrorCategory::kUnsupported) {
                    if (*equity) {
                        spdlog::warn("schwab: skipping order {}: {}", *id,
                                     parsed.error().message);
                    }
                } else {
                    return std::unexpected(parsed.error());
                }
            } else if (seen_ids.insert(parsed->order_id).second) {
                out.push_back(std::move(*parsed));
            }
        }
    }

    for (const auto* collection :
         {has_children ? &(*children_it) : nullptr,
          has_replacing ? &(*replacing_it) : nullptr}) {
        if (!collection) {
            continue;
        }
        for (const auto& child : *collection) {
            if (auto ok = CollectOrdersFromJson(child, client_id_lookup,
                                                skip_unsupported, out, seen_ids,
                                                target_order_id);
                !ok) {
                return ok;
            }
        }
    }
    return {};
}

// The fills at or after `since` in `order` and the orders inside it.
Result<void> CollectFillsFromJson(
    const json& order, types::Timestamp since,
    const std::function<std::string(const std::string&)>& client_id_lookup,
    std::vector<types::Fill>& out,
    std::unordered_set<std::string>& seen_fill_ids) {
    if (!order.is_object()) {
        return MakeError(ErrorCategory::kParse,
                         "schwab: order entry is not a JSON object");
    }

    const auto legs_it = order.find("orderLegCollection");
    const auto acts_it = order.find("orderActivityCollection");
    if (legs_it != order.end() && !legs_it->is_null() && !legs_it->is_array()) {
        return MakeError(ErrorCategory::kParse,
                         "schwab: orderLegCollection must be an array");
    }
    if (acts_it != order.end() && !acts_it->is_null() && !acts_it->is_array()) {
        return MakeError(ErrorCategory::kParse,
                         "schwab: orderActivityCollection must be an array");
    }

    if (legs_it != order.end() && legs_it->is_array() && !legs_it->empty() &&
        acts_it != order.end() && acts_it->is_array() && !acts_it->empty()) {
        auto id = IdMember(order, "orderId");
        if (!id) {
            return std::unexpected(id.error());
        }
        const std::string client_id = client_id_lookup(*id);
        const json& order_legs = *legs_it;

        for (std::size_t act_idx = 0; act_idx < acts_it->size(); ++act_idx) {
            const json& act = (*acts_it)[act_idx];
            if (!act.is_object()) {
                return MakeError(ErrorCategory::kParse,
                                 "schwab: order activity must be an object");
            }
            if (OptionalText(act, "activityType") != "EXECUTION") {
                continue;
            }
            const std::string exec_type = OptionalText(act, "executionType");
            if (!exec_type.empty() && exec_type != "FILL") {
                continue;
            }
            const auto exec_legs_it = act.find("executionLegs");
            if (exec_legs_it == act.end() || !exec_legs_it->is_array()) {
                return MakeError(
                    ErrorCategory::kParse,
                    "schwab: EXECUTION activity has no executionLegs array");
            }
            std::optional<std::string> explicit_act_id;
            if (auto act_id = IdMember(act, "activityId")) {
                explicit_act_id = *act_id;
            }

            for (std::size_t leg_idx = 0; leg_idx < exec_legs_it->size();
                 ++leg_idx) {
                const json& eleg = (*exec_legs_it)[leg_idx];
                if (!eleg.is_object()) {
                    return MakeError(ErrorCategory::kParse,
                                     "schwab: execution leg is not an object");
                }
                auto qty = DecimalMember(eleg, "quantity");
                auto price = DecimalMember(eleg, "price");
                auto time_str = TextMember(eleg, "time");
                if (!qty) return std::unexpected(qty.error());
                if (!price) return std::unexpected(price.error());
                if (!time_str) return std::unexpected(time_str.error());
                auto time = types::ParseIso8601(*time_str);
                if (!time) return std::unexpected(time.error());
                if (*time < since) {
                    continue;
                }

                // The order leg with the execution's legId, or the first.
                const json* matched_leg = &order_legs[0];
                if (auto eleg_id = IdMember(eleg, "legId")) {
                    for (const auto& oleg : order_legs) {
                        if (auto oleg_id = IdMember(oleg, "legId");
                            oleg_id && *oleg_id == *eleg_id) {
                            matched_leg = &oleg;
                            break;
                        }
                    }
                }
                if (!matched_leg->is_object()) {
                    return MakeError(ErrorCategory::kParse,
                                     "schwab: order leg is not an object");
                }
                const auto inst_it = matched_leg->find("instrument");
                if (inst_it == matched_leg->end() || !inst_it->is_object()) {
                    return MakeError(ErrorCategory::kParse,
                                     "schwab: order leg has no instrument");
                }
                const std::string asset_type =
                    OptionalText(*inst_it, "assetType");
                if (!IsTradableEquityAsset(asset_type)) {
                    continue;
                }
                auto native_sym = TextMember(*inst_it, "symbol");
                auto instruction = TextMember(*matched_leg, "instruction");
                if (!native_sym) return std::unexpected(native_sym.error());
                if (!instruction) return std::unexpected(instruction.error());
                auto symbol = data::FromSchwabSymbol(*native_sym);
                auto side = SideFrom(*instruction);
                if (!symbol) return std::unexpected(symbol.error());
                if (!side) {
                    if (side.error().category == ErrorCategory::kUnsupported) {
                        spdlog::warn("schwab: skipping fill on order {}: {}",
                                     *id, side.error().message);
                        continue;
                    }
                    return std::unexpected(side.error());
                }

                // Keyed by activityId, or failing that by the execution's
                // time and place in the list.
                const std::string act_key =
                    explicit_act_id
                        ? *explicit_act_id
                        : (std::to_string(types::ToEpochMillis(*time)) + "-" +
                           std::to_string(act_idx));
                std::string fill_id =
                    *id + ":" + act_key + ":" + std::to_string(leg_idx);
                if (!seen_fill_ids.insert(fill_id).second) {
                    continue;
                }

                // Executions give no fee (SEC and TAF fees) or liquidity.
                types::Fill fill;
                fill.fill_id = std::move(fill_id);
                fill.order_id = *id;
                fill.client_order_id = client_id;
                fill.symbol = std::move(*symbol);
                fill.side = *side;
                fill.quantity = *qty;
                fill.price = *price;
                fill.fee = Decimal();
                fill.fee_asset = "USD";
                fill.liquidity = types::Liquidity::kTaker;
                fill.time = *time;
                out.push_back(std::move(fill));
            }
        }
    }

    for (const char* key :
         {"childOrderStrategies", "replacingOrderCollection"}) {
        if (const auto children_it = order.find(key);
            children_it != order.end() && !children_it->is_null()) {
            if (!children_it->is_array()) {
                return MakeError(
                    ErrorCategory::kParse,
                    std::string("schwab: ") + key + " must be an array");
            }
            for (const auto& child : *children_it) {
                if (auto ok = CollectFillsFromJson(
                        child, since, client_id_lookup, out, seen_fill_ids);
                    !ok) {
                    return ok;
                }
            }
        }
    }
    return {};
}

bool MatchesPlacedOrder(const OrderStatus& order,
                        const types::OrderRequest& conformed,
                        types::Timestamp earliest,
                        const std::unordered_set<std::string>& known_before,
                        const std::unordered_set<std::string>& known_now) {
    if (known_before.contains(order.order_id) ||
        known_now.contains(order.order_id)) {
        return false;
    }
    if (order.symbol != conformed.symbol || order.side != conformed.side ||
        order.type != conformed.type || order.quantity != conformed.quantity) {
        return false;
    }
    const TimeInForce expected_tif = conformed.type == OrderType::kMarket
                                         ? TimeInForce::kDay
                                         : conformed.time_in_force;
    if (order.time_in_force != expected_tif) {
        return false;
    }
    if (conformed.type == OrderType::kLimit &&
        order.limit_price != conformed.limit_price) {
        return false;
    }
    if (conformed.type == OrderType::kStop &&
        order.stop_price != conformed.stop_price) {
        return false;
    }
    if (order.created_time != types::Timestamp{} &&
        order.created_time < earliest) {
        return false;
    }
    return true;
}

// First present decimal among `keys` in `obj`.
Result<std::optional<Decimal>> FirstDecimal(
    const json& obj, std::initializer_list<const char*> keys) {
    if (!obj.is_object()) {
        return std::optional<Decimal>{};
    }
    for (const char* key : keys) {
        auto val = OptionalDecimalMember(obj, key);
        if (!val) {
            return std::unexpected(val.error());
        }
        if (*val) {
            return *val;
        }
    }
    return std::optional<Decimal>{};
}

// The stock and ETF positions in a securitiesAccount.
Result<std::vector<types::Position>> ParseEquityPositions(
    const json& securities_account, std::string_view context) {
    const auto pos_it = securities_account.find("positions");
    if (pos_it == securities_account.end() || pos_it->is_null()) {
        return std::vector<types::Position>{};
    }
    if (!pos_it->is_array()) {
        return MakeError(
            ErrorCategory::kParse,
            "schwab " + std::string(context) + ": positions must be an array");
    }

    std::vector<types::Position> positions;
    for (const auto& item : *pos_it) {
        if (!item.is_object()) {
            return MakeError(ErrorCategory::kParse,
                             "schwab " + std::string(context) +
                                 ": position entry is not an object");
        }
        const auto inst_it = item.find("instrument");
        if (inst_it == item.end() || !inst_it->is_object()) {
            return MakeError(ErrorCategory::kParse,
                             "schwab " + std::string(context) +
                                 ": position has no instrument");
        }
        if (!IsTradableEquityAsset(OptionalText(*inst_it, "assetType"))) {
            continue;
        }
        auto native_sym = TextMember(*inst_it, "symbol");
        if (!native_sym) {
            return std::unexpected(native_sym.error());
        }
        auto symbol = data::FromSchwabSymbol(*native_sym);
        if (!symbol) {
            return std::unexpected(symbol.error());
        }

        auto long_qty_opt = OptionalDecimalMember(item, "longQuantity");
        auto short_qty_opt = OptionalDecimalMember(item, "shortQuantity");
        if (!long_qty_opt) return std::unexpected(long_qty_opt.error());
        if (!short_qty_opt) return std::unexpected(short_qty_opt.error());
        const Decimal net_qty = long_qty_opt->value_or(Decimal()) -
                                short_qty_opt->value_or(Decimal());
        if (net_qty.IsZero()) {
            continue;
        }

        auto avg_opt =
            net_qty.Sign() > 0
                ? FirstDecimal(item, {"averageLongPrice",
                                      "taxLotAverageLongPrice", "averagePrice"})
                : FirstDecimal(item,
                               {"averageShortPrice", "taxLotAverageShortPrice",
                                "averagePrice"});
        if (!avg_opt) {
            return std::unexpected(avg_opt.error());
        }
        const Decimal avg_price = avg_opt->value_or(Decimal());
        positions.push_back(
            types::Position{std::move(*symbol), net_qty, avg_price, Decimal()});
    }
    return positions;
}

// Whether an OCO went in: both legs working, or either filled at all (then
// the acknowledgement must show it, whatever became of the other). It failed
// if a leg closed with nothing filled.
bool IsValidOcoState(const OrderStatus& limit_child,
                     const OrderStatus& stop_child) {
    if (types::IsActive(limit_child.state) &&
        types::IsActive(stop_child.state)) {
        return true;
    }
    const bool limit_filled = limit_child.state == OrderState::kFilled ||
                              limit_child.filled_quantity.Sign() > 0;
    const bool stop_filled = stop_child.state == OrderState::kFilled ||
                             stop_child.filled_quantity.Sign() > 0;
    if (limit_filled || stop_filled) {
        if (limit_child.state == OrderState::kRejected ||
            stop_child.state == OrderState::kRejected) {
            spdlog::warn(
                "schwab OCO: sibling leg was rejected ({}/{}) while the other "
                "leg filled; returning acknowledgement with actual states",
                limit_child.order_id, stop_child.order_id);
        }
        return true;
    }
    return false;
}

// The USD balance of a securitiesAccount:
// - available: cashAvailableForTrading, then (on a margin account) the
//   non-marginable funds before availableFunds, so that borrowing power does
//   not count as cash; the lower of current and projected.
// - total: totalCash or cashBalance, or a negative marginBalance (a margin
//   loan, which then makes nothing available).
// - the start of day's initialBalances only when neither has anything.
Result<types::Balance> ResolveUsdCash(std::string_view account_type,
                                      const json& current,
                                      const json& projected,
                                      const json& initial) {
    const auto avail_keys =
        account_type == "CASH"
            ? std::initializer_list<
                  const char*>{"cashAvailableForTrading", "availableFunds",
                               "availableFundsNonMarginableTrade"}
            : std::initializer_list<const char*>{
                  "cashAvailableForTrading", "availableFundsNonMarginableTrade",
                  "availableFunds"};

    auto avail_cur = FirstDecimal(current, avail_keys);
    auto avail_proj = FirstDecimal(projected, avail_keys);
    auto avail_init = FirstDecimal(initial, avail_keys);
    if (!avail_cur) return std::unexpected(avail_cur.error());
    if (!avail_proj) return std::unexpected(avail_proj.error());
    if (!avail_init) return std::unexpected(avail_init.error());

    auto total_cur = FirstDecimal(current, {"totalCash", "cashBalance"});
    auto total_init = FirstDecimal(initial, {"totalCash", "cashBalance"});
    auto margin_bal = OptionalDecimalMember(current, "marginBalance");
    if (!total_cur) return std::unexpected(total_cur.error());
    if (!total_init) return std::unexpected(total_init.error());
    if (!margin_bal) return std::unexpected(margin_bal.error());

    std::optional<Decimal> total = *total_cur;
    if (!total && account_type != "CASH" && *margin_bal &&
        (*margin_bal)->Sign() < 0) {
        total = *margin_bal;
    }

    std::optional<Decimal> available;
    if (*avail_cur && *avail_proj) {
        if (!total) {
            total = *avail_cur;
        }
        available = std::min(**avail_cur, **avail_proj);
    } else if (*avail_proj) {
        available = *avail_proj;
    } else if (*avail_cur) {
        available = *avail_cur;
    }
    if (!total && available) {
        total = available;
    }
    if (!total) {
        total = *total_init ? *total_init : *avail_init;
    }
    if (!available) {
        available = *avail_init ? *avail_init : total;
    }
    if (!available || !total) {
        return MakeError(
            ErrorCategory::kParse,
            "schwab balances: currentBalances has no cash balance fields");
    }

    // A negative total (a margin loan) stays negative, so that equity is
    // less the loan; otherwise available is kept within the total.
    if (total->Sign() < 0) {
        return types::Balance{"USD", *total, Decimal(), Decimal()};
    }
    available = std::clamp(*available, Decimal(), *total);
    return types::Balance{"USD", *total, *available, *total - *available};
}

// GET /orders' array.
Result<json> ParseOrdersResponse(Result<net::HttpResponse> response) {
    if (!response) {
        return std::unexpected(FormatSchwabError(response.error(), "orders"));
    }
    auto parsed = util::ParseJson(response->body);
    if (!parsed || !parsed->is_array()) {
        return MakeError(ErrorCategory::kParse,
                         "schwab orders: expected a JSON array");
    }
    return parsed;
}

// When the order `item` was entered, if it or an order inside it (`nested`,
// read from it) was working at `time`: working then, or closed since shortly
// before (it may have filled while the fetch was on its way). The fetch goes
// by the outer order's entry time, so that counts too. An order without an
// entry time takes the outer one's, or failing that `from`.
std::optional<types::Timestamp> WorkingSince(
    const json& item, const std::vector<OrderStatus>& nested,
    types::Timestamp time, types::Timestamp from) {
    std::optional<types::Timestamp> outer;
    if (const auto it = item.find("enteredTime");
        it != item.end() && it->is_string()) {
        if (auto ts = types::ParseIso8601(it->get_ref<const std::string&>())) {
            outer = *ts;
        }
    }
    std::optional<types::Timestamp> since;
    for (const auto& order : nested) {
        if (types::IsActive(order.state) ||
            order.updated_time >= time - kQueryClockSkew) {
            const auto entered = order.created_time != types::Timestamp{}
                                     ? order.created_time
                                     : outer.value_or(from);
            since = std::min(since.value_or(entered), entered);
        }
    }
    if (!since) {
        return std::nullopt;
    }
    return std::min(*since, outer.value_or(*since));
}

// Whether the order `id`, in `order` or inside it, has Schwab status `status`.
bool HasStatus(const json& order, std::string_view id,
               std::string_view status) {
    if (!order.is_object()) {
        return false;
    }
    if (auto own = IdMember(order, "orderId");
        own && *own == id && OptionalText(order, "status") == status) {
        return true;
    }
    for (const char* key :
         {"childOrderStrategies", "replacingOrderCollection"}) {
        if (const auto it = order.find(key);
            it != order.end() && it->is_array()) {
            for (const auto& child : *it) {
                if (HasStatus(child, id, status)) {
                    return true;
                }
            }
        }
    }
    return false;
}

}  // namespace

SchwabTradingVenue::SchwabTradingVenue(
    std::shared_ptr<data::SchwabApi> api,
    std::shared_ptr<data::DataSource> data_source, Options options)
    : mApi(std::move(api)),
      mDataSource(std::move(data_source)),
      mOptions(std::move(options)),
      mOrderLimiter(mOptions.orders_per_second, mOptions.order_burst) {
    assert(mApi != nullptr);
    assert(mDataSource != nullptr);
    for (const auto& symbol : mOptions.symbols) {
        auto normalized = types::NormalizeSymbol(symbol);
        mSymbols.insert(normalized ? *normalized : symbol);
    }
}

SchwabTradingVenue::SchwabTradingVenue(
    std::shared_ptr<data::SchwabApi> api,
    std::shared_ptr<data::DataSource> data_source)
    : SchwabTradingVenue(std::move(api), std::move(data_source), Options{}) {}

void SchwabTradingVenue::Sleep(std::chrono::milliseconds duration) const {
    if (duration.count() <= 0) {
        return;
    }
    if (mOptions.sleeper) {
        mOptions.sleeper(duration);
    } else {
        util::DefaultSleep(duration);
    }
}

void SchwabTradingVenue::RememberOrder(const std::string& order_id,
                                       const std::string& client_order_id) {
    if (order_id.empty()) {
        return;
    }
    std::lock_guard lock(mMutex);
    mKnownOrders.insert(order_id);
    if (!client_order_id.empty()) {
        mClientOrderIds[order_id] = client_order_id;
    }
}

void SchwabTradingVenue::ClaimOrder(const std::string& order_id,
                                    const std::string& client_order_id) {
    if (order_id.empty()) {
        return;
    }
    std::lock_guard lock(mMutex);
    mKnownOrders.insert(order_id);
    mClaimedOrders.insert(order_id);
    if (!client_order_id.empty()) {
        mClientOrderIds[order_id] = client_order_id;
    }
}

bool SchwabTradingVenue::TryClaimRecoveredOrder(
    const std::string& order_id, const std::string& client_order_id) {
    if (order_id.empty()) {
        return false;
    }
    std::lock_guard lock(mMutex);
    if (!mClaimedOrders.insert(order_id).second) {
        return false;
    }
    mKnownOrders.insert(order_id);
    if (!client_order_id.empty()) {
        mClientOrderIds[order_id] = client_order_id;
    }
    return true;
}

bool SchwabTradingVenue::TryClaimRecoveredOco(const std::string& parent_id,
                                              const std::string& limit_id,
                                              const std::string& stop_id) {
    if (limit_id.empty() || stop_id.empty() || limit_id == stop_id) {
        return false;
    }
    std::lock_guard lock(mMutex);
    if (mClaimedOrders.contains(limit_id) || mClaimedOrders.contains(stop_id) ||
        (!parent_id.empty() && mClaimedOrders.contains(parent_id))) {
        return false;
    }
    mClaimedOrders.insert(limit_id);
    mClaimedOrders.insert(stop_id);
    mKnownOrders.insert(limit_id);
    mKnownOrders.insert(stop_id);
    if (!parent_id.empty()) {
        mClaimedOrders.insert(parent_id);
        mKnownOrders.insert(parent_id);
    }
    return true;
}

std::string SchwabTradingVenue::ClientOrderIdFor(
    const std::string& order_id) const {
    std::lock_guard lock(mMutex);
    const auto it = mClientOrderIds.find(order_id);
    return it != mClientOrderIds.end() ? it->second : std::string{};
}

SchwabTradingVenue::ClientIdLookup SchwabTradingVenue::ClientIds() const {
    return [this](const std::string& id) { return ClientOrderIdFor(id); };
}

std::unordered_set<std::string> SchwabTradingVenue::KnownOrdersSnapshot()
    const {
    std::lock_guard lock(mMutex);
    return mKnownOrders;
}

std::unordered_set<std::string> SchwabTradingVenue::ClaimedOrdersSnapshot()
    const {
    std::lock_guard lock(mMutex);
    return mClaimedOrders;
}

void SchwabTradingVenue::CancelOnFailure(const std::string& order_id,
                                         std::string& error_message) {
    if (order_id.empty()) {
        return;
    }
    auto cancelled = CancelOrder(order_id);
    if (!cancelled) {
        spdlog::error(
            "schwab: could not cancel order {} while recovering ({}); it may "
            "still be working",
            order_id, cancelled.error().ToString());
        error_message += "; could not cancel order " + order_id + " (" +
                         cancelled.error().message +
                         "): it may still be working";
    }
}

std::string SchwabTradingVenue::AccountUrl(const std::string& hash,
                                           std::string_view path) const {
    return mOptions.base_url + "/accounts/" + util::UrlEncode(hash) +
           std::string(path);
}

Result<net::HttpResponse> SchwabTradingVenue::SendGet(
    const std::string& url, std::map<std::string, std::string> query) const {
    net::HttpRequest request;
    request.method = net::HttpMethod::kGet;
    request.url = url;
    request.query = std::move(query);
    request.headers["Accept"] = "application/json";
    return util::Retry(
        mOptions.retry, [&] { return mApi->Send(request); },
        [this](std::chrono::milliseconds d) { Sleep(d); });
}

Result<net::HttpResponse> SchwabTradingVenue::SendOrderRequest(
    net::HttpMethod method, std::string url, std::string body) {
    net::HttpRequest request;
    request.method = method;
    request.url = std::move(url);
    request.headers["Accept"] = "application/json";
    if (!body.empty()) {
        request.headers["Content-Type"] = "application/json";
        request.body = std::move(body);
    }
    mOrderLimiter.Acquire();
    return mApi->Send(std::move(request));
}

Result<json> SchwabTradingVenue::FetchOrders(const std::string& hash,
                                             types::Timestamp from,
                                             types::Timestamp to) const {
    auto parsed = ParseOrdersResponse(
        SendGet(AccountUrl(hash, "/orders"),
                {{"fromEnteredTime", types::FormatIso8601(from)},
                 {"toEnteredTime", types::FormatIso8601(to)}}));
    if (!parsed || parsed->size() < kSchwabOrdersPageCap) {
        return parsed;
    }
    // At the cap some orders were left out: each half on its own. While an
    // old order is working this happens on every poll, at two or more
    // requests each time.
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true)) {
        spdlog::warn(
            "schwab: {} orders entered from {} to {}, Schwab's most in one "
            "fetch; fetching in parts, which takes more requests",
            kSchwabOrdersPageCap, types::FormatIso8601(from),
            types::FormatIso8601(to));
    }
    if (to - from < kMinSplitSpan) {
        return MakeError(ErrorCategory::kServer,
                         "schwab orders: more than " +
                             std::to_string(kSchwabOrdersPageCap) +
                             " entered from " + types::FormatIso8601(from) +
                             " to " + types::FormatIso8601(to));
    }
    const auto middle = from + (to - from) / 2;
    auto newer = FetchOrders(hash, middle, to);
    if (!newer) {
        return newer;
    }
    auto older = FetchOrders(hash, from, middle);
    if (!older) {
        return older;
    }
    json orders = json::array();
    std::unordered_set<std::string> ids;  // the halves meet at `middle`
    for (const json* half : {&*newer, &*older}) {
        for (const auto& item : *half) {
            if (auto id = IdMember(item, "orderId");
                !id || ids.insert(*id).second) {
                orders.push_back(item);
            }
        }
    }
    return orders;
}

Result<json> SchwabTradingVenue::RecentOrders(const std::string& hash,
                                              types::Timestamp placed_at,
                                              int attempt) const {
    if (attempt > 0) {
        Sleep(kLookupPause);
    }
    const auto now = std::max(placed_at, mOptions.clock());
    return FetchOrders(hash, placed_at - kRecoveryClockSkew,
                       now + kQueryClockSkew);
}

Result<json> SchwabTradingVenue::SecuritiesAccount(std::string_view what) {
    auto hash = AccountHash();
    if (!hash) {
        return std::unexpected(hash.error());
    }
    auto response = SendGet(AccountUrl(*hash), {{"fields", "positions"}});
    if (!response) {
        return std::unexpected(FormatSchwabError(response.error(), what));
    }
    auto parsed = util::ParseJson(response->body);
    if (!parsed || !parsed->is_object()) {
        return MakeError(ErrorCategory::kParse, "schwab " + std::string(what) +
                                                    ": expected a JSON object");
    }
    const auto account = parsed->find("securitiesAccount");
    if (account == parsed->end() || !account->is_object()) {
        return MakeError(
            ErrorCategory::kParse,
            "schwab " + std::string(what) + ": missing securitiesAccount");
    }
    return *account;
}

bool SchwabTradingVenue::IsOurs(const types::Symbol& symbol) const {
    return mSymbols.empty() || mSymbols.contains(symbol);
}

// An order that fills at or after `since` was working at the last fetch
// before then and at every fetch since, or was entered after the last of
// them; and each fetch knows the oldest order working when it ran. So the
// orders to read start at the oldest of those. Without a fetch before
// `since`, back as far as a GTC order lives.
types::Timestamp SchwabTradingVenue::OrdersFrom(types::Timestamp since,
                                                types::Timestamp now) const {
    const auto earliest = now - kMaxOrderQuerySpan;
    std::optional<types::Timestamp> oldest;
    {
        std::lock_guard lock(mMutex);
        auto it = std::ranges::upper_bound(mScans, since, {}, &OrderScan::time);
        if (it != mScans.begin()) {
            for (--it; it != mScans.end(); ++it) {
                oldest = std::min(oldest.value_or(it->oldest_working),
                                  it->oldest_working);
            }
        }
    }
    if (oldest) {
        return std::clamp(*oldest - kQueryClockSkew, earliest, now);
    }
    const auto start = std::min(since, now);
    return start > earliest + kOrderLookback ? start - kOrderLookback
                                             : earliest;
}

void SchwabTradingVenue::RecordScan(types::Timestamp time,
                                    types::Timestamp oldest_working) {
    std::lock_guard lock(mMutex);
    mScans.insert(std::ranges::upper_bound(mScans, time, {}, &OrderScan::time),
                  OrderScan{time, oldest_working});
    if (mScans.size() > kMaxScans) {
        // Merged, the pair still covers what either did.
        mScans[1].time = mScans[0].time;
        mScans[1].oldest_working =
            std::min(mScans[0].oldest_working, mScans[1].oldest_working);
        mScans.pop_front();
    }
}

Result<std::string> SchwabTradingVenue::AccountHash() {
    {
        std::lock_guard lock(mMutex);
        if (mAccountHash) {
            return *mAccountHash;
        }
    }
    auto response = SendGet(mOptions.base_url + "/accounts/accountNumbers");
    if (!response) {
        return std::unexpected(
            FormatSchwabError(response.error(), "accountNumbers"));
    }
    auto parsed = util::ParseJson(response->body);
    if (!parsed || !parsed->is_array()) {
        return MakeError(ErrorCategory::kParse,
                         "schwab accountNumbers: expected a JSON array");
    }
    if (parsed->empty()) {
        return MakeError(
            ErrorCategory::kAuth,
            "schwab accountNumbers: no accounts are linked to this "
            "authorization");
    }

    std::string chosen_hash;
    std::string first_number;
    for (const auto& entry : *parsed) {
        auto acc_num = TextMember(entry, "accountNumber");
        auto hash_val = TextMember(entry, "hashValue");
        if (!acc_num) return std::unexpected(acc_num.error());
        if (!hash_val) return std::unexpected(hash_val.error());
        if (acc_num->empty() || hash_val->empty()) {
            return MakeError(
                ErrorCategory::kParse,
                "schwab accountNumbers: empty accountNumber or hashValue");
        }
        if (first_number.empty()) {
            first_number = *acc_num;
        }
        if (!mOptions.account_number.empty()) {
            if (*acc_num == mOptions.account_number ||
                *hash_val == mOptions.account_number) {
                chosen_hash = *hash_val;
                break;
            }
        } else if (chosen_hash.empty()) {
            chosen_hash = *hash_val;
        }
    }

    if (chosen_hash.empty()) {
        return MakeError(ErrorCategory::kNotFound,
                         "schwab accountNumbers: configured account " +
                             MaskAccountNumber(mOptions.account_number) +
                             " is not among the linked accounts");
    }

    std::lock_guard lock(mMutex);
    if (!mAccountHash) {
        if (mOptions.account_number.empty() && parsed->size() > 1) {
            spdlog::warn(
                "schwab: {} accounts are linked; using account {} (set "
                "[schwab] account_number to select another)",
                parsed->size(), MaskAccountNumber(first_number));
        }
        mAccountHash = chosen_hash;
    }
    return *mAccountHash;
}

Result<SchwabTradingVenue::PreparedOrder> SchwabTradingVenue::PrepareOrder(
    const types::OrderRequest& request) const {
    auto symbol = types::NormalizeSymbol(request.symbol);
    if (!symbol) {
        return std::unexpected(symbol.error());
    }
    auto schwab_symbol = data::ToSchwabSymbol(*symbol);
    if (!schwab_symbol) {
        return std::unexpected(schwab_symbol.error());
    }
    if (request.post_only) {
        return MakeError(ErrorCategory::kUnsupported,
                         "schwab does not support post-only orders");
    }
    if (auto dur = SchwabDuration(request.type, request.time_in_force); !dur) {
        return std::unexpected(dur.error());
    }

    auto info = mDataSource->GetSymbolInfo(*symbol);
    if (!info) {
        return std::unexpected(info.error());
    }
    const std::optional<Decimal> order_price =
        request.type == OrderType::kLimit  ? request.limit_price
        : request.type == OrderType::kStop ? request.stop_price
                                           : std::nullopt;
    const types::SymbolInfo effective_info =
        EffectiveSymbolInfo(*info, order_price);

    types::OrderRequest normalized = request;
    normalized.symbol = *symbol;
    auto conformed = ApplySymbolRules(normalized, effective_info);
    if (!conformed) {
        return std::unexpected(conformed.error());
    }
    auto body = BuildSingleOrderJson(*conformed, *schwab_symbol);
    if (!body) {
        return std::unexpected(body.error());
    }
    return PreparedOrder{std::move(*conformed), std::move(*schwab_symbol),
                         std::move(*body)};
}

Result<std::optional<OrderStatus>> SchwabTradingVenue::RecoverPlacedOrder(
    const std::string& hash, const types::OrderRequest& conformed,
    types::Timestamp placed_at,
    const std::unordered_set<std::string>& known_before) {
    const auto earliest = placed_at - kRecoveryClockSkew;
    for (int attempt = 0; attempt < 2; ++attempt) {
        auto parsed = RecentOrders(hash, placed_at, attempt);
        if (!parsed) {
            if (attempt == 1) {
                return std::unexpected(parsed.error());
            }
            continue;
        }
        std::vector<OrderStatus> orders;
        for (const auto& item : *parsed) {
            if (!item.is_object()) {
                return MakeError(ErrorCategory::kParse,
                                 "schwab: order entry is not a JSON object");
            }
            // Top-level single orders only, so that a lost single order is
            // never taken for a leg of an OCO or a replacement.
            const std::string strategy_type =
                OptionalText(item, "orderStrategyType");
            if (!strategy_type.empty() && strategy_type != "SINGLE") {
                continue;
            }
            const auto legs_it = item.find("orderLegCollection");
            if (legs_it == item.end() || !legs_it->is_array() ||
                legs_it->empty()) {
                continue;
            }
            auto equity = HasOnlyEquityLegs(*legs_it);
            if (!equity) {
                return std::unexpected(equity.error());
            }
            if (!*equity) {
                continue;
            }
            auto id = IdMember(item, "orderId");
            if (!id) {
                return std::unexpected(id.error());
            }
            auto single = ParseSingleOrder(item, ClientOrderIdFor(*id));
            if (!single) {
                if (single.error().category == ErrorCategory::kUnsupported) {
                    continue;
                }
                return std::unexpected(single.error());
            }
            orders.push_back(std::move(*single));
        }
        const auto claimed_now = ClaimedOrdersSnapshot();
        std::vector<OrderStatus> matches;
        for (const auto& order : orders) {
            if (MatchesPlacedOrder(order, conformed, earliest, known_before,
                                   claimed_now)) {
                matches.push_back(order);
            }
        }
        if (matches.size() > 1) {
            spdlog::warn(
                "schwab: {} orders could be the timed-out placement on {}; "
                "not guessing",
                matches.size(), conformed.symbol);
            return std::optional<OrderStatus>{};
        }
        if (matches.size() == 1 &&
            TryClaimRecoveredOrder(matches[0].order_id,
                                   conformed.client_order_id)) {
            return std::optional<OrderStatus>(std::move(matches[0]));
        }
    }
    return std::optional<OrderStatus>{};
}

Result<OrderAcknowledgement> SchwabTradingVenue::PlaceOrder(
    const types::OrderRequest& request) {
    auto prepared = PrepareOrder(request);
    if (!prepared) {
        return std::unexpected(prepared.error());
    }
    auto hash = AccountHash();
    if (!hash) {
        return std::unexpected(hash.error());
    }

    const auto known_before = KnownOrdersSnapshot();
    const auto placed_at = mOptions.clock();
    auto response =
        SendOrderRequest(net::HttpMethod::kPost, AccountUrl(*hash, "/orders"),
                         prepared->json_body);
    if (!response) {
        types::Error err = FormatSchwabError(
            response.error(), "place order " + prepared->conformed.symbol);
        if (OutcomeUnknown(err.category)) {
            if (auto recovered = RecoverPlacedOrder(*hash, prepared->conformed,
                                                    placed_at, known_before);
                recovered && *recovered) {
                spdlog::warn(
                    "schwab: placing an order failed ({}), but order {} is "
                    "on the account",
                    err.ToString(), (*recovered)->order_id);
                return AckFrom(**recovered, placed_at, request.client_order_id);
            }
            err.message += "; it may have been placed: check the open orders";
        }
        return std::unexpected(std::move(err));
    }

    std::string order_id = ExtractOrderId(*response);
    if (order_id.empty()) {
        if (auto recovered = RecoverPlacedOrder(*hash, prepared->conformed,
                                                placed_at, known_before);
            recovered && *recovered) {
            return AckFrom(**recovered, placed_at, request.client_order_id);
        }
        return MakeError(
            ErrorCategory::kServer,
            "schwab place order " + prepared->conformed.symbol +
                ": accepted (HTTP " + std::to_string(response->status) +
                ") without an order ID, and it could not be found: check "
                "the open orders");
    }

    ClaimOrder(order_id, request.client_order_id);
    return OrderAcknowledgement{std::move(order_id), request.client_order_id,
                                OrderState::kNew, placed_at};
}

Result<void> SchwabTradingVenue::TestOrder(const types::OrderRequest& request) {
    auto prepared = PrepareOrder(request);
    if (!prepared) {
        return std::unexpected(prepared.error());
    }
    auto hash = AccountHash();
    if (!hash) {
        return std::unexpected(hash.error());
    }

    // previewOrder takes the same body as placing (as schwab-py sends it),
    // not the PreviewOrder schema the API document shows.
    auto response = SendOrderRequest(net::HttpMethod::kPost,
                                     AccountUrl(*hash, "/previewOrder"),
                                     prepared->json_body);
    if (!response) {
        return std::unexpected(FormatSchwabError(
            response.error(), "preview order " + prepared->conformed.symbol));
    }
    if (response->body.empty()) {
        return {};
    }

    auto parsed = util::ParseJson(response->body);
    if (!parsed || !parsed->is_object()) {
        return MakeError(ErrorCategory::kParse,
                         "schwab previewOrder: expected a JSON object");
    }

    std::string reject_reasons;
    const auto append_reason = [&reject_reasons](std::string_view text) {
        if (text.empty()) {
            return;
        }
        if (!reject_reasons.empty()) {
            reject_reasons += "; ";
        }
        reject_reasons += text;
    };
    const auto message_of = [](const json& item) {
        std::string msg = OptionalText(item, "message");
        return msg.empty() ? OptionalText(item, "activityMessage") : msg;
    };

    if (const auto val_it = parsed->find("orderValidationResult");
        val_it != parsed->end() && val_it->is_object()) {
        if (const auto rej_it = val_it->find("rejects");
            rej_it != val_it->end() && rej_it->is_array()) {
            for (const auto& item : *rej_it) {
                if (item.is_object()) {
                    std::string msg = message_of(item);
                    if (msg.empty()) {
                        msg = OptionalText(item, "validationRuleName");
                    }
                    append_reason(msg.empty() ? "validation reject" : msg);
                }
            }
        }
        if (const auto warn_it = val_it->find("warns");
            warn_it != val_it->end() && warn_it->is_array()) {
            for (const auto& item : *warn_it) {
                if (const std::string msg =
                        item.is_object() ? message_of(item) : "";
                    !msg.empty()) {
                    spdlog::warn("schwab previewOrder {}: {}",
                                 prepared->conformed.symbol, msg);
                }
            }
        }
    }

    std::string status = OptionalText(*parsed, "status");
    if (const auto strat_it = parsed->find("orderStrategy");
        strat_it != parsed->end() && strat_it->is_object()) {
        if (status.empty()) {
            status = OptionalText(*strat_it, "status");
        }
        if (const std::string desc =
                OptionalText(*strat_it, "statusDescription");
            !desc.empty() && status == "REJECTED") {
            append_reason(desc);
        }
    }

    if (!reject_reasons.empty() || status == "REJECTED") {
        return MakeError(
            ErrorCategory::kRejected,
            "schwab preview order " + prepared->conformed.symbol + ": " +
                (reject_reasons.empty() ? std::string("order rejected")
                                        : reject_reasons));
    }
    return {};
}

types::Error SchwabTradingVenue::FailOco(const OrderStatus& limit,
                                         const OrderStatus& stop) {
    std::string msg =
        "schwab OCO: a leg was rejected, or both closed without a fill";
    for (const auto* leg : {&limit, &stop}) {
        if (types::IsActive(leg->state)) {
            CancelOnFailure(leg->order_id, msg);
        }
    }
    return MakeError(ErrorCategory::kRejected, std::move(msg)).error();
}

Result<std::optional<types::OcoAcknowledgement>>
SchwabTradingVenue::RecoverPlacedOco(
    const std::string& hash, const types::OrderRequest& limit_leg,
    const types::OrderRequest& stop_leg, types::Timestamp placed_at,
    const std::unordered_set<std::string>& known_before,
    std::string_view expected_parent_id) {
    const auto earliest = placed_at - kRecoveryClockSkew;
    for (int attempt = 0; attempt < 2; ++attempt) {
        auto parsed = RecentOrders(hash, placed_at, attempt);
        if (!parsed) {
            if (attempt == 1) {
                return std::unexpected(parsed.error());
            }
            continue;
        }
        const auto claimed_now = ClaimedOrdersSnapshot();

        struct OcoCandidate {
            std::string parent_id;
            std::optional<OrderStatus> limit;
            std::optional<OrderStatus> stop;
        };
        std::vector<OcoCandidate> complete_matches;
        std::optional<OcoCandidate> partial_match;

        for (const auto& item : *parsed) {
            if (!item.is_object()) {
                return MakeError(ErrorCategory::kParse,
                                 "schwab: order entry is not a JSON object");
            }
            if (OptionalText(item, "orderStrategyType") != "OCO") {
                continue;
            }
            auto parent_id = IdMember(item, "orderId");
            if (!parent_id) {
                return std::unexpected(parent_id.error());
            }
            if (!expected_parent_id.empty() &&
                *parent_id != expected_parent_id) {
                continue;
            }
            if (known_before.contains(*parent_id) ||
                claimed_now.contains(*parent_id)) {
                continue;
            }
            std::vector<OrderStatus> children;
            std::unordered_set<std::string> seen;
            if (auto ok = CollectOrdersFromJson(item, ClientIds(), true,
                                                children, seen);
                !ok) {
                return std::unexpected(ok.error());
            }
            OcoCandidate cand{*parent_id, std::nullopt, std::nullopt};
            for (const auto& child : children) {
                if (!cand.limit &&
                    MatchesPlacedOrder(child, limit_leg, earliest, known_before,
                                       claimed_now)) {
                    cand.limit = child;
                } else if (!cand.stop &&
                           MatchesPlacedOrder(child, stop_leg, earliest,
                                              known_before, claimed_now)) {
                    cand.stop = child;
                }
            }
            if (cand.limit && cand.stop) {
                complete_matches.push_back(std::move(cand));
            } else if ((cand.limit || cand.stop) && !partial_match) {
                partial_match = std::move(cand);
            }
        }

        if (complete_matches.size() > 1) {
            spdlog::warn(
                "schwab: {} OCO orders could be the timed-out placement on "
                "{}; not guessing",
                complete_matches.size(), limit_leg.symbol);
            return std::optional<types::OcoAcknowledgement>{};
        }
        if (complete_matches.size() == 1) {
            const auto& match = complete_matches[0];
            if (!IsValidOcoState(*match.limit, *match.stop)) {
                return std::unexpected(FailOco(*match.limit, *match.stop));
            }
            if (!TryClaimRecoveredOco(match.parent_id, match.limit->order_id,
                                      match.stop->order_id)) {
                continue;
            }
            return std::optional<types::OcoAcknowledgement>(
                types::OcoAcknowledgement{AckFrom(*match.limit, placed_at),
                                          AckFrom(*match.stop, placed_at)});
        }
        if (attempt == 1 && partial_match) {
            const auto& lone = partial_match->limit ? *partial_match->limit
                                                    : *partial_match->stop;
            std::string msg = "schwab OCO: only one leg (" + lone.order_id +
                              ") is on the account";
            if (types::IsActive(lone.state)) {
                CancelOnFailure(lone.order_id, msg);
            }
            return MakeError(ErrorCategory::kRejected, std::move(msg));
        }
    }
    return std::optional<types::OcoAcknowledgement>{};
}

Result<types::OcoAcknowledgement> SchwabTradingVenue::PlaceOco(
    const types::OcoRequest& request) {
    if (request.limit_price.Sign() <= 0 || request.stop_price.Sign() <= 0) {
        return MakeError(ErrorCategory::kInvalidArgument,
                         "OCO limit and stop prices must be positive");
    }
    if (request.side == Side::kSell &&
        request.limit_price <= request.stop_price) {
        return MakeError(ErrorCategory::kInvalidArgument,
                         "sell OCO limit price must be above stop price");
    }
    if (request.side == Side::kBuy &&
        request.limit_price >= request.stop_price) {
        return MakeError(ErrorCategory::kInvalidArgument,
                         "buy OCO limit price must be below stop price");
    }

    auto symbol = types::NormalizeSymbol(request.symbol);
    if (!symbol) {
        return std::unexpected(symbol.error());
    }
    auto schwab_symbol = data::ToSchwabSymbol(*symbol);
    if (!schwab_symbol) {
        return std::unexpected(schwab_symbol.error());
    }
    auto info = mDataSource->GetSymbolInfo(*symbol);
    if (!info) {
        return std::unexpected(info.error());
    }

    types::OrderRequest limit_req;
    limit_req.symbol = *symbol;
    limit_req.side = request.side;
    limit_req.type = OrderType::kLimit;
    limit_req.quantity = request.quantity;
    limit_req.limit_price = request.limit_price;
    limit_req.time_in_force = TimeInForce::kGtc;

    types::OrderRequest stop_req = limit_req;
    stop_req.type = OrderType::kStop;
    stop_req.limit_price.reset();
    stop_req.stop_price = request.stop_price;

    auto conformed_limit = ApplySymbolRules(
        limit_req, EffectiveSymbolInfo(*info, request.limit_price));
    if (!conformed_limit) {
        return std::unexpected(conformed_limit.error());
    }
    auto conformed_stop = ApplySymbolRules(
        stop_req, EffectiveSymbolInfo(*info, request.stop_price));
    if (!conformed_stop) {
        return std::unexpected(conformed_stop.error());
    }
    if ((request.side == Side::kSell &&
         *conformed_limit->limit_price <= *conformed_stop->stop_price) ||
        (request.side == Side::kBuy &&
         *conformed_limit->limit_price >= *conformed_stop->stop_price)) {
        return MakeError(ErrorCategory::kRejected,
                         "OCO limit and stop prices round to the same tick");
    }

    auto limit_json = BuildSingleOrderJson(*conformed_limit, *schwab_symbol);
    auto stop_json = BuildSingleOrderJson(*conformed_stop, *schwab_symbol);
    if (!limit_json) return std::unexpected(limit_json.error());
    if (!stop_json) return std::unexpected(stop_json.error());

    auto hash = AccountHash();
    if (!hash) {
        return std::unexpected(hash.error());
    }

    const auto known_before = KnownOrdersSnapshot();
    const auto placed_at = mOptions.clock();
    auto response = SendOrderRequest(
        net::HttpMethod::kPost, AccountUrl(*hash, "/orders"),
        R"({"orderStrategyType":"OCO","childOrderStrategies":[)" + *limit_json +
            "," + *stop_json + "]}");
    if (!response) {
        types::Error err =
            FormatSchwabError(response.error(), "place OCO " + *symbol);
        if (OutcomeUnknown(err.category)) {
            auto recovered =
                RecoverPlacedOco(*hash, *conformed_limit, *conformed_stop,
                                 placed_at, known_before);
            if (!recovered) {
                return std::unexpected(recovered.error());
            }
            if (*recovered) {
                spdlog::warn(
                    "schwab: placing an OCO failed ({}), but its orders {} "
                    "and {} are on the account",
                    err.ToString(), (*recovered)->limit.order_id,
                    (*recovered)->stop.order_id);
                return **recovered;
            }
            err.message += "; it may have been placed: check the open orders";
        }
        return std::unexpected(std::move(err));
    }

    // The legs' order IDs are on the parent order, read back by the ID the
    // response gave.
    const std::string parent_id = ExtractOrderId(*response);
    if (!parent_id.empty()) {
        std::vector<OrderStatus> children;
        std::unordered_set<std::string> seen;
        if (auto detail = SendGet(
                AccountUrl(*hash, "/orders/" + util::UrlEncode(parent_id)));
            detail) {
            if (auto parsed = util::ParseJson(detail->body);
                parsed && parsed->is_object() &&
                CollectOrdersFromJson(*parsed, ClientIds(), true, children,
                                      seen)) {
                const auto leg = [&](OrderType type) {
                    const auto it =
                        std::ranges::find(children, type, &OrderStatus::type);
                    return it != children.end()
                               ? std::optional<OrderStatus>(*it)
                               : std::nullopt;
                };
                const auto limit_child = leg(OrderType::kLimit);
                const auto stop_child = leg(OrderType::kStop);
                if (limit_child && stop_child) {
                    ClaimOrder(parent_id, "");
                    if (!IsValidOcoState(*limit_child, *stop_child)) {
                        return std::unexpected(
                            FailOco(*limit_child, *stop_child));
                    }
                    ClaimOrder(limit_child->order_id, "");
                    ClaimOrder(stop_child->order_id, "");
                    return types::OcoAcknowledgement{
                        AckFrom(*limit_child, placed_at),
                        AckFrom(*stop_child, placed_at)};
                }
            }
        }
    }

    // `parent_id` is not claimed yet, so that the search can claim it (and
    // looks only at it, when the response gave one).
    auto recovered = RecoverPlacedOco(*hash, *conformed_limit, *conformed_stop,
                                      placed_at, known_before, parent_id);
    if (!recovered) {
        return std::unexpected(recovered.error());
    }
    if (*recovered) {
        if (!parent_id.empty()) {
            ClaimOrder(parent_id, "");
        }
        return **recovered;
    }

    std::string msg =
        "schwab place OCO: could not read the two legs after placing it";
    if (!parent_id.empty()) {
        ClaimOrder(parent_id, "");
        CancelOnFailure(parent_id, msg);
    } else {
        msg += ": check the open orders";
    }
    return MakeError(ErrorCategory::kServer, std::move(msg));
}

Result<CancelOutcome> SchwabTradingVenue::CancelOrder(
    const std::string& order_id) {
    if (order_id.empty()) {
        return MakeError(ErrorCategory::kInvalidArgument,
                         "order_id must not be empty");
    }
    auto hash = AccountHash();
    if (!hash) {
        return std::unexpected(hash.error());
    }

    auto response = SendOrderRequest(
        net::HttpMethod::kDelete,
        AccountUrl(*hash, "/orders/" + util::UrlEncode(order_id)));
    if (response) {
        return CancelOutcome::kCancelled;
    }

    types::Error err =
        FormatSchwabError(response.error(), "cancel order " + order_id);
    if (err.category == ErrorCategory::kNotFound ||
        err.category == ErrorCategory::kRejected ||
        OutcomeUnknown(err.category)) {
        if (auto current = GetOrder(order_id)) {
            if (current->state == OrderState::kFilled) {
                return CancelOutcome::kAlreadyFilled;
            }
            if (OutcomeUnknown(err.category) &&
                current->state == OrderState::kCancelled) {
                return CancelOutcome::kCancelled;
            }
            if (!types::IsActive(current->state)) {
                return CancelOutcome::kAlreadyClosed;
            }
            // Refused because a cancel is already on its way.
            if (auto detail = SendGet(AccountUrl(
                    *hash, "/orders/" + util::UrlEncode(order_id)))) {
                if (auto parsed = util::ParseJson(detail->body);
                    parsed && HasStatus(*parsed, order_id, "PENDING_CANCEL")) {
                    return CancelOutcome::kCancelled;
                }
            }
        }
    }
    return std::unexpected(std::move(err));
}

// The configured symbols' orders only (GetOpenOrders): orders on others are
// not Kestrel's to cancel.
Result<std::vector<std::string>> SchwabTradingVenue::CancelAll() {
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

Result<std::vector<OrderStatus>> SchwabTradingVenue::GetOpenOrders() {
    auto hash = AccountHash();
    if (!hash) {
        return std::unexpected(hash.error());
    }
    const auto now = mOptions.clock();
    const auto from = OrdersFrom(now, now);
    auto parsed = FetchOrders(*hash, from, now + kQueryClockSkew);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }

    std::vector<OrderStatus> orders;
    std::unordered_set<std::string> returned;
    types::Timestamp oldest_working = now;
    for (const auto& item : *parsed) {
        // Each item read in full, so that its own entry time counts even if
        // an order in it came up under an earlier one.
        std::vector<OrderStatus> nested;
        std::unordered_set<std::string> seen;
        if (auto ok =
                CollectOrdersFromJson(item, ClientIds(), true, nested, seen);
            !ok) {
            return std::unexpected(ok.error());
        }
        std::erase_if(
            nested, [this](const OrderStatus& o) { return !IsOurs(o.symbol); });
        if (auto since = WorkingSince(item, nested, now, from)) {
            oldest_working = std::min(oldest_working, *since);
        }
        for (auto& order : nested) {
            RememberOrder(order.order_id, "");
            if (types::IsActive(order.state) &&
                returned.insert(order.order_id).second) {
                orders.push_back(std::move(order));
            }
        }
    }
    RecordScan(now, oldest_working);
    return orders;
}

Result<OrderStatus> SchwabTradingVenue::GetOrder(const std::string& order_id) {
    if (order_id.empty()) {
        return MakeError(ErrorCategory::kInvalidArgument,
                         "order_id must not be empty");
    }
    auto hash = AccountHash();
    if (!hash) {
        return std::unexpected(hash.error());
    }
    auto response =
        SendGet(AccountUrl(*hash, "/orders/" + util::UrlEncode(order_id)));
    if (!response) {
        return std::unexpected(
            FormatSchwabError(response.error(), "get order " + order_id));
    }

    auto parsed = util::ParseJson(response->body);
    if (!parsed || !parsed->is_object()) {
        return MakeError(ErrorCategory::kParse,
                         "schwab get order: expected a JSON object");
    }

    // An unsupported sibling in an OCO is skipped, but the order asked for
    // fails if it is unsupported itself.
    std::vector<OrderStatus> collected;
    std::unordered_set<std::string> seen;
    if (auto ok = CollectOrdersFromJson(*parsed, ClientIds(), true, collected,
                                        seen, order_id);
        !ok) {
        return std::unexpected(ok.error());
    }
    for (auto& candidate : collected) {
        RememberOrder(candidate.order_id, "");
        if (candidate.order_id == order_id) {
            return candidate;
        }
    }

    // A parent order without legs of its own (an OCO): its own status, with
    // its legs' fills added up.
    if (auto top_id = IdMember(*parsed, "orderId");
        top_id && *top_id == order_id) {
        auto top_status = TextMember(*parsed, "status");
        if (!top_status) {
            return std::unexpected(top_status.error());
        }
        Decimal total_filled;
        for (const auto& child : collected) {
            total_filled += child.filled_quantity;
        }
        auto state = OrderStateFrom(*top_status, total_filled);
        if (!state) {
            return std::unexpected(state.error());
        }
        OrderStatus parent;
        if (!collected.empty()) {
            parent = collected.front();
        }
        parent.order_id = order_id;
        parent.client_order_id = ClientOrderIdFor(order_id);
        parent.filled_quantity = total_filled;
        parent.state = *state;
        RememberOrder(order_id, "");
        return parent;
    }

    return MakeError(
        ErrorCategory::kNotFound,
        "schwab get order " + order_id + ": order ID not found in response");
}

Result<std::vector<types::Fill>> SchwabTradingVenue::GetFills(
    types::Timestamp since) {
    auto hash = AccountHash();
    if (!hash) {
        return std::unexpected(hash.error());
    }
    const auto now = mOptions.clock();
    const auto from = OrdersFrom(since, now);
    auto parsed =
        FetchOrders(*hash, from, std::max(now, since) + kQueryClockSkew);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }

    std::vector<types::Fill> fills;
    std::unordered_set<std::string> seen_fills;
    types::Timestamp oldest_working = now;
    const auto client_ids = ClientIds();
    for (const auto& item : *parsed) {
        if (auto ok = CollectFillsFromJson(item, since, client_ids, fills,
                                           seen_fills);
            !ok) {
            return std::unexpected(ok.error());
        }
        std::vector<OrderStatus> nested;
        std::unordered_set<std::string> seen;  // each item in full
        if (auto ok =
                CollectOrdersFromJson(item, client_ids, true, nested, seen);
            !ok) {
            return std::unexpected(ok.error());
        }
        std::erase_if(
            nested, [this](const OrderStatus& o) { return !IsOurs(o.symbol); });
        if (auto working = WorkingSince(item, nested, now, from)) {
            oldest_working = std::min(oldest_working, *working);
        }
    }
    std::erase_if(fills,
                  [this](const types::Fill& f) { return !IsOurs(f.symbol); });
    RecordScan(now, oldest_working);
    std::ranges::stable_sort(fills, {}, &types::Fill::time);
    return fills;
}

Result<std::vector<types::Position>> SchwabTradingVenue::GetPositions() {
    auto account = SecuritiesAccount("positions");
    if (!account) {
        return std::unexpected(account.error());
    }
    return ParseEquityPositions(*account, "positions");
}

Result<std::vector<types::Balance>> SchwabTradingVenue::GetBalances() {
    auto account = SecuritiesAccount("balances");
    if (!account) {
        return std::unexpected(account.error());
    }
    const auto object = [&](const char* key) {
        const auto it = account->find(key);
        return it != account->end() && it->is_object() ? *it : json::object();
    };
    if (const auto it = account->find("currentBalances");
        it == account->end() || !it->is_object()) {
        return MakeError(ErrorCategory::kParse,
                         "schwab balances: missing currentBalances");
    }
    auto usd = ResolveUsdCash(
        OptionalText(*account, "type"), object("currentBalances"),
        object("projectedBalances"), object("initialBalances"));
    if (!usd) {
        return std::unexpected(usd.error());
    }

    std::vector<types::Balance> balances;
    balances.push_back(std::move(*usd));

    // Shares held, so that what values the account from its balances (the
    // risk gate's equity, momentum_rotation) counts them alongside the cash.
    auto positions = ParseEquityPositions(*account, "balances");
    if (!positions) {
        return std::unexpected(positions.error());
    }
    for (auto& pos : *positions) {
        if (pos.quantity.Sign() > 0) {
            balances.push_back(types::Balance{
                std::move(pos.symbol), pos.quantity, pos.quantity, Decimal()});
        }
    }
    return balances;
}

}  // namespace kestrel::trading
