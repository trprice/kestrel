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
// The longest window a protection rule looks back over.
constexpr std::chrono::minutes kHistory{15};
// How long a reported fill may go without its OnFill before it is logged.
constexpr std::chrono::seconds kUnconfirmedWarning{30};
const Decimal kBpsDivisor = Decimal::FromInt(10000);

// `value` * `bps` / 10000.
Decimal Bps(const Decimal& value, const Decimal& bps) {
    return Decimal::Divide(value * bps, kBpsDivisor, kScale,
                           Rounding::kNearest);
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
    const auto& r = mParams.protection;
    const auto require_positive = [](bool enabled, const Decimal& value,
                                     const char* name) {
        if (enabled && value.Sign() <= 0) {
            throw std::invalid_argument(std::string(name) +
                                        " must be positive");
        }
    };
    require_positive(r.stop_loss, r.stop_loss_bps, "stop_loss_bps");
    require_positive(r.loss_cap, r.loss_cap_amount, "loss_cap_amount");
    require_positive(r.drawdown_cap, r.drawdown_cap_amount,
                     "drawdown_cap_amount");
    require_positive(r.spike_standdown, r.spike_3m_bps, "spike_3m_bps");
    require_positive(r.spike_standdown, r.spike_5m_bps, "spike_5m_bps");
    require_positive(r.spike_standdown, r.spike_15m_bps, "spike_15m_bps");
    require_positive(r.downtrend_filter, r.downtrend_bps, "downtrend_bps");
    if (r.momentum_filter && r.momentum_bps.IsNegative()) {
        throw std::invalid_argument("momentum_bps cannot be negative");
    }
    if (r.spike_pause < std::chrono::milliseconds::zero() ||
        r.cooldown < std::chrono::milliseconds::zero()) {
        throw std::invalid_argument(
            "spike_pause and cooldown cannot be negative");
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
        spdlog::error(
            "market_maker: cannot read the {} balance: {}", mParams.symbol,
            parts ? balances.error().ToString() : parts.error().ToString());
        return;
    }
    mBaseAsset = parts->base;
    mQuoteAsset = parts->quote.value_or("");
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
    const Decimal rounded =
        size.RoundToIncrement(mInfo.lot_size, Rounding::kDown);
    return rounded < mInfo.min_order_size ? Decimal() : rounded;
}

void MarketMaker::OnQuote(Context& context, const types::Quote& quote) {
    if (!mReady || quote.symbol != mParams.symbol || !quote.bid || !quote.ask ||
        quote.bid->Sign() <= 0 || *quote.bid >= *quote.ask) {
        return;
    }
    const Decimal tick = mInfo.tick_size;
    const Decimal mid =
        Decimal::Divide(*quote.bid + *quote.ask, Decimal::FromInt(2), kScale,
                        Rounding::kNearest);
    const Pause pause = Protect(context, quote, mid);

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
    // Fills the venue reported but OnFill has not delivered yet count as done.
    const auto now = context.Now();
    const Decimal bid_room = mStart + mParams.max_inventory - mInventory -
                             UnconfirmedFills(Side::kBuy, now);
    const Decimal ask_room =
        mInventory - std::max(Decimal(), mStart - mParams.max_inventory) -
        UnconfirmedFills(Side::kSell, now);

    const Decimal threshold = std::max(Bps(mid, mParams.requote_bps), tick);
    Manage(context, mBid, Side::kBuy,
           bid.Sign() > 0 && !pause.bid ? std::optional(bid) : std::nullopt,
           Size(bid_room), bid_room, threshold);
    Manage(context, mAsk, Side::kSell,
           pause.ask ? std::nullopt : std::optional(ask), Size(ask_room),
           ask_room, threshold);
}

Decimal MarketMaker::Move(types::Timestamp now,
                          std::chrono::milliseconds window,
                          const Decimal& mid) const {
    const auto cutoff = now - window;
    const auto it = std::upper_bound(
        mMids.begin(), mMids.end(), cutoff,
        [](types::Timestamp t, const auto& entry) { return t < entry.first; });
    if (it == mMids.begin()) {
        return Decimal();  // not enough history
    }
    const Decimal& past = std::prev(it)->second;
    return Decimal::Divide((mid - past) * Decimal::FromInt(10000), past, 4,
                           Rounding::kNearest);
}

