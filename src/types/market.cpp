#include "kestrel/types/market.hpp"

#include "kestrel/types/orders.hpp"

namespace kestrel::types {

void Quote::MergeFrom(const Quote& update) {
    if (!update.symbol.empty()) {
        symbol = update.symbol;
    }
    if (!update.venue.empty()) {
        venue = update.venue;
    }
    received_time = update.received_time;
    if (update.exchange_time) {
        exchange_time = update.exchange_time;
    }
    if (update.bid) {
        bid = update.bid;
    }
    if (update.bid_size) {
        bid_size = update.bid_size;
    }
    if (update.ask) {
        ask = update.ask;
    }
    if (update.ask_size) {
        ask_size = update.ask_size;
    }
    if (update.last) {
        last = update.last;
    }
    if (update.last_size) {
        last_size = update.last_size;
    }
}

std::string_view ToString(Side s) { return s == Side::kBuy ? "buy" : "sell"; }

std::string_view ToString(OrderType t) {
    switch (t) {
        case OrderType::kMarket:
            return "market";
        case OrderType::kLimit:
            return "limit";
        case OrderType::kStop:
            return "stop";
    }
    return "unknown";
}

std::string_view ToString(TimeInForce t) {
    switch (t) {
        case TimeInForce::kGtc:
            return "gtc";
        case TimeInForce::kIoc:
            return "ioc";
        case TimeInForce::kFok:
            return "fok";
        case TimeInForce::kDay:
            return "day";
    }
    return "unknown";
}

std::string_view ToString(OrderState s) {
    switch (s) {
        case OrderState::kNew:
            return "new";
        case OrderState::kPartiallyFilled:
            return "partially_filled";
        case OrderState::kFilled:
            return "filled";
        case OrderState::kCancelled:
            return "cancelled";
        case OrderState::kRejected:
            return "rejected";
        case OrderState::kExpired:
            return "expired";
    }
    return "unknown";
}

}  // namespace kestrel::types
