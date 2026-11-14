#include "kestrel/strategy/single_order_position.hpp"

#include <algorithm>
#include <utility>

#include "kestrel/strategy/orders.hpp"

namespace kestrel::strategy {

using types::Decimal;
using types::Side;

SingleOrderPosition::SingleOrderPosition(types::Symbol symbol, Decimal quantity)
    : mSymbol(std::move(symbol)),
      mQuantity(quantity),
      mBaseAsset(BaseAsset(mSymbol)) {}

LogContext SingleOrderPosition::Log(Context& context) const {
    return context.Log().WithDefault("symbol", mSymbol);
}

bool SingleOrderPosition::Buy(Context& context) {
    if (Busy()) {
        return false;
    }
    const Decimal quantity = RoundDownToLot(context, mSymbol, mQuantity);
    if (quantity.Sign() <= 0) {
        Log(context).Warn("quantity {} is under one lot; no buy",
                          mQuantity.ToString());
        return false;
    }
    return Place(context, Side::kBuy, quantity);
}

bool SingleOrderPosition::Sell(Context& context) {
    if (Busy()) {
        return false;
    }
    // Long with nothing counted (a venue that reported no fill quantities):
    // sell what was bought.
    const Decimal quantity =
        RoundDownToLot(context, mSymbol, mHeld.Sign() > 0 ? mHeld : mQuantity);
    if (quantity.Sign() <= 0) {
        Log(context).Warn("{} is under one lot and cannot be sold; flat",
                          mHeld.ToString());
        Exit();
        return false;
    }
    return Place(context, Side::kSell, quantity);
}

bool SingleOrderPosition::Place(Context& context, Side side,
                                const Decimal& quantity) {
    types::OrderRequest request;
    request.symbol = mSymbol;
    request.side = side;
    request.type = types::OrderType::kMarket;
    request.quantity = quantity;
    const auto ack = Submit(context, request, types::ToString(side));
    if (!ack) {
        return false;
    }
    if (side == Side::kBuy && !mLong) {
        mEntryCost = mEntryFilled = mEntryPrice = Decimal();
    }
    mOrder = Order{ack->order_id, side, quantity, quantity, Decimal(), false};
    Log(context).Info("{} {} (order {})", types::ToString(side),
                      quantity.ToString(), ack->order_id);
    return true;
}

SingleOrderPosition::Change SingleOrderPosition::OnFill(
    Context& context, const types::Fill& fill) {
    if (fill.symbol != mSymbol) {
        return Change::kNone;
    }
    const bool ours =
        mOrder && (fill.order_id == mOrder->id ||
                   (fill.order_id.empty() && fill.side == mOrder->side));
    // A venue that reports no quantity: the rest of the order, or for
    // another order's fill, a whole entry or everything held.
    Decimal quantity = fill.quantity;
    if (quantity.Sign() <= 0) {
        if (ours) {
            quantity = std::max(mOrder->expected - mOrder->filled, Decimal());
        } else if (fill.side == Side::kBuy) {
            quantity = mQuantity;
        } else {
            quantity = mHeld;
        }
    }
    if (fill.side == Side::kBuy) {
        mHeld += ReceivedOnBuy(fill, mBaseAsset, quantity);
        NoteEntryFill(quantity, fill.price);
    } else {
        mHeld = std::max(mHeld - quantity, Decimal());
    }

    if (!ours) {
        if (fill.side == Side::kBuy) {
            if (mLong) {
                return Change::kNone;
            }
            mLong = true;
            return Change::kEntered;
        }
        return Exit();
    }
    mOrder->filled += quantity;
    return mOrder->filled >= mOrder->expected ? Finish(context) : Change::kNone;
}

SingleOrderPosition::Change SingleOrderPosition::OnOrderUpdate(
    Context& context, const types::OrderStatus& status) {
    if (!mOrder || status.order_id != mOrder->id) {
        return Change::kNone;
    }
    // Filled: what it filled is final, and may be less than was asked if
    // the venue rounded the order, so wait for that much, not more.
    if (status.state == types::OrderState::kFilled) {
        if (status.filled_quantity.Sign() > 0) {
            mOrder->expected = status.filled_quantity;
        }
        return mOrder->filled >= mOrder->expected ? Finish(context)
                                                  : Change::kNone;
    }
    if (!ClosedUnfilled(status.state)) {
        return Change::kNone;
    }
    // Closed early: what it filled is final, though its fills may still be
    // on their way. A venue that reports less than has already arrived
    // (some report nothing filled on a cancel) is behind: go by the fills.
    const Decimal filled = std::max(status.filled_quantity, mOrder->filled);
    if (filled.Sign() == 0) {
        Log(context).Warn("order {} closed without a fill: {}", status.order_id,
                          types::ToString(status.state));
    }
    mOrder->closed = true;
    mOrder->expected = filled;
    return mOrder->filled >= mOrder->expected ? Finish(context) : Change::kNone;
}

SingleOrderPosition::Change SingleOrderPosition::Finish(Context& context) {
    const Order order = *std::exchange(mOrder, std::nullopt);
    if (order.side == Side::kBuy) {
        if (order.filled.Sign() <= 0 || mLong) {
            return Change::kNone;
        }
        if (order.closed) {
            Log(context).Warn("bought {} of {}", order.filled.ToString(),
                              order.requested.ToString());
        }
        mLong = true;
        return Change::kEntered;
    }
    if (order.closed && order.filled < order.requested && mHeld.Sign() > 0) {
        Log(context).Warn("sold {} of {}; still holds {}",
                          order.filled.ToString(), order.requested.ToString(),
                          mHeld.ToString());
        return Change::kNone;  // still long: the strategy sells again
    }
    return Exit();
}

SingleOrderPosition::Change SingleOrderPosition::Exit() {
    if (!mLong) {
        return Change::kNone;
    }
    // What is left (under a lot, from a fee in the coin) stays counted and
    // goes with the next exit.
    mLong = false;
    mEntryCost = mEntryFilled = mEntryPrice = Decimal();
    return Change::kExited;
}

void SingleOrderPosition::NoteEntryFill(const Decimal& quantity,
                                        const Decimal& price) {
    if (price.Sign() <= 0 || quantity.Sign() <= 0) {
        return;
    }
    if (mEntryFilled.Sign() == 0) {
        mEntryCost = quantity * price;
        mEntryFilled = quantity;
        mEntryPrice = price;
        return;
    }
    mEntryCost += quantity * price;
    mEntryFilled += quantity;
    mEntryPrice =
        Decimal::Divide(mEntryCost, mEntryFilled,
                        std::max({8, mEntryPrice.Scale(), price.Scale()}),
                        types::Rounding::kNearest);
}

}  // namespace kestrel::strategy