MarketMaker::Pause MarketMaker::Protect(Context& context,
                                        const types::Quote& quote,
                                        const Decimal& mid) {
    const auto& rules = mParams.protection;
    const auto now = context.Now();
    Pause pause;

    // Losses, measured on every quote.
    if (!mHalted && (rules.loss_cap || rules.drawdown_cap)) {
        const Decimal pnl = mCash + Position() * mid;
        mPeak = std::max(mPeak, pnl);
        if (rules.loss_cap && pnl <= -rules.loss_cap_amount) {
            Halt("loss cap");
        } else if (rules.drawdown_cap &&
                   mPeak - pnl >= rules.drawdown_cap_amount) {
            Halt("drawdown cap");
        }
    }
    if (mHalted) {
        ExitPosition(context, "halted");
        return {true, true};
    }

    if (rules.stop_loss && !mExit && Position().Abs() >= mInfo.min_order_size) {
        const Decimal reach = Bps(mEntry, rules.stop_loss_bps);
        const bool long_stopped =
            Position().Sign() > 0 && *quote.bid <= mEntry - reach;
        const bool short_stopped =
            Position().Sign() < 0 && *quote.ask >= mEntry + reach;
        if ((long_stopped || short_stopped) &&
            ExitPosition(context, "stop loss") && rules.cooldown_after_stop) {
            mCooldownUntil = now + rules.cooldown;
        }
    }

    // Price moves.
    if (rules.spike_standdown || rules.downtrend_filter ||
        rules.momentum_filter) {
        mMids.emplace_back(now, mid);
        while (mMids.size() >= 2 && mMids[1].first <= now - kHistory) {
            mMids.pop_front();
        }
        const Decimal m3 = Move(now, std::chrono::minutes(3), mid);
        const Decimal m5 = Move(now, std::chrono::minutes(5), mid);
        const Decimal m15 = Move(now, kHistory, mid);
        if (rules.spike_standdown &&
            (m3.Abs() >= rules.spike_3m_bps || m5.Abs() >= rules.spike_5m_bps ||
             m15.Abs() >= rules.spike_15m_bps)) {
            if (now >= mStandUntil) {
                spdlog::warn(
                    "market_maker: spike (3m {} bps, 5m {}, 15m {}), standing "
                    "down",
                    m3.ToString(), m5.ToString(), m15.ToString());
            }
            mStandUntil = now + rules.spike_pause;
        }
        if (rules.downtrend_filter && m15 <= -rules.downtrend_bps) {
            pause.bid = true;
        }
        if (rules.momentum_filter && m3 < -rules.momentum_bps) {
            pause.bid = true;
        }
    }
    if (mExit || now < mStandUntil || now < mCooldownUntil) {
        pause.bid = pause.ask = true;
    }
    return pause;
}

void MarketMaker::Halt(const char* why) {
    mHalted = true;
    spdlog::error("market_maker: halted ({}), position {}", why,
                  Position().ToString());
}

bool MarketMaker::ExitPosition(Context& context, const char* why) {
    if (mExit) {
        return false;
    }
    const Decimal quantity =
        Position().Abs().RoundToIncrement(mInfo.lot_size, Rounding::kDown);
    if (quantity.Sign() <= 0 || quantity < mInfo.min_order_size) {
        return false;
    }
    // Both quotes must be gone first, or a resting one could fill into the
    // exit and the position would be sold twice.
    const bool bid_gone = Pull(context, mBid);
    const bool ask_gone = Pull(context, mAsk);
    if (!bid_gone || !ask_gone) {
        return false;  // try again on the next quote
    }
    types::OrderRequest request;
    request.symbol = mParams.symbol;
    request.side = Position().Sign() > 0 ? Side::kSell : Side::kBuy;
    request.type = types::OrderType::kMarket;
    request.quantity = quantity;
    auto ack = context.Venue().PlaceOrder(request);
    if (!ack) {
        spdlog::warn("market_maker: exit ({}) failed: {}", why,
                     ack.error().ToString());
        return false;
    }
    spdlog::warn("market_maker: exiting {} {} at market ({})",
                 types::ToString(request.side), quantity.ToString(), why);
    if (!types::IsActive(ack->state) &&
        ack->state != types::OrderState::kFilled) {
        return false;  // refused; nothing will fill
    }
    mExit = Exit{ack->order_id, quantity, Decimal()};
    return true;
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
        // Filled or refused at once. A fill is on its way to OnFill; until it
        // arrives the inventory does not show it.
        if (ack->state == types::OrderState::kFilled) {
            ExpectFills(ack->order_id, side, size, Decimal(), context.Now());
        }
        return;
    }
    quote =
        Resting{ack->order_id, *price, size, Decimal(), context.Now(), false};
    spdlog::debug("market_maker: {} {} @ {} (order {})", types::ToString(side),
                  size.ToString(), price->ToString(), ack->order_id);
}

