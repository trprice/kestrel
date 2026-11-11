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

std::chrono::sys_days DayOf(types::Timestamp t) {
    return std::chrono::floor<std::chrono::days>(t);
}

// How often a reference price may measure the losses, and how often one
// symbol may be closed.
constexpr std::chrono::seconds kWatchInterval{1};
constexpr std::chrono::minutes kCloseInterval{1};

// What `side` would hold of `symbol` if every open order of that side and
// `extra` filled: a buy's side is the long, held plus open buys; with
// shorts, a sell's side is the short, what is short plus open sells.
Decimal SideExposure(const types::Symbol& symbol, const Decimal& held,
                     const std::vector<types::OrderStatus>& open, Side side,
                     const Decimal& extra) {
    Decimal quantity = (side == Side::kBuy ? held : -held) + extra;
    for (const auto& order : open) {
        if (order.symbol == symbol && order.side == side) {
            quantity += order.quantity - order.filled_quantity;
        }
    }
    return quantity;
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
    const types::Symbol norm = types::NormalizeSymbol(symbol).value_or(symbol);
    const types::Timestamp now = mClock();
    {
        std::lock_guard lock(mPriceMutex);
        mPrices[norm] = ReferencePrice{price, now};
    }
    if (!mLimits.max_drawdown && !mLimits.max_drawdown_close &&
        !mLimits.max_loss_per_position) {
        return;
    }
    // An order being placed measures the losses itself; prices keep coming.
    std::unique_lock lock(mMutex, std::try_to_lock);
    if (!lock || now - mLastWatch < kWatchInterval) {
        return;
    }
    mLastWatch = now;
    // On the quote thread: a Decimal overflow must not unwind into the feed.
    types::Result<void> watched;
    try {
        watched = Watch(now);
    } catch (const std::exception& e) {
        watched = types::MakeError(types::ErrorCategory::kInternal, e.what());
    }
    if (!watched && !mWatchFailing) {
        spdlog::warn("risk: could not measure the losses: {}",
                     watched.error().ToString());
    } else if (watched && mWatchFailing) {
        spdlog::info("risk: measuring the losses again");
    }
    mWatchFailing = !watched;
}

void RiskGatedVenue::SetQuotesStale(const types::Symbol& symbol, bool stale) {
    const types::Symbol norm = types::NormalizeSymbol(symbol).value_or(symbol);
    std::lock_guard lock(mPriceMutex);
    if (stale) {
        mStaleQuotes.insert(norm);
    } else {
        mStaleQuotes.erase(norm);
    }
}

