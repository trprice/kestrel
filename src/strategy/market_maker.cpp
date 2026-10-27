#include "kestrel/strategy/market_maker.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <stdexcept>

namespace kestrel::strategy {

using types::Decimal;
using types::Rounding;
using types::Side;

namespace {

constexpr int kScale = 8;
const Decimal kBpsDivisor = Decimal::FromInt(10000);

// `value` * `bps` / 10000.
Decimal Bps(const Decimal& value, const Decimal& bps) {
    return Decimal::Divide(value * bps, kBpsDivisor, kScale, Rounding::kNearest);
}

}  // namespace

MarketMaker::MarketMaker(MarketMakerParams params)
    : mParams(std::move(params)) {
    if (mParams.quantity.Sign() <= 0) {
        throw std::invalid_argument("quantity must be positive");
    }
    if (mParams.half_spread_bps.Sign() <= 0) {
        throw std::invalid_argument("half_spread_bps must be positive");
    }
    if (mParams.max_inventory < mParams.quantity) {
        throw std::invalid_argument(
            "max_inventory must be at least one quote's quantity");
    }
    if (mParams.skew_bps.IsNegative() || mParams.requote_bps.IsNegative() ||
        mParams.max_quote_age < std::chrono::milliseconds::zero()) {
        throw std::invalid_argument(
            "skew_bps, requote_bps and max_quote_age cannot be negative");
    }
}

Requirements MarketMaker::Needs() const {
    Requirements needs;
    needs.symbols = {mParams.symbol};
    needs.quotes = true;
    return needs;
}

void MarketMaker::OnStart(Context& context) {
    auto info = context.SymbolInfo(mParams.symbol);
    if (!info) {
        spdlog::error("market_maker: no symbol info for {}: {}", mParams.symbol,
                      info.error().ToString());
        return;
    }
    mInfo = *info;
    // Spot: what is held is the base asset's balance. (Positions only cover
    // what was bought, not a balance the account started with.)
    const auto parts = types::SplitSymbol(mParams.symbol);
    auto balances = context.Venue().GetBalances();
    if (!parts || !balances) {
        spdlog::error("market_maker: cannot read the {} balance: {}",
                      mParams.symbol,
                      parts ? balances.error().ToString()
                            : parts.error().ToString());
        return;
    }
    for (const auto& balance : *balances) {
        if (balance.asset == parts->base) {
            mInventory = balance.total;
        }
    }
    mStart = mInventory;
    mReady = true;
}

Decimal MarketMaker::Size(const Decimal& room) const {
    const Decimal size = std::min(mParams.quantity, room);
    if (size.Sign() <= 0) {
        return Decimal();
    }
    const Decimal rounded = size.RoundToIncrement(mInfo.lot_size, Rounding::kDown);
    return rounded < mInfo.min_order_size ? Decimal() : rounded;
}

void MarketMaker::OnQuote(Context& context, const types::Quote& quote) {
    if (!mReady || quote.symbol != mParams.symbol || !quote.bid || !quote.ask ||
        quote.bid->Sign() <= 0 || *quote.bid >= *quote.ask) {
        return;
    }
    const Decimal tick = mInfo.tick_size;
    const Decimal mid = Decimal::Divide(*quote.bid + *quote.ask,
                                        Decimal::FromInt(2), kScale,
                                        Rounding::kNearest);

    // Lean away from the side the position is heavy on.
    Decimal lean = Decimal::Divide(mInventory - mStart, mParams.max_inventory,
                                   kScale, Rounding::kNearest);
    lean = std::clamp(lean, Decimal::FromInt(-1), Decimal::FromInt(1));
    const Decimal reservation = mid - Bps(mid, mParams.skew_bps * lean);
    const Decimal half = Bps(mid, mParams.half_spread_bps);

    Decimal bid = (reservation - half).RoundToIncrement(tick, Rounding::kDown);
    Decimal ask = (reservation + half).RoundToIncrement(tick, Rounding::kUp);
    bid = std::min(bid, *quote.ask - tick);  // never cross the book
    ask = std::max(ask, *quote.bid + tick);

    // Room to add to the position, and to sell down. Spot cannot go short.
    const Decimal bid_room = mStart + mParams.max_inventory - mInventory;
    const Decimal ask_room =
        mInventory - std::max(Decimal(), mStart - mParams.max_inventory);

    const Decimal threshold =
        std::max(Bps(mid, mParams.requote_bps), tick);
    Manage(context, mBid, Side::kBuy,
           bid.Sign() > 0 ? std::optional(bid) : std::nullopt, Size(bid_room),
           bid_room, threshold);
    Manage(context, mAsk, Side::kSell, ask, Size(ask_room), ask_room,
           threshold);
}

bool MarketMaker::Pull(Context& context, Resting& quote) {
    if (quote.order_id.empty()) {
        return true;
    }
    if (quote.cancel_raced) {
        return false;  // waiting for the venue to settle it
    }
    auto outcome = context.Venue().CancelOrder(quote.order_id);
    if (outcome && *outcome == trading::CancelOutcome::kAlreadyFilled) {
        // The fill is on its way (OnFill); until the order's state arrives the
        // inventory is not trustworthy.
        quote.cancel_raced = true;
        return false;
    }
    if (!outcome) {
        spdlog::warn("market_maker: cancel of {} failed: {}", quote.order_id,
                     outcome.error().ToString());
        if (outcome.error().category != types::ErrorCategory::kNotFound) {
            return false;  // still resting as far as we know; try again
        }
    }
    quote = Resting();
    return true;
}

void MarketMaker::Manage(Context& context, Resting& quote, Side side,
                         const std::optional<Decimal>& price,
                         const Decimal& size, const Decimal& room,
                         const Decimal& threshold) {
    const bool wanted = price && size.Sign() > 0;
    if (!quote.order_id.empty()) {
        const bool fresh =
            mParams.max_quote_age == std::chrono::milliseconds::zero() ||
            context.Now() - quote.placed < mParams.max_quote_age;
        const bool keep = wanted && !quote.cancel_raced && fresh &&
                          quote.Remaining() <= room &&
                          (*price - quote.price).Abs() < threshold;
        if (keep || !Pull(context, quote)) {
            return;
        }
    }
    if (!wanted) {
        return;
    }

    types::OrderRequest request;
    request.symbol = mParams.symbol;
    request.side = side;
    request.type = types::OrderType::kLimit;
    request.quantity = size;
    request.limit_price = *price;
    auto ack = context.Venue().PlaceOrder(request);
    if (!ack) {
        spdlog::warn("market_maker: {} {} @ {} failed: {}",
                     types::ToString(side), size.ToString(), price->ToString(),
                     ack.error().ToString());
        return;
    }
    if (!types::IsActive(ack->state)) {
        return;  // filled or refused at once; OnFill has any fill
    }
    quote = Resting{ack->order_id, *price, size, Decimal(), context.Now(), false};
    spdlog::debug("market_maker: {} {} @ {} (order {})", types::ToString(side),
                  size.ToString(), price->ToString(), ack->order_id);
}

void MarketMaker::OnFill(Context&, const types::Fill& fill) {
    if (fill.symbol != mParams.symbol) {
        return;
    }
    mInventory += fill.side == Side::kBuy ? fill.quantity : -fill.quantity;
    for (Resting* quote : {&mBid, &mAsk}) {
        if (!quote->order_id.empty() && quote->order_id == fill.order_id) {
            quote->filled += fill.quantity;
        }
    }
    spdlog::info("market_maker: {} {} @ {}, inventory {}",
                 types::ToString(fill.side), fill.quantity.ToString(),
                 fill.price.ToString(), mInventory.ToString());
}

void MarketMaker::OnOrderUpdate(Context&, const types::OrderStatus& status) {
    if (types::IsActive(status.state)) {
        return;
    }
    for (Resting* quote : {&mBid, &mAsk}) {
        if (!quote->order_id.empty() && quote->order_id == status.order_id) {
            *quote = Resting();
        }
    }
}

void MarketMaker::OnStop(Context& context) {
    for (Resting* quote : {&mBid, &mAsk}) {
        if (!quote->order_id.empty() && !quote->cancel_raced) {
            (void)context.Venue().CancelOrder(quote->order_id);
        }
    }
}

}  // namespace kestrel::strategy
