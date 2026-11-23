#include "kestrel/strategy/spot_book.hpp"

#include <algorithm>
#include <stdexcept>

#include "kestrel/strategy/orders.hpp"

namespace kestrel::strategy {

using types::Decimal;
using types::OrderState;
using types::Rounding;
using types::Side;

namespace {

bool IsClosed(OrderState state) {
    return state == OrderState::kFilled || ClosedUnfilled(state);
}

}  // namespace

SpotBook::SpotBook(const std::vector<types::Symbol>& symbols,
                   types::Timeframe timeframe, const std::string& default_quote)
    : mTimeframe(timeframe) {
    for (const auto& symbol : symbols) {
        auto parts = types::SplitSymbol(symbol);
        if (parts && !parts->quote && !default_quote.empty()) {
            parts->quote = default_quote;
        }
        if (!parts || !parts->quote) {
            throw std::invalid_argument("symbol " + symbol +
                                        " has no quote asset");
        }
        if (parts->base == *parts->quote) {
            // A ticker named like the cash ("USD", an ETF): its holdings
            // and the cash balance could not be told apart.
            throw std::invalid_argument("symbol " + symbol +
                                        " is named like its quote asset");
        }
        if (!mBase.emplace(symbol, parts->base).second) {
            throw std::invalid_argument("symbol " + symbol +
                                        " is listed twice");
        }
        if (mQuote.empty()) {
            mQuote = *parts->quote;
        } else if (*parts->quote != mQuote) {
            throw std::invalid_argument(
                "all symbols must share one quote asset (" + mQuote + ", " +
                *parts->quote + ")");
        }
    }
}

void SpotBook::AdoptHoldings(Context& context, bool adopt) {
    auto balances = context.Venue().GetBalances();
    if (!balances) {
        if (adopt) {
            context.Log().Warn(
                "cannot read the account's balances: {}; nothing adopted",
                balances.error().ToString());
        }
        return;
    }
    for (const auto& balance : *balances) {
        for (const auto& [symbol, base] : mBase) {
            if (balance.asset != base || balance.total.Sign() <= 0) {
                continue;
            }
            const std::string amount = balance.total.Normalized().ToString();
            if (!adopt) {
                context.Log().Warn(
                    "the account holds {} {}, which this run did not buy: it "
                    "is not counted, and not sold (adopt_holdings counts it)",
                    amount, base);
            } else if (balance.available.Sign() > 0) {
                // What is free to sell: a part locked in an order is not.
                mHeld[symbol] = balance.available;
                context.Log().Info(
                    "adopting {} {} the account holds: held from now on",
                    balance.available.Normalized().ToString(), base);
            }
        }
    }
}

std::vector<SpotBook::Closed> SpotBook::Reconcile(Context& context) {
    std::vector<Closed> closed;
    for (auto it = mOrders.begin(); it != mOrders.end();) {
        auto status = context.Venue().GetOrder(it->first);
        const bool gone = (status && IsClosed(status->state)) ||
                          (!status && status.error().category ==
                                          types::ErrorCategory::kNotFound);
        if (!gone) {
            ++it;
            continue;
        }
        context.Log().Warn("order {} had closed unnoticed", it->first);
        const types::Symbol sym = status && !status->symbol.empty()
                                      ? status->symbol
                                      : it->second.symbol;
        if (status) {
            CountFilled(it->first, it->second, sym, status->filled_quantity);
        }
        closed.push_back({it->second.sell, sym});
        it = mOrders.erase(it);
    }
    return closed;
}

void SpotBook::CountFilled(const std::string& order_id, const OrderInfo& info,
                           const types::Symbol& symbol,
                           const Decimal& filled_quantity) {
    if (filled_quantity <= info.filled || symbol.empty()) {
        return;
    }
    const Decimal delta = filled_quantity - info.filled;
    Decimal& held = mHeld[symbol];
    held = info.sell ? std::max(held - delta, Decimal()) : held + delta;
    mCounted[order_id] += delta;
}

bool SpotBook::SellOut() const {
    return std::ranges::any_of(
        mOrders, [](const auto& order) { return order.second.sell; });
}

Decimal SpotBook::Held(const types::Symbol& symbol) const {
    const auto it = mHeld.find(symbol);
    return it == mHeld.end() ? Decimal() : it->second;
}

Decimal SpotBook::Sellable(Context& context,
                           const types::Symbol& symbol) const {
    const Decimal held = Held(symbol);
    if (held.Sign() <= 0) {
        return Decimal();
    }
    auto info = context.SymbolInfo(symbol);
    if (!info) {
        return Decimal();
    }
    const Decimal quantity =
        held.RoundToIncrement(info->lot_size, Rounding::kDown);
    const auto price = LastClose(context, symbol);
    if (quantity < info->min_order_size ||
        (info->min_notional.Sign() > 0 && price &&
         quantity * *price < info->min_notional)) {
        return Decimal();  // dust: as good as not held
    }
    return quantity;
}

std::optional<Decimal> SpotBook::LastClose(Context& context,
                                           const types::Symbol& symbol) const {
    const auto bars = context.Bars(symbol, mTimeframe);
    if (bars.empty()) {
        return std::nullopt;
    }
    return bars.back().close;
}

std::optional<Decimal> SpotBook::Equity(Context& context) const {
    auto balances = context.Venue().GetBalances();
    if (!balances) {
        return std::nullopt;
    }
    Decimal equity;
    for (const auto& balance : *balances) {
        if (balance.asset == mQuote) {
            equity = balance.total;
        }
    }
    for (const auto& [symbol, held] : mHeld) {
        if (held.Sign() > 0) {
            const auto price = LastClose(context, symbol);
            if (!price) {
                return std::nullopt;
            }
            equity += held * *price;
        }
    }
    return equity;
}

std::optional<Decimal> SpotBook::FreeCash(Context& context) const {
    auto balances = context.Venue().GetBalances();
    if (!balances) {
        return std::nullopt;
    }
    Decimal cash;
    for (const auto& balance : *balances) {
        if (balance.asset == mQuote) {
            cash = balance.available;
        }
    }
    return cash;
}

std::optional<std::string> SpotBook::Place(Context& context,
                                           const types::Symbol& symbol,
                                           Side side, const Decimal& quantity) {
    types::OrderRequest request;
    request.symbol = symbol;
    request.side = side;
    request.type = types::OrderType::kMarket;
    request.quantity = quantity;
    const auto ack =
        Submit(context, request,
               std::string(types::ToString(side)) + " " + quantity.ToString());
    if (!ack) {
        return std::nullopt;
    }
    context.Log().Info("{} {} {} (order {}, {})", types::ToString(side),
                       quantity.ToString(), symbol, ack->order_id,
                       types::ToString(ack->state));
    // Tracked even when it filled at once (a market order on the paper venue
    // or Binance.US): its cash has moved but its fill is still on the way,
    // and until that is counted the holdings are out of date.
    if (!ClosedUnfilled(ack->state)) {
        mOrders[ack->order_id] =
            OrderInfo{side == Side::kSell, symbol, quantity, {}};
    }
    return ack->order_id;
}

void SpotBook::OnFill(const types::Fill& fill) {
    const auto base = mBase.find(fill.symbol);
    if (base == mBase.end()) {
        return;
    }
    if (const auto ord = mOrders.find(fill.order_id); ord != mOrders.end()) {
        ord->second.filled += fill.quantity;
        if (ord->second.filled >= ord->second.quantity) {
            mOrders.erase(ord);  // all of it filled and counted
        }
    }
    // A fill of an order Reconcile counted from its status: not again.
    Decimal quantity = fill.quantity;
    if (const auto counted = mCounted.find(fill.order_id);
        counted != mCounted.end()) {
        const Decimal skip = std::min(quantity, counted->second);
        quantity -= skip;
        counted->second -= skip;
        if (counted->second.Sign() <= 0) {
            mCounted.erase(counted);
        }
    }
    Decimal& held = mHeld[fill.symbol];
    if (fill.side == Side::kBuy) {
        held += quantity;
    } else {
        held -= quantity;
    }
    if (fill.fee_asset == base->second) {
        held -= fill.fee;
    }
    held = std::max(held, Decimal());
}

std::optional<SpotBook::Done> SpotBook::OnOrderUpdate(
    const types::OrderStatus& status) {
    if (!IsClosed(status.state)) {
        return std::nullopt;
    }
    const auto it = mOrders.find(status.order_id);
    if (it == mOrders.end()) {
        return std::nullopt;
    }
    Done done{it->second.sell, status.state == OrderState::kFilled,
              !status.symbol.empty() ? status.symbol : it->second.symbol};
    // Its fills may not have been seen yet (an update can come before them):
    // counted from the status, and not again when they come.
    CountFilled(it->first, it->second, done.symbol, status.filled_quantity);
    mOrders.erase(it);
    return done;
}

bool PeriodGate::Complete() const {
    if (mSeen.size() == mSymbols) {
        return true;
    }
    return !mActive.empty() &&
           std::ranges::includes(mSeen, mActive);  // both sorted sets
}

std::vector<types::Timestamp> PeriodGate::Add(const types::Symbol& symbol,
                                              types::Timestamp open_time) {
    std::vector<types::Timestamp> complete;
    if (!mPeriod || open_time > *mPeriod) {
        // A new period: the last one, if a bar of it never came, is complete
        // with what did, and the symbols missing from it are not waited for.
        if (mPeriod && !mDone && !mSeen.empty()) {
            complete.push_back(*mPeriod);
            mActive = mSeen;
        }
        mPeriod = open_time;
        mSeen.clear();
        mDone = false;
    } else if (open_time < *mPeriod) {
        return complete;  // a period already complete
    } else if (mDone) {
        // After the period completed without it: waited for from now on.
        mActive.insert(symbol);
        return complete;
    }
    mSeen.insert(symbol);
    if (Complete()) {
        mDone = true;
        mActive = mSeen;
        complete.push_back(*mPeriod);
    }
    return complete;
}

}  // namespace kestrel::strategy