void RiskGatedVenue::SetSymbolInfo(
    std::function<types::Result<types::SymbolInfo>(const types::Symbol&)>
        info) {
    std::lock_guard lock(mPriceMutex);
    mSymbolInfo = std::move(info);
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

std::optional<Decimal> RiskGatedVenue::Drawdown(types::Timestamp now) {
    const auto unrealized = Unrealized(now);
    if (!unrealized) {
        return std::nullopt;
    }
    const Decimal equity = mTotalRealized + *unrealized;
    mPeakEquity = std::max(mPeakEquity, equity);
    return mPeakEquity - equity;
}

std::map<types::Symbol, Decimal> RiskGatedVenue::Holdings(
    const std::vector<types::Position>& positions, bool fills) const {
    std::map<types::Symbol, Decimal> held;
    for (const auto& position : positions) {
        held[position.symbol] = position.quantity;
    }
    if (fills) {
        for (const auto& position : mPnl.Positions()) {
            Decimal& h = held[position.symbol];
            // With shorts, the larger position either way (the venue's on a
            // tie).
            if (!mLimits.shorts) {
                h = std::max(h, position.quantity);
            } else if (position.quantity.Abs() > h.Abs()) {
                h = position.quantity;
            }
        }
    }
    std::erase_if(held, [](const auto& h) { return h.second.Sign() == 0; });
    return held;
}

void RiskGatedVenue::Close(const types::Symbol& symbol, const Decimal& held,
                           const std::string& reason, types::Timestamp now) {
    mLastClose[symbol] = now;
    if (auto open = mVenue->GetOpenOrders()) {
        for (const auto& order : *open) {
            if (order.symbol != symbol) {
                continue;
            }
            if (auto cancelled = mVenue->CancelOrder(order.order_id);
                !cancelled) {
                spdlog::warn("risk: could not cancel {} before closing {}: {}",
                             order.order_id, symbol,
                             cancelled.error().ToString());
            }
        }
    } else {
        spdlog::warn(
            "risk: could not read the open orders before closing {}: {}",
            symbol, open.error().ToString());
    }

    Decimal quantity = held.Abs();
    if (held.Sign() > 0) {
        // No more than the account has: a fee taken in the coin is not
        // taken off what the loss count holds.
        auto parts = types::SplitSymbol(symbol);
        auto balances = mVenue->GetBalances();
        if (parts && balances) {
            for (const auto& balance : *balances) {
                if (balance.asset == parts->base && balance.total.Sign() > 0) {
                    quantity = std::min(quantity, balance.total);
                }
            }
        }
    }
    std::function<types::Result<types::SymbolInfo>(const types::Symbol&)> info;
    {
        std::lock_guard lock(mPriceMutex);
        info = mSymbolInfo;
    }
    if (info) {
        // Fees taken in the coin can leave less than a lot over.
        if (auto rules = info(symbol); rules && rules->lot_size.Sign() > 0) {
            quantity = quantity.RoundToIncrement(rules->lot_size,
                                                 types::Rounding::kDown);
        }
    }
    if (quantity.Sign() <= 0) {
        spdlog::warn("risk: cannot close {} {}: less than one lot",
                     held.ToString(), symbol);
        return;
    }
    types::OrderRequest close;
    close.symbol = symbol;
    close.side = held.Sign() > 0 ? Side::kSell : Side::kBuy;
    close.type = types::OrderType::kMarket;
    close.quantity = quantity;
    auto placed = mVenue->PlaceOrder(close);
    if (!placed) {
        spdlog::error("risk: could not close {} {} ({}): {}",
                      quantity.ToString(), symbol, reason,
                      placed.error().ToString());
        return;
    }
    spdlog::warn("risk: closing {} {} at market ({}): order {}",
                 quantity.ToString(), symbol, reason, placed->order_id);
}

types::Result<void> RiskGatedVenue::Watch(types::Timestamp now) {
    if (auto caught = CatchUpFills(now); !caught) {
        return caught;
    }
    std::optional<Decimal> drawdown;
    if (mLimits.max_drawdown || mLimits.max_drawdown_close) {
        drawdown = Drawdown(now);
        // max_drawdown fails closed at the next order; the close cannot.
        if (mLimits.max_drawdown_close && !drawdown && !mDrawdownUnknown) {
            spdlog::warn(
                "risk: drawdown unknown (a held symbol has no current "
                "price), so max_drawdown_close cannot act");
        }
        mDrawdownUnknown = !drawdown.has_value();
    }

    // The symbols to close, and why; with `everything`, all that is held.
    std::map<types::Symbol, std::string> closing;
    std::optional<std::string> everything;
    if (mLimits.max_drawdown_close && drawdown &&
        *drawdown < *mLimits.max_drawdown_close) {
        mDrawdownClosed = false;  // a later fall cancels afresh
    }
    if (mLimits.max_drawdown_close && drawdown &&
        *drawdown >= *mLimits.max_drawdown_close) {
        // Halts and cancels at each breach; closes what is still held at most
        // once a minute, however often the fall crosses the limit (a close
        // still on its way shows as held).
        const std::string reason = "drawdown close limit reached (down " +
                                   drawdown->ToString() +
                                   " from the peak, limit " +
                                   mLimits.max_drawdown_close->ToString() + ")";
        if (!mLastDrawdownClose ||
            now - *mLastDrawdownClose >= kCloseInterval) {
            everything = reason;
            mLastDrawdownClose = now;
        }
        mHalted = true;  // again after a Resume, while the fall stands
        if (!mDrawdownClosed) {
            mDrawdownClosed = true;
            spdlog::warn("risk: trading halted: {}; closing every position",
                         reason);
            if (auto cancelled = mVenue->CancelAll(); !cancelled) {
                spdlog::warn("risk: could not cancel the open orders: {}",
                             cancelled.error().ToString());
            }
        }
    } else if (mLimits.max_loss_per_position) {
        for (const auto& position : mPnl.Positions()) {
            if (position.quantity.Sign() == 0) {
                continue;
            }
            const auto price = ReferencePriceFor(position.symbol, now);
            if (!price) {
                continue;  // not known: the strategy's own stop stands
            }
            const Decimal loss =
                position.quantity * (position.average_price - *price);
            if (loss < *mLimits.max_loss_per_position) {
                continue;
            }
            const auto last = mLastClose.find(position.symbol);
            if (last != mLastClose.end() &&
                now - last->second < kCloseInterval) {
                continue;  // closing already
            }
            mStopped[position.symbol] = DayOf(now);
            closing[position.symbol] =
                "position loss limit reached (down " + loss.ToString() +
                ", limit " + mLimits.max_loss_per_position->ToString() + ")";
        }
    }
    if (closing.empty() && !everything) {
        return {};
    }

    // What to close is what is held, as the gate counts it: the venue's
    // position too, if it reports more.
    auto positions = mVenue->GetPositions();
    if (!positions) {
        spdlog::warn("risk: could not read positions; closing what filled: {}",
                     positions.error().ToString());
    }
    const auto held =
        Holdings(positions ? *positions : std::vector<types::Position>{}, true);
    for (const auto& [symbol, quantity] : held) {
        if (!mLimits.shorts && quantity.Sign() < 0) {
            continue;
        }
        if (everything) {
            Close(symbol, quantity, *everything, now);
        } else if (const auto why = closing.find(symbol);
                   why != closing.end()) {
            Close(symbol, quantity, why->second, now);
        }
    }
    return {};
}

std::optional<std::string> RiskGatedVenue::Losses(types::Timestamp now) {
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
        const auto drawdown = Drawdown(now);
        if (!drawdown) {
            return std::string(
                "drawdown unknown: a held symbol has no current price");
        }
        if (*drawdown >= *mLimits.max_drawdown) {
            return "drawdown limit reached (down " + drawdown->ToString() +
                   " from the peak, limit " + mLimits.max_drawdown->ToString() +
                   ")";
        }
    }
    return std::nullopt;
}

