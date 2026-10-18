#include "kestrel/trading/paper_trading_venue.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>

#include "kestrel/trading/order_rules.hpp"

namespace kestrel::trading {

using types::Decimal;
using types::ErrorCategory;
using types::Liquidity;
using types::MakeError;
using types::OrderState;
using types::OrderType;
using types::Result;
using types::Rounding;
using types::Side;

namespace {

constexpr int kBookScale =
    8;  // balances, fees and reserves are kept to 8 places

Decimal ToBookScale(const Decimal& d, Rounding mode) {
    return d.Rescale(kBookScale, mode);
}

// Price the order would trade at: the ask for buys, the bid for sells. Falls
// back to the last trade when that side of the book is missing.
std::optional<Decimal> ReferencePrice(const types::Quote& q, Side side) {
    if (side == Side::kBuy) {
        return q.ask ? q.ask : q.last;
    }
    return q.bid ? q.bid : q.last;
}

std::optional<Decimal> TouchSize(const types::Quote& q, Side side) {
    return side == Side::kBuy ? q.ask_size : q.bid_size;
}

}  // namespace

struct PaperTradingVenue::Order {
    types::OrderStatus status;
    types::SymbolInfo info;
    std::string base_asset;
    std::string quote_asset;
    std::string reserve_asset;
    Decimal reserve_unit;     // reserved amount per unit of remaining quantity
    Decimal notional_filled;  // sum of price*quantity over fills
    bool triggered = false;   // stop orders only
    std::optional<types::Timestamp> expires;

