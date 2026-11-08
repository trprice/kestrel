#include "kestrel/strategy/market_maker.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
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
// The mid is sampled for the volatility at most this often, so the flicker
// of the book between trades does not count as volatility.
constexpr std::chrono::seconds kVolSampleInterval{1};

double ToDouble(const Decimal& d) {
    return static_cast<double>(d.Mantissa()) / std::pow(10.0, d.Scale());
}

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
        (mParams.requote_toward_bps &&
         mParams.requote_toward_bps->IsNegative()) ||
        mParams.max_quote_age < std::chrono::milliseconds::zero() ||
        mParams.quiet_pull < std::chrono::milliseconds::zero() ||
        mParams.skew_age < std::chrono::milliseconds::zero()) {
        throw std::invalid_argument(
            "skew_bps, requote_bps, requote_toward_bps, max_quote_age, "
            "quiet_pull and skew_age cannot be negative");
    }
    if (mParams.skew_power < Decimal::FromInt(1) ||
        mParams.skew_power > Decimal::FromInt(4)) {
        throw std::invalid_argument("skew_power must be from 1 to 4");
    }
    if (mParams.vol_multiplier.IsNegative() ||
        mParams.max_half_spread_bps.IsNegative()) {
        throw std::invalid_argument(
            "vol_multiplier and max_half_spread_bps cannot be negative");
    }
    if (mParams.vol_multiplier.Sign() > 0 &&
        mParams.vol_window <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("vol_window must be positive");
    }
    if (mParams.microprice_weight.IsNegative() ||
        mParams.microprice_weight > Decimal::FromInt(1)) {
        throw std::invalid_argument("microprice_weight must be from 0 to 1");
    }
    if (mParams.flow_pull_imbalance.IsNegative() ||
        mParams.flow_pull_imbalance > Decimal::FromInt(1)) {
        throw std::invalid_argument("flow_pull_imbalance must be from 0 to 1");
    }
    if (mParams.flow_pull_imbalance.Sign() > 0 &&
        mParams.flow_window <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("flow_window must be positive");
    }
    if (mParams.max_half_spread_bps.Sign() > 0 &&
        mParams.max_half_spread_bps < mParams.half_spread_bps) {
        throw std::invalid_argument(
            "max_half_spread_bps must be at least half_spread_bps");
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
    if (mParams.quiet_pull > std::chrono::milliseconds::zero()) {
        needs.timer_interval =
            std::clamp(mParams.quiet_pull / 4, std::chrono::milliseconds(100),
                       std::chrono::milliseconds(1000));
    }
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
    if (!mReady || quote.symbol != mParams.symbol) {
        return;
    }
    mLastArrival = context.Now();
    mQuietPulled = false;
    if (mQuietLogged) {
        mQuietLogged = false;
        spdlog::info("market_maker: quotes for {} again", quote.symbol);
    }
    if (mParams.flow_pull_imbalance.Sign() > 0) {
        NoteTrade(quote, context.Now());
    }
    if (!quote.bid || !quote.ask || quote.bid->Sign() <= 0 ||
        *quote.bid >= *quote.ask) {
        return;
    }
    const Decimal tick = mInfo.tick_size;
    const Decimal mid =
        Decimal::Divide(*quote.bid + *quote.ask, Decimal::FromInt(2), kScale,
                        Rounding::kNearest);
    Pause pause = Protect(context, quote, mid);
    if (mParams.flow_pull_imbalance.Sign() > 0) {
        FlowPause(context.Now(), pause);
    }

    // Lean away from the side the position is heavy on.
    const Decimal reservation =
        Centre(quote, mid) - Bps(mid, SkewBps(context.Now()));
    const Decimal half = Bps(mid, HalfSpreadBps(context.Now(), mid));

    Decimal bid = (reservation - half).RoundToIncrement(tick, Rounding::kDown);
    Decimal ask = (reservation + half).RoundToIncrement(tick, Rounding::kUp);
    bid = std::min(bid, *quote.ask - tick);  // never cross the book
    ask = std::max(ask, *quote.bid + tick);
    HoldToTouch(quote, bid, ask);

    // Room to add to the position, and to sell down. Spot cannot go short.
    // Fills the venue reported but OnFill has not delivered yet count as done.
    const auto now = context.Now();
    const Decimal bid_room = mStart + mParams.max_inventory - mInventory -
                             UnconfirmedFills(Side::kBuy, now);
    const Decimal ask_room =
        mInventory - std::max(Decimal(), mStart - mParams.max_inventory) -
        UnconfirmedFills(Side::kSell, now);

    const Decimal away = std::max(Bps(mid, mParams.requote_bps), tick);
    const Decimal toward = std::max(
        Bps(mid, mParams.requote_toward_bps.value_or(mParams.requote_bps)),
        tick);
    Manage(context, mBid, Side::kBuy,
           bid.Sign() > 0 && !pause.bid ? std::optional(bid) : std::nullopt,
           Size(bid_room), bid_room, away, toward, *quote.bid);
    Manage(context, mAsk, Side::kSell,
           pause.ask ? std::nullopt : std::optional(ask), Size(ask_room),
           ask_room, away, toward, *quote.ask);
}

