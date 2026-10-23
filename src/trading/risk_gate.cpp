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
      mClock(std::move(clock)) {}

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
    return mVenue->CancelAll();
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

types::Result<std::optional<std::string>> RiskGatedVenue::CheckLosses(
    types::Timestamp now) {
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
        mPnl.Apply(fill);
        const Decimal gained = mPnl.Realized(fill.symbol) - before;
        mTotalRealized += gained;
        if (fill.side == Side::kSell) {
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
    }
    if (mLimits.max_orders_per_day) {
        if (DayOf(now) != mOrderDay) {
            mOrderDay = DayOf(now);
            mOrdersToday = 0;
        }
        if (!blocked && mOrdersToday >= *mLimits.max_orders_per_day) {
            blocked = std::to_string(mOrdersToday) + " orders today (limit " +
                      std::to_string(*mLimits.max_orders_per_day) + ")";
        }
    }

    // Holdings are needed to tell whether a blocked order reduces a position
    // and to size positions.
    std::vector<types::Position> positions;
    if (blocked || mLimits.max_position_notional) {
        auto fetched = mVenue->GetPositions();
        if (!fetched) {
            return Reject("could not read positions: " +
                          fetched.error().ToString());
        }
        positions = std::move(*fetched);
    }
    const Decimal held = Held(positions, request.symbol);
    const bool reduces =
        request.side == Side::kSell && request.quantity <= held;
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
            open.size() >= *mLimits.max_open_orders) {
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
            if (mLimits.max_position_notional && request.side == Side::kBuy) {
                Decimal quantity = held + request.quantity;
                for (const auto& order : open) {
                    if (order.symbol == request.symbol &&
                        order.side == Side::kBuy) {
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
        if (mRecentOrders.size() >= *mLimits.max_orders_per_minute) {
            return Reject("more than " +
                          std::to_string(*mLimits.max_orders_per_minute) +
                          " orders in the last minute");
        }
        mRecentOrders.push_back(now);
    }
    if (mLimits.max_orders_per_day) {
        ++mOrdersToday;
    }

    return mVenue->PlaceOrder(request);
}

}  // namespace kestrel::trading