void MarketMaker::OnFill(Context&, const types::Fill& fill) {
    if (fill.symbol != mParams.symbol) {
        return;
    }
    const Decimal before = Position();
    mInventory += fill.side == Side::kBuy ? fill.quantity : -fill.quantity;

    // The cash, and the average entry price of the position.
    const Decimal notional = fill.quantity * fill.price;
    mCash += fill.side == Side::kBuy ? -notional : notional;
    if (fill.fee_asset == mQuoteAsset) {
        mCash -= fill.fee;
    } else if (fill.fee_asset == mBaseAsset) {
        mCash -= fill.fee * fill.price;
    }
    const Decimal after = Position();
    if (after.IsZero()) {
        mEntry = Decimal();
    } else if (before.IsZero() || before.Sign() != after.Sign()) {
        mEntry = fill.price;  // a new position
    } else if (after.Abs() > before.Abs()) {
        mEntry =
            Decimal::Divide(before.Abs() * mEntry + fill.quantity * fill.price,
                            after.Abs(), kScale, Rounding::kNearest);
    }  // a reduction keeps its entry price

    if (mExit && mExit->order_id == fill.order_id) {
        mExit->filled += fill.quantity;
        if (mExit->filled >= mExit->quantity) {
            mExit.reset();
        }
    }
    for (Resting* quote : {&mBid, &mAsk}) {
        if (!quote->order_id.empty() && quote->order_id == fill.order_id) {
            quote->filled += fill.quantity;
        }
    }
    if (auto it = mUnconfirmed.find(fill.order_id); it != mUnconfirmed.end()) {
        it->second.seen += fill.quantity;
        if (it->second.seen >= it->second.expected) {
            mUnconfirmed.erase(it);
        }
    }
    spdlog::info("market_maker: {} {} @ {}, inventory {}",
                 types::ToString(fill.side), fill.quantity.ToString(),
                 fill.price.ToString(), mInventory.ToString());
}

void MarketMaker::OnOrderUpdate(Context& context,
                                const types::OrderStatus& status) {
    if (types::IsActive(status.state)) {
        return;
    }
    if (mExit && mExit->order_id == status.order_id &&
        status.state != types::OrderState::kFilled) {
        mExit.reset();  // closed without filling; the next quote may retry
    }
    for (Resting* quote : {&mBid, &mAsk}) {
        if (!quote->order_id.empty() && quote->order_id == status.order_id) {
            // Closed with fills OnFill has not delivered: keep counting them.
            ExpectFills(status.order_id, status.side, status.filled_quantity,
                        quote->filled, context.Now());
            *quote = Resting();
        }
    }
}

void MarketMaker::ExpectFills(const std::string& order_id, Side side,
                              const Decimal& filled, const Decimal& seen,
                              types::Timestamp now) {
    if (filled > seen) {
        mUnconfirmed[order_id] = Unconfirmed{side, filled, seen, now, false};
    }
}

Decimal MarketMaker::UnconfirmedFills(Side side, types::Timestamp now) {
    Decimal total;
    for (auto& [order_id, pending] : mUnconfirmed) {
        if (pending.side != side) {
            continue;
        }
        total += pending.expected - pending.seen;
        // Kept until the fill arrives, however long: a fill that never
        // arrives leaves that side short of room rather than over the limit.
        if (!pending.warned && now - pending.since >= kUnconfirmedWarning) {
            pending.warned = true;
            spdlog::warn(
                "market_maker: order {} filled {} but only {} has arrived "
                "after {} s; that side keeps the room for it",
                order_id, pending.expected.ToString(), pending.seen.ToString(),
                std::chrono::duration_cast<std::chrono::seconds>(now -
                                                                 pending.since)
                    .count());
        }
    }
    return total;
}

void MarketMaker::OnStop(Context& context) {
    for (Resting* quote : {&mBid, &mAsk}) {
        if (!quote->order_id.empty() && !quote->cancel_raced) {
            (void)context.Venue().CancelOrder(quote->order_id);
        }
    }
}

}  // namespace kestrel::strategy