void MarketMaker::HoldToTouch(const types::Quote& quote, Decimal& bid,
                              Decimal& ask) const {
    using Touch = MarketMakerParams::Touch;
    if (mParams.touch == Touch::kCompute) {
        return;
    }
    const Decimal tick = mInfo.tick_size;
    Decimal best_bid = *quote.bid;
    Decimal best_ask = *quote.ask;
    // Room to improve: three ticks of spread leaves a tick between our two
    // improved quotes.
    if (mParams.touch == Touch::kImprove &&
        best_ask - best_bid >= tick * Decimal::FromInt(3)) {
        // Our own quote at the touch is the improvement already made, and so
        // is our last one there: the book may not show yet that it filled.
        if (mLastBidPrice != best_bid) {
            best_bid += tick;
        }
        if (mLastAskPrice != best_ask) {
            best_ask -= tick;
        }
    }
    bid = std::min(bid, best_bid);
    ask = std::max(ask, best_ask);
}

Decimal MarketMaker::Centre(const types::Quote& quote, const Decimal& mid) {
    if (mParams.microprice_weight.IsZero() || !quote.bid_size ||
        !quote.ask_size || quote.bid_size->Sign() <= 0 ||
        quote.ask_size->Sign() <= 0) {
        return mid;
    }
    const Decimal micro = Decimal::Divide(
        *quote.bid * *quote.ask_size + *quote.ask * *quote.bid_size,
        *quote.bid_size + *quote.ask_size, kScale, Rounding::kNearest);
    const Decimal centre = (mid + mParams.microprice_weight * (micro - mid))
                               .Rescale(kScale, Rounding::kNearest);
    ++mCentreCount;
    mCentreShiftSum += std::abs(ToDouble(centre) / ToDouble(mid) - 1.0) * 1e4;
    return centre;
}

void MarketMaker::NoteTrade(const types::Quote& quote, types::Timestamp now) {
    const bool first = !mFirstQuote;
    if (first) {
        mFirstQuote = now;
    }
    mLastQuote = now;
    // Book updates repeat the last trade, so a trade is new only if its
    // price, size, side or time changed. The first quote's trade came before
    // the strategy was listening.
    const LastTrade last{quote.last, quote.last_size, quote.last_taker_side,
                         quote.exchange_time};
    if (quote.last && !first && (!mLastTrade || last != *mLastTrade)) {
        if (!quote.last_size || quote.last_size->Sign() <= 0 ||
            !quote.last_taker_side) {
            ++mFlowUncounted;
        } else {
            const bool bought = *quote.last_taker_side == Side::kBuy;
            mFlow.push_back({now, *quote.last_size, bought});
            (bought ? mFlowBought : mFlowSold) += *quote.last_size;
        }
    }
    if (quote.last) {
        mLastTrade = last;
    }
    while (!mFlow.empty() && mFlow.front().time <= now - mParams.flow_window) {
        const FlowTrade& old = mFlow.front();
        (old.bought ? mFlowBought : mFlowSold) -= old.size;
        mFlow.pop_front();
    }
}

