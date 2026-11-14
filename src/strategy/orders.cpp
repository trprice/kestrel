#include "kestrel/strategy/orders.hpp"

#include <algorithm>

namespace kestrel::strategy {

using types::Decimal;
using types::OrderState;

std::string BaseAsset(const types::Symbol& symbol) {
    if (const auto parts = types::SplitSymbol(symbol)) {
        return parts->base;
    }
    return {};
}

bool ClosedUnfilled(OrderState state) {
    return state == OrderState::kCancelled || state == OrderState::kRejected ||
           state == OrderState::kExpired;
}

bool FeeInBase(const types::Fill& fill, std::string_view base_asset) {
    return fill.fee.Sign() > 0 && !base_asset.empty() &&
           fill.fee_asset == base_asset;
}

Decimal ReceivedOnBuy(const types::Fill& fill, std::string_view base_asset) {
    return ReceivedOnBuy(fill, base_asset, fill.quantity);
}

Decimal ReceivedOnBuy(const types::Fill& fill, std::string_view base_asset,
                      const Decimal& quantity) {
    if (!FeeInBase(fill, base_asset)) {
        return quantity;
    }
    return std::max(quantity - fill.fee, Decimal());
}

Decimal RoundDownToLot(Context& context, const types::Symbol& symbol,
                       const Decimal& quantity) {
    if (auto info = context.SymbolInfo(symbol);
        info && info->lot_size.Sign() > 0) {
        return quantity.RoundToIncrement(info->lot_size,
                                         types::Rounding::kDown);
    }
    return quantity;
}

std::optional<types::OrderAcknowledgement> Submit(
    Context& context, const types::OrderRequest& request,
    std::string_view what) {
    auto ack = context.Venue().PlaceOrder(request);
    if (!ack) {
        context.Log().Warn("{} {} failed: {}", what, request.symbol,
                           ack.error().ToString());
        return std::nullopt;
    }
    if (ClosedUnfilled(ack->state)) {
        context.Log().Warn("{} {} closed without a fill: {}", what,
                           request.symbol, types::ToString(ack->state));
        return std::nullopt;
    }
    return *std::move(ack);
}

}  // namespace kestrel::strategy