types::Result<Decimal> RiskGatedVenue::Equity(const types::Symbol& symbol,
                                              types::Timestamp now) {
    auto parts = types::SplitSymbol(symbol);
    if (!parts) {
        return std::unexpected(parts.error());
    }
    const std::string quote =
        parts->quote.value_or(mLimits.default_quote_asset);
    auto balances = mVenue->GetBalances();
    if (!balances) {
        return std::unexpected(balances.error());
    }
    Decimal equity;
    for (const auto& balance : *balances) {
        if (balance.total.Sign() == 0) {
            continue;
        }
        if (balance.asset == quote) {
            equity += balance.total;
            continue;
        }
        // A coin's symbol names its quote; an equity's is the asset itself.
        auto price = ReferencePriceFor(balance.asset + "/" + quote, now);
        if (!price) {
            price = ReferencePriceFor(balance.asset, now);
        }
        if (!price) {
            if (mUnpricedAssets.insert(balance.asset).second) {
                spdlog::warn(
                    "risk: {} {} is left out of equity: no price for it",
                    balance.total.ToString(), balance.asset);
            }
            continue;
        }
        equity += balance.total * *price;
    }
    return equity;
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
    switch (request.type) {
        case types::OrderType::kLimit:
            return request.limit_price;
        case types::OrderType::kStop:
            return request.stop_price;
        case types::OrderType::kMarket:
            return ReferencePriceFor(request.symbol, now);
    }
    return ReferencePriceFor(request.symbol, now);
}