    Decimal Remaining() const {
        return status.quantity - status.filled_quantity;
    }
};

struct PaperTradingVenue::Candidate {
    Decimal price;
    Decimal quantity;
    Liquidity liquidity = Liquidity::kTaker;
    bool triggers_stop = false;
};

struct PaperTradingVenue::PositionState {
    Decimal quantity;
    Decimal average_price;
    Decimal realized_pnl;
};

PaperTradingVenue::PaperTradingVenue(
    std::shared_ptr<data::DataSource> data_source, PaperOptions options,
    std::function<types::Timestamp()> clock)
    : mDataSource(std::move(data_source)),
      mOptions(std::move(options)),
      mClock(std::move(clock)) {
    for (const auto& [asset, amount] : mOptions.initial_balances) {
        mTotal[asset] = ToBookScale(amount, Rounding::kNearest);
    }
}

PaperTradingVenue::~PaperTradingVenue() { Stop(); }

Result<void> PaperTradingVenue::Start(const std::vector<std::string>& symbols) {
    if (!mDataSource) {
        return MakeError(ErrorCategory::kInvalidArgument,
                         "paper venue has no data source");
    }
    auto sub = mDataSource->StreamQuotes(
        symbols, [this](const types::Quote& q) { OnQuote(q); });
    if (!sub) {
        return std::unexpected(sub.error());
    }
    mSubscription = std::move(*sub);
    return {};
}

void PaperTradingVenue::Stop() {
    // Stopping joins the stream thread, so it must happen without holding
    // mMutex (OnQuote takes it).
    if (mSubscription) {
        mSubscription->Stop();
        mSubscription.reset();
    }
}

void PaperTradingVenue::Adjust(std::map<std::string, Decimal>& book,
                               const std::string& asset, const Decimal& delta) {
    auto& slot = book[asset];  // default-constructed Decimal is zero
    slot = ToBookScale(slot + delta, Rounding::kNearest);
}

Decimal PaperTradingVenue::Reserve(const Order& order,
                                   const Decimal& remaining) const {
    return ToBookScale(order.reserve_unit * remaining, Rounding::kUp);
}

void PaperTradingVenue::Close(Order& order, OrderState state,
                              std::string reason) {
    Adjust(mLocked, order.reserve_asset, -Reserve(order, order.Remaining()));
    order.status.state = state;
    order.status.updated_time = mClock();
    if (!reason.empty()) {
        order.status.reason = std::move(reason);
    }
}

std::optional<PaperTradingVenue::Candidate> PaperTradingVenue::Evaluate(
    const Order& order, const types::Quote& quote, bool placing) const {
    const Side side = order.status.side;
    const auto ref = ReferencePrice(quote, side);
    if (!ref || ref->Sign() <= 0) {
        return std::nullopt;
    }

    Candidate c;
    c.quantity = order.Remaining();
    const bool market_like =
        order.status.type == OrderType::kMarket ||
        (order.status.type == OrderType::kStop && (order.triggered || [&] {
             const Decimal& stop = *order.status.stop_price;
             return side == Side::kBuy ? *ref >= stop : *ref <= stop;
         }()));
    if (order.status.type == OrderType::kStop && !order.triggered &&
        market_like) {
        c.triggers_stop = true;
    }

    if (market_like) {
        const Decimal one(1, 0);
        c.liquidity = Liquidity::kTaker;
        if (side == Side::kBuy) {
            c.price =
                (*ref * (one + mOptions.slippage))
                    .RoundToIncrement(order.info.tick_size, Rounding::kUp);
        } else {
            c.price =
                (*ref * (one - mOptions.slippage))
                    .RoundToIncrement(order.info.tick_size, Rounding::kDown);
            if (c.price.Sign() <= 0) {
                return std::nullopt;
            }
        }
        return c;
    }

    if (order.status.type == OrderType::kLimit) {
        const Decimal& limit = *order.status.limit_price;
        const bool crosses = side == Side::kBuy ? *ref <= limit : *ref >= limit;
        if (!crosses) {
            return std::nullopt;
        }
        if (placing) {
            c.price = *ref;  // marketable on arrival: take the touch
            c.liquidity = Liquidity::kTaker;
        } else {
            c.price = limit;  // resting order gets its own price
            c.liquidity = Liquidity::kMaker;
        }
        if (mOptions.cap_limit_fills_at_quote_size) {
            if (const auto size = TouchSize(quote, side)) {
                if (size->Sign() <= 0) {
                    return std::nullopt;
                }
                c.quantity = std::min(c.quantity, *size);
            }
        }
        return c;
    }
    return std::nullopt;  // untriggered stop
}

bool PaperTradingVenue::Execute(Order& order, const Candidate& c,
                                Liquidity liquidity) {
    const Side side = order.status.side;
    const Decimal& rate = liquidity == Liquidity::kMaker
                              ? mOptions.maker_fee_rate
                              : mOptions.taker_fee_rate;
    const Decimal notional = c.price * c.quantity;
    const Decimal fee = ToBookScale(notional * rate, Rounding::kUp);

    if (side == Side::kBuy) {
        const Decimal cost = ToBookScale(notional, Rounding::kUp) + fee;
        // Funds already set aside for this order count towards what it may
        // spend.
        const Decimal spendable = mTotal[order.quote_asset] -
                                  mLocked[order.quote_asset] +
                                  Reserve(order, order.Remaining());
        if (cost > spendable) {
            Close(order, OrderState::kCancelled,
                  "insufficient funds at fill time");
            return false;
        }
    }

    // Release the reserve for the quantity being filled (telescoping, so the
    // lock is exactly zero once the order is done).
    const Decimal before = Reserve(order, order.Remaining());
    order.status.filled_quantity += c.quantity;
    const Decimal after = Reserve(order, order.Remaining());
    Adjust(mLocked, order.reserve_asset, after - before);

    PositionState& pos = mPositions[order.status.symbol];
    if (side == Side::kBuy) {
        Adjust(mTotal, order.quote_asset,
               -(ToBookScale(notional, Rounding::kUp) + fee));
        Adjust(mTotal, order.base_asset, c.quantity);
        const Decimal new_qty = pos.quantity + c.quantity;
        pos.average_price =
            Decimal::Divide(pos.average_price * pos.quantity + notional,
                            new_qty, kBookScale, Rounding::kNearest);
        pos.quantity = new_qty;
        pos.realized_pnl =
            ToBookScale(pos.realized_pnl - fee, Rounding::kNearest);
    } else {
        Adjust(mTotal, order.base_asset, -c.quantity);
        Adjust(mTotal, order.quote_asset,
               ToBookScale(notional, Rounding::kDown) - fee);
        // Only quantity bought through this venue has a known cost basis.
        const Decimal tracked = std::min(c.quantity, pos.quantity);
        pos.realized_pnl = ToBookScale(
            pos.realized_pnl + (c.price - pos.average_price) * tracked - fee,
            Rounding::kNearest);
        pos.quantity -= tracked;
        if (pos.quantity.IsZero()) {
            pos.average_price = Decimal();
        }
    }

    order.notional_filled += notional;
    order.status.average_fill_price =
        Decimal::Divide(order.notional_filled, order.status.filled_quantity,
                        kBookScale, Rounding::kNearest);
    order.status.updated_time = mClock();
    order.status.state = order.Remaining().IsZero()
                             ? OrderState::kFilled
                             : OrderState::kPartiallyFilled;

    types::Fill fill;
    char id[32];
    std::snprintf(id, sizeof id, "PAPER-F%06llu",
                  static_cast<unsigned long long>(mNextFill++));
    fill.fill_id = id;
    fill.order_id = order.status.order_id;
    fill.client_order_id = order.status.client_order_id;
    fill.symbol = order.status.symbol;
    fill.side = side;
    fill.quantity = c.quantity;
    fill.price = c.price;
    fill.fee = fee;
    fill.fee_asset = order.quote_asset;
    fill.liquidity = liquidity;
    fill.time = order.status.updated_time;
    mFills.push_back(std::move(fill));
    return true;
}

void PaperTradingVenue::TryMatch(Order& order, const types::Quote& quote,
                                 bool placing) {
    if (!types::IsActive(order.status.state)) {
        return;
    }
    const auto candidate = Evaluate(order, quote, placing);
    if (!candidate) {
        return;
    }
    if (candidate->triggers_stop) {
        order.triggered = true;
    }
    Execute(order, *candidate, candidate->liquidity);
}

void PaperTradingVenue::ExpireDayOrders() {
    const auto now = mClock();
    for (auto& [id, order] : mOrders) {
        if (types::IsActive(order.status.state) && order.expires &&
            now >= *order.expires) {
            Close(order, OrderState::kExpired,
                  "day order expired at end of UTC day");
        }
    }
}

void PaperTradingVenue::OnQuote(const types::Quote& quote) {
    std::lock_guard lock(mMutex);
    mQuotes[quote.symbol].MergeFrom(quote);
    ExpireDayOrders();
    const types::Quote& latest = mQuotes[quote.symbol];
    for (auto& [id, order] : mOrders) {
        if (order.status.symbol == quote.symbol) {
            TryMatch(order, latest, /*placing=*/false);
        }
    }
}

Result<types::OrderAcknowledgement> PaperTradingVenue::PlaceOrder(
    const types::OrderRequest& raw) {
    auto symbol = types::NormalizeSymbol(raw.symbol);
    if (!symbol) {
        return std::unexpected(symbol.error());
    }
    if (!mDataSource) {
        return MakeError(ErrorCategory::kInvalidArgument,
                         "paper venue has no data source");
    }

    // Network access (cached after the first call) happens before taking the
    // lock.
    auto info = mDataSource->GetSymbolInfo(*symbol);
    if (!info) {
        return std::unexpected(info.error());
    }
    types::OrderRequest request = raw;
    request.symbol = *symbol;
    auto rules = ApplySymbolRules(request, *info);
    if (!rules) {
        return std::unexpected(rules.error());
    }
    request = *rules;

    if (request.type == OrderType::kStop &&
        request.time_in_force != types::TimeInForce::kGtc &&
        request.time_in_force != types::TimeInForce::kDay) {
        return MakeError(ErrorCategory::kInvalidArgument,
                         "stop orders support only GTC and DAY");
    }
    const auto parts = types::SplitSymbol(*symbol);
    if (!parts) {
        return std::unexpected(parts.error());
    }

    std::lock_guard lock(mMutex);
    ExpireDayOrders();
    const auto now = mClock();

    Order order;
    order.info = *info;
    order.base_asset = parts->base;
    order.quote_asset = parts->quote.value_or(mOptions.default_quote_asset);
    order.status.symbol = *symbol;
    order.status.side = request.side;
    order.status.type = request.type;
    order.status.time_in_force = request.time_in_force;
    order.status.quantity = request.quantity;
    order.status.limit_price = request.limit_price;
    order.status.stop_price = request.stop_price;
    order.status.client_order_id = request.client_order_id;
    order.status.created_time = order.status.updated_time = now;
    if (request.time_in_force == types::TimeInForce::kDay) {
        order.expires =
            std::chrono::floor<std::chrono::days>(now) + std::chrono::days(1);
    }

    const auto quote_it = mQuotes.find(*symbol);
    const types::Quote* quote =
        quote_it == mQuotes.end() ? nullptr : &quote_it->second;
    const Decimal one(1, 0);

    // How much to set aside per unit: quote asset for buys (price plus the
    // worst-case fee), base asset for sells.
    if (request.side == Side::kSell) {
        order.reserve_asset = order.base_asset;
        order.reserve_unit = one;
    } else {
        order.reserve_asset = order.quote_asset;
        Decimal ref_price;
        if (request.type == OrderType::kLimit) {
            ref_price = *request.limit_price;
        } else if (request.type == OrderType::kStop) {
            ref_price = *request.stop_price * (one + mOptions.slippage);
        } else {
            const auto ref =
                quote ? ReferencePrice(*quote, request.side) : std::nullopt;
            if (!ref) {
                return MakeError(ErrorCategory::kNotFound,
                                 "no quote available yet for " + *symbol);
            }
            ref_price = *ref * (one + mOptions.slippage);
        }
        const Decimal fee_rate =
            request.type == OrderType::kLimit
                ? std::max(mOptions.maker_fee_rate, mOptions.taker_fee_rate)
                : mOptions.taker_fee_rate;
        order.reserve_unit = ref_price * (one + fee_rate);
    }
    if (request.type != OrderType::kLimit && request.type != OrderType::kStop &&
        !quote) {
        return MakeError(ErrorCategory::kNotFound,
                         "no quote available yet for " + *symbol);
    }

    const Decimal needed = Reserve(order, order.Remaining());
    const Decimal available =
        mTotal[order.reserve_asset] - mLocked[order.reserve_asset];
    if (needed > available) {
        return MakeError(ErrorCategory::kInsufficientFunds,
                         "need " + needed.ToString() + " " +
                             order.reserve_asset + " but only " +
                             available.ToString() + " is available");
    }

    char id[32];
    std::snprintf(id, sizeof id, "PAPER-O%06llu",
                  static_cast<unsigned long long>(mNextOrder++));
    order.status.order_id = id;

    // Fill-or-kill: all or nothing, decided before anything is booked.
    if (request.time_in_force == types::TimeInForce::kFok) {
        const auto c =
            quote ? Evaluate(order, *quote, /*placing=*/true) : std::nullopt;
        if (!c || c->quantity < order.Remaining()) {
            order.status.state = OrderState::kExpired;
            order.status.reason =
                "fill-or-kill order could not be filled in full";
            const auto [it, _] =
                mOrders.emplace(order.status.order_id, std::move(order));
            return types::OrderAcknowledgement{
                it->first, it->second.status.client_order_id,
                it->second.status.state, now};
        }
    }

    Adjust(mLocked, order.reserve_asset, needed);
    const auto [it, _] =
        mOrders.emplace(order.status.order_id, std::move(order));
    Order& placed = it->second;
    if (quote) {
        TryMatch(placed, *quote, /*placing=*/true);
    }
    if (types::IsActive(placed.status.state) &&
        request.time_in_force == types::TimeInForce::kIoc) {
        Close(placed, OrderState::kExpired,
              "immediate-or-cancel remainder cancelled");
    }
    spdlog::debug("paper: order {} {} {} {} -> {}", placed.status.order_id,
                  types::ToString(request.side),
                  placed.status.quantity.ToString(), *symbol,
                  types::ToString(placed.status.state));
    return types::OrderAcknowledgement{placed.status.order_id,
                                       placed.status.client_order_id,
                                       placed.status.state, now};
}

Result<CancelOutcome> PaperTradingVenue::CancelOrder(
    const std::string& order_id) {
    std::lock_guard lock(mMutex);
    ExpireDayOrders();
    const auto it = mOrders.find(order_id);
    if (it == mOrders.end()) {
        return MakeError(ErrorCategory::kNotFound, "unknown order " + order_id);
    }
    Order& order = it->second;
    if (order.status.state == OrderState::kFilled) {
        return CancelOutcome::kAlreadyFilled;
    }
    if (!types::IsActive(order.status.state)) {
        return CancelOutcome::kAlreadyClosed;
    }
    Close(order, OrderState::kCancelled, "cancelled by user");
    return CancelOutcome::kCancelled;
}

Result<std::vector<std::string>> PaperTradingVenue::CancelAll() {
    std::lock_guard lock(mMutex);
    ExpireDayOrders();
    std::vector<std::string> cancelled;
    for (auto& [id, order] : mOrders) {
        if (!types::IsActive(order.status.state)) {
            continue;
        }
        Close(order, OrderState::kCancelled, "cancelled by user");
        cancelled.push_back(id);
    }
    return cancelled;
}

Result<std::vector<types::OrderStatus>> PaperTradingVenue::GetOpenOrders() {
    std::lock_guard lock(mMutex);
    ExpireDayOrders();
    std::vector<types::OrderStatus> out;
    for (const auto& [id, order] : mOrders) {
        if (types::IsActive(order.status.state)) {
            out.push_back(order.status);
        }
    }
    return out;
}

Result<std::vector<types::Fill>> PaperTradingVenue::GetFills(
    types::Timestamp since) {
    std::lock_guard lock(mMutex);
    std::vector<types::Fill> out;
    std::ranges::copy_if(mFills, std::back_inserter(out),
                         [&](const types::Fill& f) { return f.time >= since; });
    return out;
}

Result<std::vector<types::Position>> PaperTradingVenue::GetPositions() {
    std::lock_guard lock(mMutex);
    std::vector<types::Position> out;
    for (const auto& [symbol, pos] : mPositions) {
        if (pos.quantity.Sign() > 0) {
            out.push_back(
                {symbol, pos.quantity, pos.average_price, pos.realized_pnl});
        }
    }
    return out;
}

Result<std::vector<types::Balance>> PaperTradingVenue::GetBalances() {
    std::lock_guard lock(mMutex);
    std::vector<types::Balance> out;
    for (const auto& [asset, total] : mTotal) {
        const Decimal locked =
            mLocked.contains(asset) ? mLocked.at(asset) : Decimal();
        if (total.IsZero() && locked.IsZero()) {
            continue;
        }
        out.push_back({asset, total, total - locked, locked});
    }
    return out;
}

}  // namespace kestrel::trading
