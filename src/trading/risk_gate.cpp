#include "kestrel/trading/risk_gate.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace kestrel::trading {

using types::Decimal;
using types::ErrorCategory;
using types::MakeError;
using types::Side;

namespace {

constexpr std::chrono::seconds kRateWindow{60};

std::unexpected<types::Error> Reject(const std::string& why) {
    return MakeError(ErrorCategory::kRejected, "risk: " + why);
}

Decimal Held(const std::vector<types::Position>& positions,
             const types::Symbol& symbol) {
    for (const auto& position : positions) {
        if (position.symbol == symbol) {
            return position.quantity;
        }
    }
    return Decimal();
}

std::chrono::sys_days DayOf(types::Timestamp t) {
    return std::chrono::floor<std::chrono::days>(t);
}

}  // namespace

RiskGatedVenue::RiskGatedVenue(std::shared_ptr<TradingVenue> venue,
                               RiskLimits limits,
                               std::function<types::Timestamp()> clock)
    : mVenue(std::move(venue)),
      mLimits(std::move(limits)),
      mClock(std::move(clock)),
      mPnl(mLimits.shorts) {}

void RiskGatedVenue::NoteReferencePrice(const types::Symbol& symbol,
                                        const Decimal& price) {
    std::lock_guard lock(mPriceMutex);
    mPrices[symbol] = ReferencePrice{price, mClock()};
}

types::Result<void> RiskGatedVenue::StartLossCount() {
    auto positions = mVenue->GetPositions();
    if (!positions) {
        return std::unexpected(positions.error());
    }
    const types::Timestamp now = mClock();
    mPnl.Reset(*positions);
    mDay = DayOf(now);
    mFillCursor = now;
    mSeenFills.clear();
    mConsecutiveLosses = 0;
    mTotalRealized = Decimal();
    mPeakEquity = Decimal();
    mCounting = true;
    return {};
}

types::Result<void> RiskGatedVenue::ResetDailyBaseline() {
    std::lock_guard lock(mMutex);
    return StartLossCount();
}

types::Result<std::vector<std::string>> RiskGatedVenue::Halt(
    const std::string& reason) {
    mHalted = true;
    spdlog::warn("risk: trading halted: {}", reason);
    auto cancelled = mVenue->CancelAll();

    // An order already inside PlaceOrder read the flag before it was set and
    // can still land after the cancel above. PlaceOrder holds the order lock
    // from its checks to the venue call, so once the lock is ours that order
    // has landed, and any later one sees the halt. Cancel again to catch it.
    { std::lock_guard lock(mMutex); }
    auto recancelled = mVenue->CancelAll();

    if (!recancelled) {
        return recancelled;
    }
    if (cancelled) {
        recancelled->insert(recancelled->begin(), cancelled->begin(),
                            cancelled->end());
    }
    return recancelled;
}

void RiskGatedVenue::NewDay(std::chrono::sys_days day) {
    mPnl.ResetRealized();
    mConsecutiveLosses = 0;
    mDay = day;
}

std::optional<Decimal> RiskGatedVenue::Unrealized(types::Timestamp now) const {
    Decimal total;
    for (const auto& position : mPnl.Positions()) {
        if (position.quantity.Sign() == 0) {
            continue;
        }
        const auto price = ReferencePriceFor(position.symbol, now);
        if (!price) {
            return std::nullopt;
        }
        total += position.quantity * (*price - position.average_price);
    }
    return total;
}

types::Result<void> RiskGatedVenue::CatchUpFills(types::Timestamp now) {
    if (!mCounting) {
        if (auto started = StartLossCount(); !started) {
            return std::unexpected(started.error());
        }
    }
    auto fills = mVenue->GetFills(mFillCursor);
    if (!fills) {
        return std::unexpected(fills.error());
    }
    std::stable_sort(fills->begin(), fills->end(),
                     [](const types::Fill& a, const types::Fill& b) {
                         return a.time < b.time;
                     });
    for (const types::Fill& fill : *fills) {
        if (!mSeenFills.emplace(fill.fill_id, fill.time).second) {
            continue;
        }
        // A fill from a new day starts that day's count before it is applied.
        if (DayOf(fill.time) != mDay) {
            NewDay(DayOf(fill.time));
        }
        const Decimal before = mPnl.Realized(fill.symbol);
        const Decimal held = mPnl.Held(fill.symbol);
        mPnl.Apply(fill);
        const Decimal gained = mPnl.Realized(fill.symbol) - before;
        mTotalRealized += gained;
        // With shorts, the fills that close a position: a sell of a long, a
        // buy of a short (a short sale realizes only its fee).
        const bool closing = fill.side == Side::kSell
                                 ? !mLimits.shorts || held.Sign() > 0
                                 : mLimits.shorts && held.IsNegative();
        if (closing) {
            mConsecutiveLosses = gained.Sign() < 0 ? mConsecutiveLosses + 1 : 0;
        }
        mFillCursor = std::max(mFillCursor, fill.time);
    }
    std::erase_if(mSeenFills, [this](const auto& seen) {
        return seen.second < mFillCursor;
    });
    if (DayOf(now) != mDay) {
        NewDay(DayOf(now));
    }
    return {};
}