types::Result<void> RiskGatedVenue::CheckSize(
    const types::OrderRequest& request, const Decimal& price,
    const Decimal& held, const std::map<types::Symbol, Decimal>& holdings,
    const std::vector<types::OrderStatus>& open, types::Timestamp now) {
    const Decimal position = SideExposure(request.symbol, held, open,
                                          request.side, request.quantity) *
                             price;
    if (mLimits.max_position_notional &&
        position > *mLimits.max_position_notional) {
        return Reject("position notional " + position.ToString() +
                      " would exceed limit " +
                      mLimits.max_position_notional->ToString());
    }

    if (mLimits.max_total_notional) {
        // Each symbol on its larger side; the order's own symbol at its price.
        std::set<types::Symbol> symbols{request.symbol};
        for (const auto& [symbol, quantity] : holdings) {
            symbols.insert(symbol);
        }
        for (const auto& order : open) {
            symbols.insert(order.symbol);
        }
        Decimal total;
        for (const auto& symbol : symbols) {
            const auto found = holdings.find(symbol);
            const Decimal h =
                found != holdings.end() ? found->second : Decimal();
            const bool ordered = symbol == request.symbol;
            const auto extra = [&](Side side) {
                return ordered && request.side == side ? request.quantity
                                                       : Decimal();
            };
            Decimal quantity = std::max(
                SideExposure(symbol, h, open, Side::kBuy, extra(Side::kBuy)),
                Decimal());
            if (mLimits.shorts) {
                quantity = std::max(quantity,
                                    SideExposure(symbol, h, open, Side::kSell,
                                                 extra(Side::kSell)));
            }
            if (quantity.Sign() == 0) {
                continue;
            }
            const auto at = ordered ? std::optional<Decimal>(price)
                                    : ReferencePriceFor(symbol, now);
            if (!at) {
                return Reject("no current price for " + symbol +
                              " to count towards the total");
            }
            total += quantity * *at;
        }
        if (total > *mLimits.max_total_notional) {
            return Reject("total notional " + total.ToString() +
                          " would exceed limit " +
                          mLimits.max_total_notional->ToString());
        }
    }

    if (mLimits.max_symbol_share) {
        auto equity = Equity(request.symbol, now);
        if (!equity) {
            return Reject("could not work out equity: " +
                          equity.error().ToString());
        }
        if (equity->Sign() <= 0) {
            return Reject("equity " + equity->ToString() +
                          " is not above zero");
        }
        if (position > *mLimits.max_symbol_share * *equity) {
            return Reject("position notional " + position.ToString() +
                          " would be over " +
                          mLimits.max_symbol_share->ToString() + " of equity " +
                          equity->ToString());
        }
    }
    return {};
}

types::Result<types::OrderAcknowledgement> RiskGatedVenue::PlaceOrder(
    const types::OrderRequest& request) {
    std::lock_guard lock(mMutex);
    types::OrderRequest normalized = request;
    normalized.symbol =
        types::NormalizeSymbol(request.symbol).value_or(request.symbol);
    if (auto admitted = Admit(normalized, 1); !admitted) {
        return std::unexpected(admitted.error());
    }
    return mVenue->PlaceOrder(request);
}

