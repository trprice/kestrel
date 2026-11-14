#pragma once

#include <optional>
#include <string>

#include "kestrel/strategy/strategy.hpp"
#include "kestrel/types/decimal.hpp"

namespace kestrel::strategy {

// A long position in one symbol, entered and left by market orders one at a
// time: the order handling ma_crossover and mean_reversion share.
//
// The position changes on the fills, not on the order. An order stays
// working until its fills add up to its quantity, or, once it has closed, to
// what the venue says it filled (a venue that rounds the order fills less
// than was asked); so a market order filled in parts is one
// entry or one exit, and nothing new is sent while its parts come in. A
// buy's fee taken in the coin is off what is held, and a sell is for what is
// held, rounded down to the lot.
//
// A fill of another order (a risk gate closing the position, say) still
// changes what is held: a buy makes it long, a sell flat. A fill without an
// order id is taken as the working order's if it is on the same side.
class SingleOrderPosition {
   public:
    // What a fill or an update did to the position.
    enum class Change { kNone, kEntered, kExited };

    SingleOrderPosition(types::Symbol symbol, types::Decimal quantity);

    bool Busy() const { return mOrder.has_value(); }  // an order is working
    bool Long() const { return mLong; }
    const types::Decimal& Held() const { return mHeld; }
    // The average price of the entry's fills (and of any fill of another
    // buy while long); zero when flat.
    const types::Decimal& EntryPrice() const { return mEntryPrice; }

    // Buys `quantity`, rounded down to the lot, at market; whether the venue
    // took the order. Nothing is sent while an order is working.
    bool Buy(Context& context);
    // Sells what is held at market; whether the venue took the order. Less
    // than one lot is dust that cannot be sold: nothing is sent, and the
    // position counts as flat at once (false, and Long() is false).
    bool Sell(Context& context);

    Change OnFill(Context& context, const types::Fill& fill);
    Change OnOrderUpdate(Context& context, const types::OrderStatus& status);

   private:
    struct Order {
        std::string id;
        types::Side side = types::Side::kBuy;
        types::Decimal requested;
        types::Decimal expected;  // `requested`, or what it filled once closed
        types::Decimal filled;
        bool closed = false;
    };

    bool Place(Context& context, types::Side side,
               const types::Decimal& quantity);
    Change Finish(Context& context);
    Change Exit();
    void NoteEntryFill(const types::Decimal& quantity,
                       const types::Decimal& price);

    types::Symbol mSymbol;
    types::Decimal mQuantity;
    std::string mBaseAsset;
    types::Decimal mHeld;
    bool mLong = false;
    std::optional<Order> mOrder;
    types::Decimal mEntryCost;
    types::Decimal mEntryFilled;
    types::Decimal mEntryPrice;
};

}  // namespace kestrel::strategy
