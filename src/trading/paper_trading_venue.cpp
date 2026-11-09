#include "kestrel/trading/paper_trading_venue.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>
#include <utility>

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

// Whether a trade whose taker was `taker` took liquidity from resting orders on
// `side`: a seller hitting bids, a buyer lifting asks. A trade with no side
// might have.
bool TakesFrom(const std::optional<Side>& taker, Side side) {
    return !taker || *taker != side;
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
    // LimitFillModel::kConservative, resting limits only: size queued ahead of
    // this order at its price, or unknown.
    std::optional<Decimal> queue_ahead;
    // order_latency: whether the order has reached the book, and whether a
    // cancel is on its way to it.
    bool arrived = true;
    bool cancelling = false;
    bool post_only = false;
    // The other order of a one-cancels-other pair, if any.
    std::string linked;
    // SetDeferMatching: placed while set, the order enters the book (is
    // checked and matched as it would be on placement) at the next quote.
    bool deferred = false;

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

PaperTradingVenue::PaperTradingVenue(
    std::shared_ptr<data::DataSource> data_source, PaperOptions options,
    std::function<types::Timestamp()> clock)
    : mDataSource(std::move(data_source)),
      mHours(mDataSource ? mDataSource->Hours()
                         : types::MarketHours::AlwaysOpen()),
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

class PaperTradingVenue::ExecutionSubscription final
    : public data::Subscription {
   public:
    ExecutionSubscription(std::weak_ptr<Listeners> listeners, std::uint64_t id)
        : mListeners(std::move(listeners)), mId(id) {}
    ~ExecutionSubscription() override { Stop(); }
    void Stop() override {
        if (auto listeners = mListeners.lock()) {
            std::lock_guard lock(listeners->mutex);
            if (listeners->callbacks.erase(mId) > 0) {
                --listeners->count;
            }
        }
    }

   private:
    std::weak_ptr<Listeners> mListeners;
    std::uint64_t mId;
};

Result<std::unique_ptr<data::Subscription>> PaperTradingVenue::StreamExecutions(
    ExecutionCallback callback) {
    std::lock_guard lock(mListeners->mutex);
    const std::uint64_t id = mListeners->next++;
    mListeners->callbacks.emplace(id, std::move(callback));
    ++mListeners->count;
    return std::unique_ptr<data::Subscription>(
        std::make_unique<ExecutionSubscription>(mListeners, id));
}

void PaperTradingVenue::Notice(const Order& order,
                               const std::optional<types::Fill>& fill) {
    if (mListeners->count == 0) {
        return;  // nobody to tell; do not let the queue grow
    }
    mNotices.emplace_back(mClock() + mOptions.report_latency,
                          ExecutionUpdate{fill, order.status});
}

void PaperTradingVenue::Deliver() {
    if (mListeners->count == 0) {
        return;
    }
    std::lock_guard deliver(mListeners->mutex);
    std::vector<ExecutionUpdate> batch;
    {
        std::lock_guard lock(mMutex);
        const auto now = mClock();
        while (!mNotices.empty() && mNotices.front().first <= now) {
            batch.push_back(std::move(mNotices.front().second));
            mNotices.pop_front();
        }
    }
    for (const ExecutionUpdate& update : batch) {
        for (const auto& [id, callback] : mListeners->callbacks) {
            callback(update);
        }
    }
}

void PaperTradingVenue::Close(Order& order, OrderState state,
                              std::string reason) {
    Adjust(mLocked, order.reserve_asset, -Reserve(order, order.Remaining()));
    order.status.state = state;
    order.status.updated_time = mClock();
    if (!reason.empty()) {
        order.status.reason = std::move(reason);
    }
    Notice(order, std::nullopt);
    // Its one-cancels-other partner goes with it: cancelled or rejected
    // alike, otherwise expired.
    CloseLinked(
        order, state == OrderState::kCancelled || state == OrderState::kRejected
                   ? state
                   : OrderState::kExpired);
}

void PaperTradingVenue::CloseLinked(Order& order, OrderState state) {
    if (order.linked.empty()) {
        return;
    }
    // Unlinked first, so the partner's close does not come back to this one
    // (a part-filled limit stays open when its stop closes).
    const auto it = mOrders.find(std::exchange(order.linked, {}));
    if (it == mOrders.end()) {
        return;
    }
    it->second.linked.clear();
    if (types::IsActive(it->second.status.state)) {
        Close(it->second, state,
              "one-cancels-other: " + order.status.order_id + " " +
                  std::string(types::ToString(order.status.state)));
    }
}

std::optional<PaperTradingVenue::Candidate> PaperTradingVenue::Evaluate(
    const Order& order, const types::Quote& quote, bool placing) const {
    if (!mHours->IsOpen(mClock())) {
        return std::nullopt;  // nothing trades while the market is closed
    }
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

void PaperTradingVenue::UpdateQueue(Order& order,
                                    const types::Quote& quote) const {
    const bool buy = order.status.side == Side::kBuy;
    const Decimal& limit = *order.status.limit_price;
    const auto& own = buy ? quote.bid : quote.ask;
    if (!own) {
        return;
    }
    if (buy ? *own < limit : *own > limit) {
        order.queue_ahead = Decimal();  // nobody else is at our price
    } else if (*own == limit && !order.queue_ahead) {
        order.queue_ahead = buy ? quote.bid_size : quote.ask_size;
    }
}

std::optional<PaperTradingVenue::Candidate> PaperTradingVenue::EvaluateQueued(
    Order& order, const types::Quote& quote,
    const std::optional<Trade>& trade) const {
    const bool buy = order.status.side == Side::kBuy;
    const Decimal& limit = *order.status.limit_price;
    Decimal fillable;

    if (trade) {
        if (buy ? trade->price < limit : trade->price > limit) {
            fillable = order.Remaining();
        } else if (trade->price == limit && trade->size && order.queue_ahead &&
                   TakesFrom(trade->taker_side, order.status.side)) {
            const Decimal used = std::min(*order.queue_ahead, *trade->size);
            *order.queue_ahead -= used;
            fillable = std::min(order.Remaining(), *trade->size - used);
        }
    }

    const auto& opposite = buy ? quote.ask : quote.bid;
    if (opposite && (buy ? *opposite < limit : *opposite > limit)) {
        Decimal crossed = order.Remaining();
        if (mOptions.cap_limit_fills_at_quote_size) {
            if (const auto size = TouchSize(quote, order.status.side)) {
                crossed = std::min(crossed, std::max(*size, Decimal()));
            }
        }
        fillable = std::max(fillable, crossed);
    }

    UpdateQueue(order, quote);
    if (fillable.Sign() <= 0) {
        return std::nullopt;
    }
    Candidate c;
    c.price = limit;
    c.quantity = fillable;
    c.liquidity = Liquidity::kMaker;
    return c;
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
        // Funds already set aside for this order (or held for it by its
        // one-cancels-other partner) count towards what it may spend.
        Decimal spendable = mTotal[order.quote_asset] -
                            mLocked[order.quote_asset] +
                            Reserve(order, order.Remaining());
        if (const auto partner = mOrders.find(order.linked);
            !order.linked.empty() && partner != mOrders.end() &&
            types::IsActive(partner->second.status.state)) {
            spendable += Reserve(partner->second, partner->second.Remaining());
        }
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

    if (side == Side::kBuy) {
        Adjust(mTotal, order.quote_asset,
               -(ToBookScale(notional, Rounding::kUp) + fee));
        Adjust(mTotal, order.base_asset, c.quantity);
    } else {
        Adjust(mTotal, order.base_asset, -c.quantity);
        Adjust(mTotal, order.quote_asset,
               ToBookScale(notional, Rounding::kDown) - fee);
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
    mPnl.Apply(fill);
    Notice(order, fill);
    // Any fill closes a partner. If the partner held the pair's reserve, this
    // order takes over what its remainder needs first.
    if (const auto partner = mOrders.find(order.linked);
        !order.linked.empty() && partner != mOrders.end() &&
        types::IsActive(order.status.state) && order.reserve_unit.IsZero() &&
        types::IsActive(partner->second.status.state)) {
        order.reserve_unit = partner->second.reserve_unit;
        Adjust(mLocked, order.reserve_asset, Reserve(order, order.Remaining()));
    }
    CloseLinked(order, OrderState::kExpired);
    mFills.push_back(std::move(fill));
    if (mOptions.max_fills > 0 && mFills.size() > mOptions.max_fills) {
        mFills.pop_front();
    }
    return true;
}

void PaperTradingVenue::TryMatch(Order& order, const types::Quote& quote,
                                 bool placing,
                                 const std::optional<Trade>& trade) {
    if (!types::IsActive(order.status.state)) {
        return;
    }
    const bool queued =
        !placing && order.status.type == OrderType::kLimit &&
        mOptions.limit_fill_model == LimitFillModel::kConservative;
    const auto candidate = queued ? EvaluateQueued(order, quote, trade)
                                  : Evaluate(order, quote, placing);
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
    std::erase_if(mActive, [&](Order* order) {
        if (types::IsActive(order->status.state) && order->expires &&
            now >= *order->expires) {
            Close(*order, OrderState::kExpired,
                  "day order expired at end of UTC day");
        }
        if (types::IsActive(order->status.state)) {
            return false;
        }
        if (mOptions.max_closed_orders > 0) {
            mClosed.push_back(order->status.order_id);
        }
        return true;
    });
    // Only closed orders, out of mActive, are forgotten; an order or cancel
    // still in flight for one finds it gone and skips it, as for a closed
    // one.
    while (mOptions.max_closed_orders > 0 &&
           mClosed.size() > mOptions.max_closed_orders) {
        mOrders.erase(mClosed.front());
        mClosed.pop_front();
    }
}

void PaperTradingVenue::ProcessPending() {
    const auto now = mClock();
    while (!mInFlight.empty() && mInFlight.front().due <= now) {
        const InFlight action = std::move(mInFlight.front());
        mInFlight.pop_front();
        const auto it = mOrders.find(action.order_id);
        if (it == mOrders.end() || !types::IsActive(it->second.status.state)) {
            continue;  // filled (or closed) before this reached it
        }
        if (action.cancel) {
            Close(it->second, OrderState::kCancelled, "cancelled by user");
        } else {
            Arrive(it->second);
        }
    }
}

void PaperTradingVenue::Arrive(Order& order) {
    order.arrived = true;
    if (order.deferred) {
        return;  // checked and entered at the next quote, in OnQuote
    }
    const auto quote_it = mQuotes.find(order.status.symbol);
    const types::Quote* quote =
        quote_it == mQuotes.end() ? nullptr : &quote_it->second;
    if (WouldTake(order, quote)) {
        Close(order, OrderState::kRejected,
              "post-only order would take liquidity at " +
                  order.status.limit_price->ToString());
        return;
    }
    if (order.status.time_in_force == types::TimeInForce::kFok) {
        const auto c =
            quote ? Evaluate(order, *quote, /*placing=*/true) : std::nullopt;
        if (!c || c->quantity < order.Remaining()) {
            Close(order, OrderState::kExpired,
                  "fill-or-kill order could not be filled in full");
            return;
        }
    }
    Enter(order);
}

void PaperTradingVenue::Enter(Order& order) {
    const auto quote_it = mQuotes.find(order.status.symbol);
    if (order.deferred) {
        return;  // OnQuote enters it at the next quote
    }
    if (quote_it != mQuotes.end()) {
        TryMatch(order, quote_it->second, /*placing=*/true);
        if (types::IsActive(order.status.state) &&
            order.status.type == OrderType::kLimit &&
            mOptions.limit_fill_model == LimitFillModel::kConservative) {
            UpdateQueue(order,
                        quote_it->second);  // joins the back of the queue
        }
    }
    if (types::IsActive(order.status.state) &&
        order.status.time_in_force == types::TimeInForce::kIoc) {
        Close(order, OrderState::kExpired,
              "immediate-or-cancel remainder cancelled");
    }
}

void PaperTradingVenue::OnQuote(const types::Quote& quote) {
    DeliverOnExit deliver{*this};
    std::lock_guard lock(mMutex);
    // What arrived before this quote met the book as it was.
    ProcessPending();
    const bool seen_before = mQuotes.contains(quote.symbol);
    types::Quote& latest = mQuotes[quote.symbol];
    // Only the trade fields are compared, so keep those, not a copy of the
    // whole quote (its strings would be allocated for every update).
    const auto before_last = latest.last;
    const auto before_last_size = latest.last_size;
    const auto before_taker_side = latest.last_taker_side;
    const auto before_time = latest.exchange_time;
    latest.MergeFrom(quote);

    // Streams repeat the last trade on every book update, so a trade is new
    // only if its price, size, side or time changed. The first quote for a
    // symbol has nothing to compare with.
    std::optional<Trade> trade;
    if (seen_before && latest.last &&
        (before_last != latest.last || before_last_size != latest.last_size ||
         before_taker_side != latest.last_taker_side ||
         before_time != latest.exchange_time)) {
        trade = Trade{*latest.last, latest.last_size, latest.last_taker_side};
    }

    ExpireDayOrders();
    for (Order* order : mActive) {
        if (order->status.symbol != quote.symbol || !order->arrived ||
            !types::IsActive(order->status.state)) {
            continue;
        }
        if (order->deferred) {
            // Its first quote: checked and matched as on placement, so a
            // marketable limit takes the touch, and a post-only order that
            // would take is refused.
            order->deferred = false;
            if (WouldTake(*order, &latest)) {
                Close(*order, OrderState::kRejected,
                      "post-only order would take liquidity at " +
                          order->status.limit_price->ToString());
                continue;
            }
            Enter(*order);
            continue;
        }
        TryMatch(*order, latest, /*placing=*/false, trade);
    }
}

void PaperTradingVenue::SetDeferMatching(bool defer) {
    std::lock_guard lock(mMutex);
    mDeferMatching = defer;
}

std::vector<Decimal> PaperTradingVenue::RestingPrices(
    const types::Symbol& symbol) const {
    std::lock_guard lock(mMutex);
    std::vector<Decimal> prices;
    for (const Order* order : mActive) {
        if (order->status.symbol != symbol || !order->arrived ||
            !types::IsActive(order->status.state)) {
            continue;
        }
        if (order->status.type == OrderType::kLimit) {
            prices.push_back(*order->status.limit_price);
        } else if (order->status.type == OrderType::kStop &&
                   !order->triggered) {
            prices.push_back(*order->status.stop_price);
        }
    }
    return prices;
}

Result<PaperTradingVenue::Prepared> PaperTradingVenue::Prepare(
    const types::OrderRequest& raw) const {
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

    if (rules->type == OrderType::kStop &&
        rules->time_in_force != types::TimeInForce::kGtc &&
        rules->time_in_force != types::TimeInForce::kDay) {
        return MakeError(ErrorCategory::kInvalidArgument,
                         "stop orders support only GTC and DAY");
    }
    const auto parts = types::SplitSymbol(*symbol);
    if (!parts) {
        return std::unexpected(parts.error());
    }
    return Prepared{*rules, *info, parts->base,
                    parts->quote.value_or(mOptions.default_quote_asset)};
}

Result<PaperTradingVenue::Order> PaperTradingVenue::Build(
    const Prepared& prepared, const types::Quote* quote,
    types::Timestamp now) const {
    const types::OrderRequest& request = prepared.request;
    Order order;
    order.post_only = request.post_only;
    order.deferred = mDeferMatching &&
                     request.time_in_force != types::TimeInForce::kIoc &&
                     request.time_in_force != types::TimeInForce::kFok;
    order.info = prepared.info;
    order.base_asset = prepared.base_asset;
    order.quote_asset = prepared.quote_asset;
    order.status.symbol = request.symbol;
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
                return MakeError(
                    ErrorCategory::kNotFound,
                    "no quote available yet for " + request.symbol);
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
                         "no quote available yet for " + request.symbol);
    }
    return order;
}

bool PaperTradingVenue::WouldTake(const Order& order,
                                  const types::Quote* quote) const {
    const auto& limit = order.status.limit_price;
    if (!order.post_only || !limit || !quote) {
        return false;
    }
    return order.status.side == Side::kBuy
               ? quote->ask && *limit >= *quote->ask
               : quote->bid && *limit <= *quote->bid;
}

Result<void> PaperTradingVenue::CheckFunds(const std::string& asset,
                                           const Decimal& needed) {
    const Decimal available = mTotal[asset] - mLocked[asset];
    if (needed > available) {
        return MakeError(ErrorCategory::kInsufficientFunds,
                         "need " + needed.ToString() + " " + asset +
                             " but only " + available.ToString() +
                             " is available");
    }
    return {};
}

std::string PaperTradingVenue::NextOrderId() {
    char id[32];
    std::snprintf(id, sizeof id, "PAPER-O%06llu",
                  static_cast<unsigned long long>(mNextOrder++));
    return id;
}

PaperTradingVenue::Order& PaperTradingVenue::Book(Order order) {
    Adjust(mLocked, order.reserve_asset, Reserve(order, order.Remaining()));
    std::string id = order.status.order_id;
    const auto [it, _] = mOrders.emplace(std::move(id), std::move(order));
    Order& placed = it->second;
    mActive.push_back(&placed);
    if (mOptions.order_latency.count() > 0) {
        placed.arrived = false;
        mInFlight.push_back(
            {mClock() + mOptions.order_latency, placed.status.order_id});
    }
    return placed;
}

Result<types::OrderAcknowledgement> PaperTradingVenue::PlaceOrder(
    const types::OrderRequest& raw) {
    auto prepared = Prepare(raw);
    if (!prepared) {
        return std::unexpected(prepared.error());
    }

    DeliverOnExit deliver{*this};
    std::lock_guard lock(mMutex);
    ProcessPending();
    ExpireDayOrders();
    const auto now = mClock();
    const bool delayed = mOptions.order_latency.count() > 0;
    const auto quote_it = mQuotes.find(prepared->request.symbol);
    const types::Quote* quote =
        quote_it == mQuotes.end() ? nullptr : &quote_it->second;

    auto built = Build(*prepared, quote, now);
    if (!built) {
        return std::unexpected(built.error());
    }
    Order& order = *built;
    // Delayed, the post-only check is made against the book the order
    // arrives at (Arrive); deferred, against the next quote (OnQuote).
    if (!delayed && !order.deferred && WouldTake(order, quote)) {
        return MakeError(ErrorCategory::kWouldTake,
                         "post-only order would take liquidity at " +
                             order.status.limit_price->ToString());
    }
    if (auto funds =
            CheckFunds(order.reserve_asset, Reserve(order, order.Remaining()));
        !funds) {
        return std::unexpected(funds.error());
    }
    order.status.order_id = NextOrderId();

    // Fill-or-kill: all or nothing, decided before anything is booked (or,
    // delayed, when it arrives).
    if (!delayed && order.status.time_in_force == types::TimeInForce::kFok) {
        const auto c =
            quote ? Evaluate(order, *quote, /*placing=*/true) : std::nullopt;
        if (!c || c->quantity < order.Remaining()) {
            order.status.state = OrderState::kExpired;
            order.status.reason =
                "fill-or-kill order could not be filled in full";
            const auto [it, _] =
                mOrders.emplace(order.status.order_id, std::move(order));
            if (mOptions.max_closed_orders > 0) {
                mClosed.push_back(it->first);  // closed already
            }
            return types::OrderAcknowledgement{
                it->first, it->second.status.client_order_id,
                it->second.status.state, now};
        }
    }

    Order& placed = Book(std::move(order));
    if (!delayed) {
        Enter(placed);
    }
    spdlog::debug("paper: order {} {} {} {} -> {}", placed.status.order_id,
                  types::ToString(placed.status.side),
                  placed.status.quantity.ToString(), placed.status.symbol,
                  types::ToString(placed.status.state));
    return types::OrderAcknowledgement{placed.status.order_id,
                                       placed.status.client_order_id,
                                       placed.status.state, now};
}

Result<types::OcoAcknowledgement> PaperTradingVenue::PlaceOco(
    const types::OcoRequest& raw) {
    const bool sell = raw.side == Side::kSell;
    if (raw.limit_price.Sign() <= 0 || raw.stop_price.Sign() <= 0 ||
        (sell ? raw.limit_price <= raw.stop_price
              : raw.limit_price >= raw.stop_price)) {
        return MakeError(ErrorCategory::kInvalidArgument,
                         sell ? "a sell OCO's limit must be above its stop"
                              : "a buy OCO's limit must be below its stop");
    }
    types::OrderRequest limit_leg;
    limit_leg.symbol = raw.symbol;
    limit_leg.side = raw.side;
    limit_leg.type = OrderType::kLimit;
    limit_leg.quantity = raw.quantity;
    limit_leg.limit_price = raw.limit_price;
    limit_leg.post_only = true;
    types::OrderRequest stop_leg = limit_leg;
    stop_leg.type = OrderType::kStop;
    stop_leg.limit_price.reset();
    stop_leg.stop_price = raw.stop_price;
    stop_leg.post_only = false;
    auto limit_prepared = Prepare(limit_leg);
    if (!limit_prepared) {
        return std::unexpected(limit_prepared.error());
    }
    auto stop_prepared = Prepare(stop_leg);
    if (!stop_prepared) {
        return std::unexpected(stop_prepared.error());
    }

    DeliverOnExit deliver{*this};
    std::lock_guard lock(mMutex);
    ProcessPending();
    ExpireDayOrders();
    const auto now = mClock();
    const bool delayed = mOptions.order_latency.count() > 0;
    const auto quote_it = mQuotes.find(limit_prepared->request.symbol);
    const types::Quote* quote =
        quote_it == mQuotes.end() ? nullptr : &quote_it->second;
    auto limit = Build(*limit_prepared, quote, now);
    if (!limit) {
        return std::unexpected(limit.error());
    }
    auto stop = Build(*stop_prepared, quote, now);
    if (!stop) {
        return std::unexpected(stop.error());
    }
    if (!delayed && !limit->deferred && WouldTake(*limit, quote)) {
        return MakeError(ErrorCategory::kWouldTake,
                         "the OCO's limit would take liquidity at " +
                             limit->status.limit_price->ToString());
    }
    // One reserve for both, the larger: the quantity is only spent once.
    Order& holder = stop->reserve_unit > limit->reserve_unit ? *stop : *limit;
    Order& other = &holder == &*stop ? *limit : *stop;
    other.reserve_unit = Decimal();
    if (auto funds = CheckFunds(holder.reserve_asset,
                                Reserve(holder, holder.Remaining()));
        !funds) {
        return std::unexpected(funds.error());
    }
    limit->status.order_id = NextOrderId();
    stop->status.order_id = NextOrderId();
    limit->linked = stop->status.order_id;
    stop->linked = limit->status.order_id;

    Order& placed_limit = Book(std::move(*limit));
    Order& placed_stop = Book(std::move(*stop));
    if (!delayed) {
        Enter(placed_limit);
        if (types::IsActive(placed_stop.status.state)) {
            Enter(placed_stop);
        }
    }
    spdlog::debug(
        "paper: OCO {} / {} {} {} {} -> {} / {}", placed_limit.status.order_id,
        placed_stop.status.order_id, types::ToString(raw.side),
        placed_limit.status.quantity.ToString(), placed_limit.status.symbol,
        types::ToString(placed_limit.status.state),
        types::ToString(placed_stop.status.state));
    return types::OcoAcknowledgement{
        {placed_limit.status.order_id, placed_limit.status.client_order_id,
         placed_limit.status.state, now},
        {placed_stop.status.order_id, placed_stop.status.client_order_id,
         placed_stop.status.state, now}};
}

Result<CancelOutcome> PaperTradingVenue::CancelOrder(
    const std::string& order_id) {
    DeliverOnExit deliver{*this};
    std::lock_guard lock(mMutex);
    ProcessPending();
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
    Cancel(order);
    return CancelOutcome::kCancelled;
}

void PaperTradingVenue::Cancel(Order& order) {
    if (mOptions.order_latency.count() == 0) {
        Close(order, OrderState::kCancelled, "cancelled by user");
    } else if (!order.cancelling) {
        order.cancelling = true;
        mInFlight.push_back(
            {mClock() + mOptions.order_latency, order.status.order_id, true});
    }
}

Result<std::vector<std::string>> PaperTradingVenue::CancelAll() {
    DeliverOnExit deliver{*this};
    std::lock_guard lock(mMutex);
    ProcessPending();
    ExpireDayOrders();
    std::vector<std::string> cancelled;
    for (Order* order : mActive) {
        if (!types::IsActive(order->status.state)) {
            continue;
        }
        Cancel(*order);
        cancelled.push_back(order->status.order_id);
    }
    return cancelled;
}

Result<std::vector<types::OrderStatus>> PaperTradingVenue::GetOpenOrders() {
    DeliverOnExit deliver{*this};
    std::lock_guard lock(mMutex);
    ProcessPending();
    ExpireDayOrders();
    std::vector<types::OrderStatus> out;
    for (const Order* order : mActive) {
        // An order whose cancel was acknowledged is not listed, as a live
        // venue would not once its cancel returned, though it can still
        // fill until the cancel reaches the book.
        if (types::IsActive(order->status.state) && !order->cancelling) {
            out.push_back(order->status);
        }
    }
    return out;
}

Result<types::OrderStatus> PaperTradingVenue::GetOrder(
    const std::string& order_id) {
    DeliverOnExit deliver{*this};
    std::lock_guard lock(mMutex);
    ProcessPending();
    ExpireDayOrders();
    auto it = mOrders.find(order_id);
    if (it == mOrders.end()) {
        return MakeError(ErrorCategory::kNotFound, "unknown order " + order_id);
    }
    return it->second.status;
}

Result<std::vector<types::Fill>> PaperTradingVenue::GetFills(
    types::Timestamp since) {
    std::lock_guard lock(mMutex);
    // Fills are appended as they happen, so they are sorted by time.
    const auto first =
        std::ranges::lower_bound(mFills, since, {}, &types::Fill::time);
    return std::vector<types::Fill>(first, mFills.end());
}

Result<std::vector<types::Position>> PaperTradingVenue::GetPositions() {
    std::lock_guard lock(mMutex);
    std::vector<types::Position> out;
    for (auto& position : mPnl.Positions()) {
        // Flat positions are not reported, and their realized P&L goes with
        // them.
        if (position.quantity.Sign() > 0) {
            out.push_back(std::move(position));
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