types::Result<types::OcoAcknowledgement> RiskGatedVenue::PlaceOco(
    const types::OcoRequest& request) {
    std::lock_guard lock(mMutex);
    // Checked as its limit order, counting as two orders: both rest, though
    // only one can fill. For a buy OCO, the stop price is above the limit
    // price, so use the higher price as the worst-case notional.
    types::OrderRequest limit;
    limit.symbol =
        types::NormalizeSymbol(request.symbol).value_or(request.symbol);
    limit.side = request.side;
    limit.type = types::OrderType::kLimit;
    limit.quantity = request.quantity;
    limit.limit_price = request.side == Side::kBuy
                            ? std::max(request.limit_price, request.stop_price)
                            : request.limit_price;
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

    // Watching catches up on the fills, and may close positions (and halt)
    // before this order is judged.
    std::optional<std::string> blocked;
    bool caught_up = false;  // the fills are applied to mPnl
    const bool counting = mLimits.max_daily_loss ||
                          mLimits.max_consecutive_losses ||
                          mLimits.max_drawdown || mLimits.max_drawdown_close ||
                          mLimits.max_loss_per_position;
    if (counting) {
        auto watched = Watch(now);
        // Halted, an exit can still pass on the venue's position alone.
        if (!watched && !mHalted) {
            return Reject("could not work out the losses: " +
                          watched.error().ToString());
        }
        caught_up = watched.has_value();
    }
    if (mHalted) {
        blocked = "trading is halted";
    } else if (counting) {
        blocked = Losses(now);
    }
    if (!blocked) {
        if (const auto stopped = mStopped.find(request.symbol);
            stopped != mStopped.end() && stopped->second == DayOf(now)) {
            blocked = "the position in " + request.symbol +
                      " was closed at its loss limit today";
        }
    }
    if (!blocked && mLimits.max_quote_age) {
        std::lock_guard lock(mPriceMutex);
        if (mStaleQuotes.contains(request.symbol)) {
            blocked = "no quotes for " + request.symbol + " for over " +
                      std::to_string(mLimits.max_quote_age->count()) + " ms";
        }
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
    const bool sizing = mLimits.max_position_notional ||
                        mLimits.max_total_notional || mLimits.max_symbol_share;
    std::map<types::Symbol, Decimal> holdings;
    if (blocked || sizing) {
        auto fetched = mVenue->GetPositions();
        if (!fetched) {
            return Reject("could not read positions: " +
                          fetched.error().ToString());
        }
        auto caught = caught_up ? types::Result<void>{} : CatchUpFills(now);
        if (!caught && !blocked) {
            return Reject("could not read fills: " + caught.error().ToString());
        }
        if (!caught) {
            // Only an exit can pass now: the venue's position alone is safe
            // to sell against, and refusing would keep the position open.
            spdlog::warn(
                "risk: could not read fills, so only the venue's "
                "position counts as held: {}",
                caught.error().ToString());
        }
        holdings = Holdings(*fetched, caught.has_value());
    }
    const auto found = holdings.find(request.symbol);
    const Decimal held = found != holdings.end() ? found->second : Decimal();
    const bool buy = request.side == Side::kBuy;
    const bool reduces = !buy ? request.quantity <= held
                              : mLimits.shorts && request.quantity <= -held;
    if (blocked && !reduces) {
        return Reject(*blocked);
    }

    std::vector<types::OrderStatus> open;
    if (blocked) {
        // An exit, as the order limits do not apply to it. Exits already
        // working count against what is held, so two cannot sell it twice;
        // if the open orders cannot be read, the position alone decides.
        auto fetched = mVenue->GetOpenOrders();
        if (!fetched) {
            spdlog::warn(
                "risk: could not read open orders while checking an exit: {}",
                fetched.error().ToString());
        } else {
            Decimal working;
            for (const auto& order : *fetched) {
                if (order.symbol == request.symbol &&
                    order.side == request.side) {
                    working += order.quantity - order.filled_quantity;
                }
            }
            if (request.quantity + working > held.Abs()) {
                return Reject(*blocked + "; open orders already exit " +
                              working.ToString() + " of the " +
                              held.Abs().ToString() + " held");
            }
        }
    } else if (mLimits.max_open_orders || sizing) {
        auto fetched = mVenue->GetOpenOrders();
        if (!fetched) {
            return Reject("could not read open orders: " +
                          fetched.error().ToString());
        }
        open = std::move(*fetched);
        if (mLimits.max_open_orders &&
            open.size() + orders > *mLimits.max_open_orders) {
            return Reject("already " + std::to_string(open.size()) +
                          " open orders (limit " +
                          std::to_string(*mLimits.max_open_orders) + ")");
        }
    }

    // A position-reducing order that got past a halt skips the size limits;
    // it still counts towards the order rate.
    if (!blocked) {
        if (mLimits.max_order_notional || sizing) {
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
            // adds to a short. Other orders, a buy covering a short among
            // them, only reduce a position.
            const bool adds = !reduces && (buy || mLimits.shorts);
            if (adds && sizing) {
                if (auto sized =
                        CheckSize(request, *price, held, holdings, open, now);
                    !sized) {
                    return std::unexpected(sized.error());
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
