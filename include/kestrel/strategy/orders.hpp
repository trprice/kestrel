#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "kestrel/strategy/strategy.hpp"
#include "kestrel/types/decimal.hpp"

// Small pieces of order handling the strategies share.
namespace kestrel::strategy {

// The base asset of a symbol ("BTC" of "BTC/USD"); empty if it does not
// split into one.
std::string BaseAsset(const types::Symbol& symbol);

// Closed without filling completely (perhaps partly): cancelled, rejected or
// expired.
bool ClosedUnfilled(types::OrderState state);

// Whether the fill's fee was taken in `base_asset` (Binance.US takes a buy's
// fee in what was bought). False for an empty `base_asset`.
bool FeeInBase(const types::Fill& fill, std::string_view base_asset);

// What a buy's fill added to the base balance: its quantity (or `quantity`,
// for a fill that reported none), less a fee taken in the base asset; never
// below zero.
types::Decimal ReceivedOnBuy(const types::Fill& fill,
                             std::string_view base_asset);
types::Decimal ReceivedOnBuy(const types::Fill& fill,
                             std::string_view base_asset,
                             const types::Decimal& quantity);

// `quantity` rounded down to the symbol's lot size, which may make it zero;
// unchanged if the venue gives no lot size.
types::Decimal RoundDownToLot(Context& context, const types::Symbol& symbol,
                              const types::Decimal& quantity);

// Places the order. If the venue refuses it, or acknowledges it already
// closed without a fill (no fill will come), logs a warning as
// "<tag>: <what> <symbol> ..." and returns nothing.
std::optional<types::OrderAcknowledgement> Submit(
    Context& context, const types::OrderRequest& request, std::string_view tag,
    std::string_view what);

}  // namespace kestrel::strategy