types::Result<std::optional<std::string>> RiskGatedVenue::CheckLosses(
    types::Timestamp now) {
    if (auto caught = CatchUpFills(now); !caught) {
        return std::unexpected(caught.error());
    }

    if (mLimits.max_daily_loss) {
        const Decimal loss = -mPnl.Realized();
        if (loss >= *mLimits.max_daily_loss) {
            return "daily loss limit reached (lost " + loss.ToString() +
                   ", limit " + mLimits.max_daily_loss->ToString() + ")";
        }
    }
    if (mLimits.max_consecutive_losses &&
        mConsecutiveLosses >= *mLimits.max_consecutive_losses) {
        return std::to_string(mConsecutiveLosses) +
               " consecutive losing sells (limit " +
               std::to_string(*mLimits.max_consecutive_losses) + ")";
    }
    if (mLimits.max_drawdown) {
        // Fails closed: with something held that cannot be priced, the
        // drawdown is unknown.
        const auto unrealized = Unrealized(now);
        if (!unrealized) {
            return std::string(
                "drawdown unknown: a held symbol has no current price");
        }
        const Decimal equity = mTotalRealized + *unrealized;
        mPeakEquity = std::max(mPeakEquity, equity);
        const Decimal drawdown = mPeakEquity - equity;
        if (drawdown >= *mLimits.max_drawdown) {
            return "drawdown limit reached (down " + drawdown.ToString() +
                   " from the peak, limit " + mLimits.max_drawdown->ToString() +
                   ")";
        }
    }
    return std::nullopt;
}

std::optional<Decimal> RiskGatedVenue::ReferencePriceFor(
    const types::Symbol& symbol, types::Timestamp now) const {
    std::lock_guard lock(mPriceMutex);
    auto it = mPrices.find(symbol);
    if (it == mPrices.end()) {
        return std::nullopt;
    }
    if (mLimits.max_price_age &&
        now - it->second.time > *mLimits.max_price_age) {
        return std::nullopt;
    }
    return it->second.price;
}

std::optional<Decimal> RiskGatedVenue::PriceFor(
    const types::OrderRequest& request, types::Timestamp now) const {
    if (request.limit_price) {
        return request.limit_price;
    }
    if (request.stop_price) {
        return request.stop_price;
    }
    return ReferencePriceFor(request.symbol, now);
}

types::Result<types::OrderAcknowledgement> RiskGatedVenue::PlaceOrder(
    const types::OrderRequest& request) {
    std::lock_guard lock(mMutex);
    if (auto admitted = Admit(request, 1); !admitted) {
        return std::unexpected(admitted.error());
    }
    return mVenue->PlaceOrder(request);
}

types::Result<types::OcoAcknowledgement> RiskGatedVenue::PlaceOco(
    const types::OcoRequest& request) {
    std::lock_guard lock(mMutex);
    // Checked as its limit order, counting as two orders: both rest, though
    // only one can fill.
    types::OrderRequest limit;
    limit.symbol = request.symbol;
    limit.side = request.side;
    limit.type = types::OrderType::kLimit;
    limit.quantity = request.quantity;
    limit.limit_price = request.limit_price;
    if (auto admitted = Admit(limit, 2); !admitted) {
        return std::unexpected(admitted.error());
    }
    return mVenue->PlaceOco(request);
}

