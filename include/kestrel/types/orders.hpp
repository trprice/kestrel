#pragma once

#include <optional>
#include <string>

#include "kestrel/types/decimal.hpp"
#include "kestrel/types/symbol.hpp"
#include "kestrel/types/time.hpp"

namespace kestrel::types {

enum class Side { kBuy, kSell };
enum class OrderType { kMarket, kLimit, kStop /* stop-market */ };
enum class TimeInForce { kGtc, kIoc, kFok, kDay };
enum class OrderState {
    kNew,
    kPartiallyFilled,
    kFilled,
    kCancelled,
    kRejected,
    kExpired
};
enum class Liquidity { kMaker, kTaker };

std::string_view ToString(Side);
std::string_view ToString(OrderType);
std::string_view ToString(TimeInForce);
std::string_view ToString(OrderState);

inline bool IsActive(OrderState s) {
    return s == OrderState::kNew || s == OrderState::kPartiallyFilled;
}

struct OrderRequest {
    Symbol symbol;
    Side side = Side::kBuy;
    OrderType type = OrderType::kMarket;
    Decimal quantity;                    // base-asset units
    std::optional<Decimal> limit_price;  // required for kLimit
    std::optional<Decimal> stop_price;   // required for kStop
    TimeInForce time_in_force = TimeInForce::kGtc;
    std::string client_order_id;  // caller-chosen, may be empty
};

struct OrderAcknowledgement {
    std::string order_id;  // the venue's ID
    std::string client_order_id;
    OrderState state = OrderState::kNew;
    Timestamp time;
};

struct OrderStatus {
    std::string order_id;
    std::string client_order_id;
    Symbol symbol;
    Side side = Side::kBuy;
    OrderType type = OrderType::kMarket;
    TimeInForce time_in_force = TimeInForce::kGtc;
    Decimal quantity;  // after tick/lot rounding
    Decimal filled_quantity;
    std::optional<Decimal> limit_price;  // after tick rounding
    std::optional<Decimal> stop_price;
    std::optional<Decimal> average_fill_price;
    OrderState state = OrderState::kNew;
    Timestamp created_time;
    Timestamp updated_time;
    std::optional<std::string>
        reason;  // why rejected/cancelled/expired, if known
};

struct Fill {
    std::string fill_id;
    std::string order_id;
    std::string client_order_id;
    Symbol symbol;
    Side side = Side::kBuy;
    Decimal quantity;
    Decimal price;
    Decimal fee;
    std::string fee_asset;
    Liquidity liquidity = Liquidity::kTaker;
    Timestamp time;
};

struct Position {
    Symbol symbol;
    Decimal quantity;       // base-asset units held
    Decimal average_price;  // average entry price of the open quantity
    Decimal realized_pnl;   // in the quote asset, net of fees
};

struct Balance {
    std::string asset;
    Decimal total;
    Decimal available;  // total minus locked
    Decimal locked;     // reserved by open orders
};

}  // namespace kestrel::types