void MarketMaker::FlowPause(types::Timestamp now, Pause& pause) {
    const Decimal total = mFlowBought + mFlowSold;
    const Decimal imbalance =
        total.Sign() > 0 ? Decimal::Divide(mFlowBought - mFlowSold, total,
                                           kScale, Rounding::kNearest)
                         : Decimal();
    const auto note = [&](FlowPulls& pulls, bool on, const char* side) {
        if (on && !pulls.on) {
            ++pulls.count;
            pulls.since = now;
            spdlog::debug("market_maker: pulling the {} on order flow ({})",
                          side, imbalance.ToString());
        } else if (!on && pulls.on) {
            pulls.total += now - pulls.since;
        }
        pulls.on = on;
    };
    const bool bid = imbalance <= -mParams.flow_pull_imbalance;
    const bool ask = imbalance >= mParams.flow_pull_imbalance;
    note(mFlowBid, bid, "bid");
    note(mFlowAsk, ask, "ask");
    pause.bid = pause.bid || bid;
    pause.ask = pause.ask || ask;
}

Decimal MarketMaker::HalfSpreadBps(types::Timestamp now, const Decimal& mid) {
    if (mParams.vol_multiplier.IsZero()) {
        return mParams.half_spread_bps;
    }
    if (!mVolLast) {
        mVolLast.emplace(now, mid);
        mVolFirst = now;
    } else if (now - mVolLast->first >= kVolSampleInterval) {
        const double move =
            (ToDouble(mid) / ToDouble(mVolLast->second) - 1.0) * 1e4;
        mVolSamples.push_back({mVolLast->first, now, move * move});
        mVolLast.emplace(now, mid);
        // The newest sample is kept even if it alone is older than the
        // window: after a gap it is all there is.
        while (mVolSamples.size() >= 2 &&
               mVolSamples.front().start < now - mParams.vol_window) {
            mVolSamples.pop_front();
        }
        mVolBps = 0;
        if (now - *mVolFirst >= mParams.vol_window) {
            double sum = 0;
            for (const VolSample& s : mVolSamples) {
                sum += s.squared_bps;
            }
            const std::chrono::duration<double, std::ratio<60>> covered =
                mVolSamples.back().end - mVolSamples.front().start;
            mVolBps = std::sqrt(sum / covered.count());
        }
    }

    Decimal half = mParams.half_spread_bps;
    const double scaled = ToDouble(mParams.vol_multiplier) * mVolBps;
    if (scaled > ToDouble(half)) {
        half = Decimal(std::llround(scaled * 100), 2);  // to 0.01 bps
        ++mHalfWidened;
    }
    if (mParams.max_half_spread_bps.Sign() > 0) {
        half = std::min(half, mParams.max_half_spread_bps);
    }
    ++mHalfCount;
    mHalfSum += ToDouble(half);
    mHalfMax = std::max(mHalfMax, half);
    return half;
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
                         const Decimal& away, const Decimal& toward,
                         const Decimal& best) {
    const bool wanted = price && size.Sign() > 0;
    // What the new quote replaces, and why, for the log.
    std::string replaced;
    if (!quote.order_id.empty()) {
        const bool fresh =
            mParams.max_quote_age == std::chrono::milliseconds::zero() ||
            context.Now() - quote.placed < mParams.max_quote_age;
        // How far the market has moved towards the resting quote: positive
        // when the target is below a bid or above an ask, negative when it
        // has moved away.
        const Decimal closer = wanted
                                   ? (side == Side::kBuy ? quote.price - *price
                                                         : *price - quote.price)
                                   : Decimal();
        const bool moved_toward = closer.Sign() > 0 && closer >= toward;
        // At the touch, a move away costs nothing yet: keep the place in the
        // queue.
        const bool at_touch = mParams.keep_at_touch && quote.price == best;
        const bool moved_away =
            closer.Sign() < 0 && -closer >= away && !at_touch;
        const bool moved = wanted && (moved_toward || moved_away);
        const bool keep = wanted && !quote.cancel_raced && fresh &&
                          quote.Remaining() <= room && !moved;
        if (keep) {
            return;
        }
        const char* why = !wanted        ? "not wanted"
                          : moved_toward ? "market moved to it"
                          : moved_away   ? "market moved away"
                          : !fresh       ? "too old"
                                         : "over the room left";
        const std::string old_id = quote.order_id;
        const std::string old_price = quote.price.ToString();
        if (!Pull(context, quote)) {
            return;
        }
        if (!wanted) {
            spdlog::info("market_maker: pulled {} quote {} @ {} ({})",
                         types::ToString(side), old_id, old_price, why);
            return;
        }
        replaced = ", replaces " + old_id + " @ " + old_price + ", " + why;
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
    request.post_only = mParams.post_only;
    auto ack = context.Venue().PlaceOrder(request);
    if (!ack && ack.error().category == types::ErrorCategory::kWouldTake) {
        // The book moved through the price; the next quote tries again.
        ++mWouldTake;
        spdlog::debug("market_maker: post-only {} {} @ {} refused: {}",
                      types::ToString(side), size.ToString(), price->ToString(),
                      ack.error().ToString());
        return;
    }
    if (!ack) {
        spdlog::warn("market_maker: {} {} @ {} failed: {}",
                     types::ToString(side), size.ToString(), price->ToString(),
                     ack.error().ToString());
        return;
    }
    if (!types::IsActive(ack->state)) {
        // Filled or refused at once. A fill is on its way to OnFill; until it
        // arrives the inventory does not show it.
        spdlog::info("market_maker: {} {} @ {} (order {}{}) was {} at once",
                     types::ToString(side), size.ToString(), price->ToString(),
                     ack->order_id, replaced, types::ToString(ack->state));
        if (ack->state == types::OrderState::kFilled) {
            ExpectFills(ack->order_id, side, size, Decimal(), context.Now());
            (side == Side::kBuy ? mLastBidPrice : mLastAskPrice) = *price;
        }
        return;
    }
    quote =
        Resting{ack->order_id, *price, size, Decimal(), context.Now(), false};
    (side == Side::kBuy ? mLastBidPrice : mLastAskPrice) = *price;
    spdlog::info("market_maker: quote {} {} @ {} (order {}{})",
                 types::ToString(side), size.ToString(), price->ToString(),
                 ack->order_id, replaced);
}

Decimal MarketMaker::SkewBps(types::Timestamp now) const {
    if (mParams.skew_bps.IsZero()) {
        return Decimal();
    }
    Decimal lean = Decimal::Divide(Position(), mParams.max_inventory, kScale,
                                   Rounding::kNearest);
    lean = std::clamp(lean, Decimal::FromInt(-1), Decimal::FromInt(1));
    if (mParams.skew_power == Decimal::FromInt(1) &&
        mParams.skew_age == std::chrono::milliseconds::zero()) {
        return mParams.skew_bps * lean;  // linear, exactly as before
    }
    double shift =
        std::pow(std::abs(ToDouble(lean)), ToDouble(mParams.skew_power)) *
        (lean.IsNegative() ? -1.0 : 1.0);
    if (mParams.skew_age > std::chrono::milliseconds::zero() &&
        mHeldSide != 0) {
        const double held =
            std::chrono::duration<double>(now - mHeldSince).count() /
            std::chrono::duration<double>(mParams.skew_age).count();
        shift *= 1.0 + std::clamp(held, 0.0, 1.0);
    }
    return mParams.skew_bps * Decimal(std::llround(shift * 1e8), 8);
}

void MarketMaker::NoteHeld(types::Timestamp now) {
    // Held from half a quote's quantity: a buy's fee taken in the coin
    // (Binance.US) leaves a full quote's fill a little under its quantity.
    const Decimal position = Position();
    const int side = position.Abs() * Decimal::FromInt(2) < mParams.quantity
                         ? 0
                         : position.Sign();
    if (side != mHeldSide) {
        mHeldSide = side;
        mHeldSince = now;
    }
}

void MarketMaker::OnFill(Context& context, const types::Fill& fill) {
    if (fill.symbol != mParams.symbol) {
        return;
    }
    const Decimal before = Position();
    mInventory += fill.side == Side::kBuy ? fill.quantity : -fill.quantity;
    // A fee in the base asset (Binance.US charges a taker buy's fee in what
    // was bought) leaves less held than was filled; selling the full amount
    // would be refused for the balance.
    if (fill.fee_asset == mBaseAsset) {
        mInventory -= fill.fee;
    }

    // The cash, and the average entry price of the position. A base-asset fee
    // is already counted as the units it took away.
    const Decimal notional = fill.quantity * fill.price;
    mCash += fill.side == Side::kBuy ? -notional : notional;
    if (fill.fee_asset == mQuoteAsset) {
        mCash -= fill.fee;
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
    NoteHeld(context.Now());
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

void MarketMaker::OnTimer(Context& context) {
    if (!mReady || mQuietPulled || !mLastArrival ||
        mParams.quiet_pull <= std::chrono::milliseconds::zero()) {
        return;
    }
    const auto quiet = context.Now() - *mLastArrival;
    if (quiet < mParams.quiet_pull) {
        return;
    }
    // Logged once per silence, when there was something to pull.
    const bool resting = !mBid.order_id.empty() || !mAsk.order_id.empty();
    if (resting && !mQuietLogged) {
        mQuietLogged = true;
        spdlog::warn(
            "market_maker: no quote for {} for {} ms; pulling the quotes",
            mParams.symbol,
            std::chrono::duration_cast<std::chrono::milliseconds>(quiet)
                .count());
    }
    // A side that cannot be pulled yet (a cancel lost to a fill) is tried
    // again on the next tick.
    const bool bid = Pull(context, mBid);
    const bool ask = Pull(context, mAsk);
    mQuietPulled = bid && ask;
}

void MarketMaker::OnStop(Context& context) {
    if (mHalfCount > 0) {
        spdlog::info(
            "market_maker: half-spread {:.2f} bps on average, {} at most, "
            "widened by volatility on {:.1f}% of quotes",
            mHalfSum / static_cast<double>(mHalfCount),
            mHalfMax.Normalized().ToString(),
            100.0 * static_cast<double>(mHalfWidened) /
                static_cast<double>(mHalfCount));
    }
    if (mCentreCount > 0) {
        spdlog::info(
            "market_maker: the microprice moved the centre {:.2f} bps from "
            "the mid on average",
            mCentreShiftSum / static_cast<double>(mCentreCount));
    }
    if (mParams.flow_pull_imbalance.Sign() > 0 && mFirstQuote) {
        const double span =
            std::chrono::duration<double>(mLastQuote - *mFirstQuote).count();
        const auto share = [&](const FlowPulls& pulls) {
            auto total = pulls.total;
            if (pulls.on) {
                total += mLastQuote - pulls.since;
            }
            return span > 0
                       ? 100.0 * std::chrono::duration<double>(total).count() /
                             span
                       : 0.0;
        };
        spdlog::info(
            "market_maker: order flow pulled the bid {} time(s) ({:.1f}% of "
            "the run) and the ask {} ({:.1f}%); {} trade(s) without a side "
            "or size not counted",
            mFlowBid.count, share(mFlowBid), mFlowAsk.count, share(mFlowAsk),
            mFlowUncounted);
    }
    if (mWouldTake > 0) {
        spdlog::info(
            "market_maker: {} post-only quote(s) refused as they would have "
            "taken liquidity",
            mWouldTake);
    }
    for (Resting* quote : {&mBid, &mAsk}) {
        if (!quote->order_id.empty() && !quote->cancel_raced) {
            auto outcome = context.Venue().CancelOrder(quote->order_id);
            if (outcome) {
                spdlog::info(
                    "market_maker: stopping, {} {} @ {}",
                    *outcome == trading::CancelOutcome::kCancelled
                        ? "cancelled"
                        : "already closed:",
                    quote->order_id, quote->price.ToString());
            } else {
                spdlog::warn("market_maker: stopping, cancel of {} failed: {}",
                             quote->order_id, outcome.error().ToString());
            }
        }
    }
}

}  // namespace kestrel::strategy