types::Result<void> RiskGatedVenue::Admit(const types::OrderRequest& request,
                                          std::size_t orders) {
    const types::Timestamp now = mClock();

    if (request.quantity.Sign() <= 0) {
        return MakeError(ErrorCategory::kInvalidArgument,
                         "order quantity must be positive");
    }
    if (!mLimits.allowed_symbols.empty() &&
        !mLimits.allowed_symbols.contains(request.symbol)) {
        return Reject("symbol " + request.symbol + " is not allowed");
    }

    std::optional<std::string> blocked;
    bool caught_up = false;  // the fills are applied to mPnl
    if (mHalted) {
        blocked = "trading is halted";
    } else if (mLimits.max_daily_loss || mLimits.max_consecutive_losses ||
               mLimits.max_drawdown) {
        auto checked = CheckLosses(now);
        if (!checked) {
            return Reject("could not work out the losses: " +
                          checked.error().ToString());
        }
        blocked = *checked;
        caught_up = true;
    }
    if (mLimits.max_orders_per_day) {
        if (DayOf(now) != mOrderDay) {
            mOrderDay = DayOf(now);
            mOrdersToday = 0;
        }
        if (!blocked && mOrdersToday + orders > *mLimits.max_orders_per_day) {
            blocked = std::to_string(mOrdersToday) + " orders today (limit " +
                      std::to_string(*mLimits.max_orders_per_day) + ")";
        }
    }

    // Holdings are needed to tell whether a blocked order reduces a position
    // and to size positions: the venue's, or what the gate has seen fill
    // since it started counting if that is more. A venue may report none
    // (Binance.US spot has no positions), and a halted gate must still let
    // the exit of what it bought through.
    Decimal held;
    if (blocked || mLimits.max_position_notional) {
        auto fetched = mVenue->GetPositions();
        if (!fetched) {
            return Reject("could not read positions: " +
                          fetched.error().ToString());
        }
        held = Held(*fetched, request.symbol);
        auto caught = caught_up ? types::Result<void>{} : CatchUpFills(now);
        if (caught) {
            // With shorts, the larger position either way (the venue's on a
            // tie).
            const Decimal seen = mPnl.Held(request.symbol);
            if (!mLimits.shorts) {
                held = std::max(held, seen);
            } else if (seen.Abs() > held.Abs()) {
                held = seen;
            }
        } else if (!blocked) {
            return Reject("could not read fills: " + caught.error().ToString());
        } else {
            // Only an exit can pass now: the venue's position alone is safe
            // to sell against, and refusing would keep the position open.
            spdlog::warn(
                "risk: could not read fills, so only the venue's "
                "position counts as held: {}",
                caught.error().ToString());
        }
    }
    const bool reduces = request.side == Side::kSell
                             ? request.quantity <= held
                             : mLimits.shorts && request.quantity <= -held;
    if (blocked && !reduces) {
        return Reject(*blocked);
    }

    // A position-reducing order that got past a halt skips the size limits;
    // it still counts towards the order rate.
    if (!blocked) {
        std::vector<types::OrderStatus> open;
        if (mLimits.max_open_orders || mLimits.max_position_notional) {
            auto fetched = mVenue->GetOpenOrders();
            if (!fetched) {
                return Reject("could not read open orders: " +
                              fetched.error().ToString());
            }
            open = std::move(*fetched);
        }
        if (mLimits.max_open_orders &&
            open.size() + orders > *mLimits.max_open_orders) {
            return Reject("already " + std::to_string(open.size()) +
                          " open orders (limit " +
                          std::to_string(*mLimits.max_open_orders) + ")");
        }

        if (mLimits.max_order_notional || mLimits.max_position_notional) {
            const auto price = PriceFor(request, now);
            if (!price || price->Sign() <= 0) {
                return Reject("no current price for " + request.symbol);
            }
            const Decimal notional = request.quantity * *price;
            if (mLimits.max_order_notional &&
                notional > *mLimits.max_order_notional) {
                return Reject("order notional " + notional.ToString() +
                              " exceeds limit " +
                              mLimits.max_order_notional->ToString());
            }
            // A buy adds to a long; with shorts, a sale past what is held
            // adds to a short.
            const bool buy = request.side == Side::kBuy;
            if (mLimits.max_position_notional &&
                (buy || (mLimits.shorts && !reduces))) {
                Decimal quantity =
                    buy ? held + request.quantity : request.quantity - held;
                for (const auto& order : open) {
                    if (order.symbol == request.symbol &&
                        order.side == request.side) {
                        quantity += order.quantity - order.filled_quantity;
                    }
                }
                const Decimal exposure = quantity * *price;
                if (exposure > *mLimits.max_position_notional) {
                    return Reject("position notional " + exposure.ToString() +
                                  " would exceed limit " +
                                  mLimits.max_position_notional->ToString());
                }
            }
        }
    }

    if (mLimits.max_orders_per_minute) {
        while (!mRecentOrders.empty() &&
               now - mRecentOrders.front() >= kRateWindow) {
            mRecentOrders.pop_front();
        }
        if (mRecentOrders.size() + orders > *mLimits.max_orders_per_minute) {
            return Reject("more than " +
                          std::to_string(*mLimits.max_orders_per_minute) +
                          " orders in the last minute");
        }
        mRecentOrders.insert(mRecentOrders.end(), orders, now);
    }
    if (mLimits.max_orders_per_day) {
        mOrdersToday += orders;
    }
    return {};
}

}  // namespace kestrel::trading
