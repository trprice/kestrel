#include "kestrel/strategy/market_maker.hpp"

#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>
#include <stdexcept>

#include "kestrel/util/json.hpp"

namespace kestrel::strategy {

using types::Decimal;
using types::Rounding;
using types::Side;

namespace {

constexpr int kScale = 8;
// The longest window a protection rule looks back over.
constexpr std::chrono::minutes kHistory{15};
// Kept state is saved at least this often while the P&L moves, and at once
// when the halt, a stand-down or a cooldown changes, or a fill lands.
constexpr std::chrono::seconds kStateSaveInterval{60};
// After a failed save, the next attempt waits this long.
constexpr std::chrono::seconds kStateRetryInterval{10};
// How long a reported fill may go without its OnFill before it is logged.
constexpr std::chrono::seconds kUnconfirmedWarning{30};
// An unconfirmed fill is checked with the venue after this long, and again
// at most this often.
constexpr std::chrono::seconds kReconcileAfter{60};
// And booked unseen after this long: past the runner's duplicate check
// (Runner::kSeenFillHorizon, 10 minutes), a fill not delivered by then is
// unlikely to come (it still can, if no newer fill moved the runner's
// cursor on: the copy is skipped, see mBookedUnseen).
constexpr std::chrono::minutes kBookUnseenAfter{11};
// A booked fill's late copy is watched for this long, then forgotten.
constexpr std::chrono::minutes kBookedUnseenKept{20};
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
    if (mParams.levels == 0 || mParams.levels > 10 ||
        mParams.level_step_bps.IsNegative()) {
        throw std::invalid_argument(
            "levels must be from 1 to 10, and level_step_bps not negative");
    }
    mBids.resize(mParams.levels);
    mAsks.resize(mParams.levels);
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
    needs.state = KeepsState();
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
        context.Log().Error("no symbol info for {}: {}", mParams.symbol,
                            info.error().ToString());
        return;
    }
    mInfo = *info;
    // Spot: what is held is the base asset's balance. (Positions only cover
    // what was bought, not a balance the account started with.)
    const auto parts = types::SplitSymbol(mParams.symbol);
    auto balances = context.Venue().GetBalances();
    if (!parts || !balances) {
        context.Log().Error(
            "cannot read the {} balance: {}", mParams.symbol,
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
    mReady = LoadState(context);
}

bool MarketMaker::KeepsState() const {
    const auto& rules = mParams.protection;
    return rules.loss_cap || rules.drawdown_cap || rules.spike_standdown ||
           rules.cooldown_after_stop;
}

bool MarketMaker::LoadState(Context& context) {
    StateStore* store = context.State();
    if (!store) {
        return true;
    }
    if (mParams.protection.reset_state) {
        if (auto erased = store->Erase(StateKey()); !erased) {
            context.Log().Error("cannot reset the saved state: {}",
                                erased.error().ToString());
            return false;
        }
        context.Log().Warn(
            "saved state reset; starting afresh, the caps from zero (take "
            "reset_state out of the config for the next run)");
        return true;
    }
    auto value = store->Load(StateKey());
    if (!value) {
        // Fail closed: without it the caps would restart from zero.
        context.Log().Error("cannot read the saved state ({}); not quoting",
                            value.error().ToString());
        return false;
    }
    if (!*value) {
        return true;  // nothing saved yet
    }
    auto json = util::ParseJson(**value);
    const auto decimal = [&](const char* key) -> std::optional<Decimal> {
        if (!json || !json->contains(key)) {
            return std::nullopt;
        }
        auto d = util::JsonDecimal((*json)[key]);
        return d ? std::optional(*d) : std::nullopt;
    };
    const auto pnl = decimal("pnl");
    const auto peak = decimal("peak");
    if (!json || !json->is_object() || !pnl || !peak ||
        !(*json)["halted"].is_boolean() ||
        !(*json)["stand_until_ms"].is_number_integer() ||
        !(*json)["cooldown_until_ms"].is_number_integer()) {
        context.Log().Error(
            "the saved state is not readable ({}); not quoting. reset_state = "
            "true starts afresh",
            **value);
        return false;
    }
    mHalted = (*json)["halted"].get<bool>();
    mCarried = *pnl;
    mLastPnl = *pnl;
    // A peak below the carried P&L (the caps were off, or it was halted)
    // is raised to it by the first quote; drawdown is from there.
    mPeak = *peak;
    // The position the last run held, with its entry and the mid it was
    // marked at in the carried P&L. Taken back on, so its moves from that
    // mark count, the stop loss watches it, and a halted maker exits it.
    // (States saved before these fields carry no position.)
    const Decimal position = decimal("position").value_or(Decimal());
    if (!position.IsZero()) {
        mEntry = decimal("entry").value_or(Decimal());
        // The mid its value in the carried P&L was taken at (the entry if
        // none was saved, or a mark of zero).
        const auto saved_mark = decimal("mark");
        const Decimal mark =
            saved_mark && saved_mark->Sign() > 0 ? *saved_mark : mEntry;
        if (mark.Sign() <= 0) {
            // Only a damaged row: its position cannot be valued.
            context.Log().Error(
                "the saved state holds a position of {} with no price; not "
                "quoting. reset_state = true starts afresh",
                position.ToString());
            return false;
        }
        Decimal held = position;
        if (position.Sign() > 0 && position > mInventory) {
            context.Log().Warn(
                "the last run held {} more {} than it started with, but only "
                "{} is held now; taking that on (the rest stays in the "
                "carried P&L at its last mark)",
                position.ToString(), mBaseAsset, mInventory.ToString());
            held = mInventory;
        }
        mStart = mInventory - held;
        // Its value at the mark is in the carried P&L already, so only its
        // moves from the mark count. (Its holding age starts again.)
        mCash = -held * mark;
        mLastMark = mark;
        NoteHeld(context.Now());
    }
    mStandUntil =
        types::FromEpochMillis((*json)["stand_until_ms"].get<std::int64_t>());
    mCooldownUntil = types::FromEpochMillis(
        (*json)["cooldown_until_ms"].get<std::int64_t>());
    context.Log().Info(
        "resumed the saved state: P&L {} (peak {}), position {}{}",
        mCarried.Normalized().ToString(), mPeak.Normalized().ToString(),
        Position().Normalized().ToString(), mHalted ? ", halted" : "");
    if (mHalted) {
        context.Log().Error(
            "halted in an earlier run; not quoting{}. reset_state = true "
            "starts afresh",
            Position().IsZero() ? "" : ", exiting its position");
    }
    return true;
}

void MarketMaker::SaveState(Context& context, bool now) {
    StateStore* store = context.State();
    if (!store || !mReady) {
        return;
    }
    const auto at = context.Now();
    if (at < mStateRetryAt) {
        return;  // a save failed a moment ago
    }
    if (!now && mStateSaved && at - *mStateSaved < kStateSaveInterval) {
        return;
    }
    nlohmann::json json;
    json["halted"] = mHalted;
    json["pnl"] = mLastPnl.Normalized().ToString();
    json["peak"] = mPeak.Normalized().ToString();
    json["stand_until_ms"] = types::ToEpochMillis(mStandUntil);
    json["cooldown_until_ms"] = types::ToEpochMillis(mCooldownUntil);
    json["position"] = Position().Normalized().ToString();
    json["entry"] = mEntry.Normalized().ToString();
    // The mid the P&L was taken at, which a resumed position is measured
    // from. Fills since the last quote are in the position but not yet in
    // the P&L: a resume takes them at this mark (an error of about the
    // half-spread on them).
    json["mark"] =
        (mLastMark.Sign() > 0 ? mLastMark : mEntry).Normalized().ToString();
    if (auto saved = store->Save(StateKey(), json.dump()); !saved) {
        if (!mStateWarned) {
            context.Log().Warn("cannot save the state: {}",
                               saved.error().ToString());
            mStateWarned = true;
        }
        // Tried again after a pause, so a database that hangs does not hold
        // up every quote.
        mStateRetryAt = at + kStateRetryInterval;
        return;
    }
    mStateWarned = false;
    mStateSaved = at;
    mStateDirty = false;
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
    if (!mUnconfirmed.empty()) {
        ReconcileUnconfirmed(context);
    }
    mLastArrival = context.Now();
    mQuietPulled = false;
    if (mQuietLogged) {
        mQuietLogged = false;
        context.Log().Info("quotes for {} again", quote.symbol);
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
        FlowPause(context, context.Now(), pause);
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
                             UnconfirmedFills(context, Side::kBuy, now);
    const Decimal ask_room =
        mInventory - std::max(Decimal(), mStart - mParams.max_inventory) -
        UnconfirmedFills(context, Side::kSell, now);

    const Decimal away = std::max(Bps(mid, mParams.requote_bps), tick);
    const Decimal toward = std::max(
        Bps(mid, mParams.requote_toward_bps.value_or(mParams.requote_bps)),
        tick);
    // Each level a step further from the touch, sharing the side's room:
    // what the levels before it hold (or are about to) comes off first.
    // The default step is the half-spread quoted (widened by volatility,
    // with vol_multiplier).
    const Decimal step = std::max(mParams.level_step_bps.Sign() > 0
                                      ? Bps(mid, mParams.level_step_bps)
                                      : half,
                                  tick);
    // First pull any deeper level the room no longer covers, so the levels
    // placed below never rest beside it beyond max_inventory. (A partly
    // filled level replaced below can come back larger than its remainder,
    // and a deeper one then goes just after it: brief, and accepted.)
    if (mBids.size() > 1) {
        for (auto [levels, room] :
             {std::pair{&mBids, bid_room}, std::pair{&mAsks, ask_room}}) {
            Decimal left = room;
            for (Resting& level : *levels) {
                if (!level.order_id.empty() && level.Remaining() > left) {
                    const std::string id = level.order_id;
                    const std::string price = level.price.ToString();
                    if (Pull(context, level)) {
                        context.Log().Info(
                            "pulled {} quote {} @ {} (over the room left)",
                            levels == &mBids ? "buy" : "sell", id, price);
                    }
                }
                left -= level.order_id.empty() ? Size(left) : level.Remaining();
            }
        }
    }
    Decimal bid_left = bid_room;
    Decimal ask_left = ask_room;
    for (std::size_t i = 0; i < mBids.size(); ++i) {
        const Decimal back = step * Decimal::FromInt(static_cast<int>(i));
        const Decimal level_bid =
            (bid - back).RoundToIncrement(tick, Rounding::kDown);
        const Decimal level_ask =
            (ask + back).RoundToIncrement(tick, Rounding::kUp);
        const Decimal bid_size = Size(bid_left);
        const Decimal ask_size = Size(ask_left);
        Manage(context, mBids[i], Side::kBuy,
               level_bid.Sign() > 0 && !pause.bid ? std::optional(level_bid)
                                                  : std::nullopt,
               bid_size, bid_left, away, toward, *quote.bid, i == 0);
        Manage(context, mAsks[i], Side::kSell,
               pause.ask ? std::nullopt : std::optional(level_ask), ask_size,
               ask_left, away, toward, *quote.ask, i == 0);
        // The room this level takes: its resting quote, or what it would
        // have placed (also when it was not wanted: a paused side or a bid
        // at zero, where every deeper level is unwanted too).
        bid_left -= mBids[i].order_id.empty() ? bid_size : mBids[i].Remaining();
        ask_left -= mAsks[i].order_id.empty() ? ask_size : mAsks[i].Remaining();
    }
}

bool MarketMaker::PullAll(Context& context) {
    bool all = true;
    for (auto* side : {&mBids, &mAsks}) {
        for (Resting& quote : *side) {
            all = Pull(context, quote) && all;
        }
    }
    return all;
}

bool MarketMaker::AnyResting() const {
    for (const auto* side : {&mBids, &mAsks}) {
        for (const Resting& quote : *side) {
            if (!quote.order_id.empty()) {
                return true;
            }
        }
    }
    return false;
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

void MarketMaker::FlowPause(Context& context, types::Timestamp now,
                            Pause& pause) {
    const Decimal total = mFlowBought + mFlowSold;
    const Decimal imbalance =
        total.Sign() > 0 ? Decimal::Divide(mFlowBought - mFlowSold, total,
                                           kScale, Rounding::kNearest)
                         : Decimal();
    const auto note = [&](FlowPulls& pulls, bool on, const char* side) {
        if (on && !pulls.on) {
            ++pulls.count;
            pulls.since = now;
            context.Log().Debug("pulling the {} on order flow ({})", side,
                                imbalance.ToString());
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

    // Losses, measured on every quote, with the P&L of earlier runs when
    // the state is kept.
    mLastPnl = mCarried + mCash + Position() * mid;
    mLastMark = mid;
    if (!mHalted && (rules.loss_cap || rules.drawdown_cap)) {
        const Decimal pnl = mLastPnl;
        mPeak = std::max(mPeak, pnl);
        if (rules.loss_cap && pnl <= -rules.loss_cap_amount) {
            Halt(context, "loss cap");
        } else if (rules.drawdown_cap &&
                   mPeak - pnl >= rules.drawdown_cap_amount) {
            Halt(context, "drawdown cap");
        }
    }
    if (mHalted) {
        SaveState(context, mStateDirty);
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
            mStateDirty = true;
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
                context.Log().Warn(
                    "spike (3m {} bps, 5m {}, 15m {}), standing down",
                    m3.ToString(), m5.ToString(), m15.ToString());
            }
            mStandUntil = now + rules.spike_pause;
            mStateDirty = true;
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
    SaveState(context, mStateDirty);
    return pause;
}

void MarketMaker::Halt(Context& context, const char* why) {
    mHalted = true;
    mStateDirty = true;
    context.Log().Error("halted ({}), position {}", why, Position().ToString());
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
    if (!PullAll(context)) {
        return false;  // try again on the next quote
    }
    types::OrderRequest request;
    request.symbol = mParams.symbol;
    request.side = Position().Sign() > 0 ? Side::kSell : Side::kBuy;
    request.type = types::OrderType::kMarket;
    request.quantity = quantity;
    auto ack = context.Venue().PlaceOrder(request);
    if (!ack) {
        context.Log().Warn("exit ({}) failed: {}", why,
                           ack.error().ToString());
        return false;
    }
    context.Log().Warn("exiting {} {} at market ({})",
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
        context.Log().Warn("cancel of {} failed: {}", quote.order_id,
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
                         const Decimal& best, bool top) {
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
            context.Log().Info("pulled {} quote {} @ {} ({})",
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
        context.Log().Debug("post-only {} {} @ {} refused: {}",
                            types::ToString(side), size.ToString(),
                            price->ToString(), ack.error().ToString());
        return;
    }
    if (!ack) {
        context.Log().Warn("{} {} @ {} failed: {}", types::ToString(side),
                           size.ToString(), price->ToString(),
                           ack.error().ToString());
        return;
    }
    if (!types::IsActive(ack->state)) {
        // Filled or refused at once. A fill is on its way to OnFill; until it
        // arrives the inventory does not show it.
        context.Log().Info("{} {} @ {} (order {}{}) was {} at once",
                           types::ToString(side), size.ToString(),
                           price->ToString(), ack->order_id, replaced,
                           types::ToString(ack->state));
        if (ack->state == types::OrderState::kFilled) {
            ExpectFills(ack->order_id, side, size, Decimal(), context.Now(),
                        *price);
            if (top) {
                (side == Side::kBuy ? mLastBidPrice : mLastAskPrice) = *price;
            }
        }
        return;
    }
    quote =
        Resting{ack->order_id, *price, size, Decimal(), context.Now(), false};
    if (top) {
        (side == Side::kBuy ? mLastBidPrice : mLastAskPrice) = *price;
    }
    context.Log().Info("quote {} {} @ {} (order {}{})", types::ToString(side),
                       size.ToString(), price->ToString(), ack->order_id,
                       replaced);
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
    // A fill already booked unseen (ReconcileUnconfirmed) that arrives after
    // all: only what goes beyond it counts.
    if (auto it = mBookedUnseen.find(fill.order_id);
        it != mBookedUnseen.end()) {
        const Decimal skip = std::min(fill.quantity, it->second.quantity);
        it->second.quantity -= skip;
        if (it->second.quantity.Sign() <= 0) {
            mBookedUnseen.erase(it);
        }
        context.Log().Info(
            "fill {} of order {} ({}) was booked already; {} of it skipped",
            fill.fill_id, fill.order_id, fill.quantity.ToString(),
            skip.ToString());
        if (skip >= fill.quantity) {
            return;
        }
        types::Fill rest = fill;
        rest.quantity -= skip;
        ApplyFill(context, rest);
        return;
    }
    ApplyFill(context, fill);
}

void MarketMaker::ApplyFill(Context& context, const types::Fill& fill) {
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
    for (auto* side : {&mBids, &mAsks}) {
        for (Resting& quote : *side) {
            if (!quote.order_id.empty() && quote.order_id == fill.order_id) {
                quote.filled += fill.quantity;
            }
        }
    }
    if (auto it = mUnconfirmed.find(fill.order_id); it != mUnconfirmed.end()) {
        it->second.seen += fill.quantity;
        if (it->second.seen >= it->second.expected) {
            mUnconfirmed.erase(it);
        }
    }
    NoteHeld(context.Now());
    mStateDirty = true;  // saved with the next quote's mark
    context.Log().Info("{} {} @ {}, inventory {}", types::ToString(fill.side),
                       fill.quantity.ToString(), fill.price.ToString(),
                       mInventory.ToString());
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
    for (auto* side : {&mBids, &mAsks}) {
        for (Resting& quote : *side) {
            if (!quote.order_id.empty() && quote.order_id == status.order_id) {
                // Closed with fills OnFill has not delivered: keep counting
                // them.
                ExpectFills(status.order_id, status.side,
                            status.filled_quantity, quote.filled, context.Now(),
                            status.average_fill_price.value_or(quote.price));
                quote = Resting();
            }
        }
    }
}

void MarketMaker::ExpectFills(const std::string& order_id, Side side,
                              const Decimal& filled, const Decimal& seen,
                              types::Timestamp now, const Decimal& price) {
    if (filled > seen) {
        mUnconfirmed[order_id] =
            Unconfirmed{side, filled, seen, now, false, price, std::nullopt};
    }
}

void MarketMaker::ReconcileUnconfirmed(Context& context) {
    const auto now = context.Now();
    for (auto it = mBookedUnseen.begin(); it != mBookedUnseen.end();) {
        it = now - it->second.at >= kBookedUnseenKept ? mBookedUnseen.erase(it)
                                                      : std::next(it);
    }
    // At most one venue call per quote, so a slow venue cannot hold up
    // quoting for long.
    bool asked = false;
    for (auto it = mUnconfirmed.begin(); it != mUnconfirmed.end() && !asked;) {
        auto& [order_id, pending] = *it;
        if (now - pending.since < kReconcileAfter ||
            (pending.checked && now - *pending.checked < kReconcileAfter)) {
            ++it;
            continue;
        }
        pending.checked = now;
        asked = true;
        auto status = context.Venue().GetOrder(order_id);
        if (!status) {
            // Without the venue's word nothing is booked: the side keeps the
            // room for it, as before.
            if (!pending.lookup_warned) {
                pending.lookup_warned = true;
                context.Log().Warn(
                    "cannot check order {} with the venue ({}); its side keeps "
                    "the room for it",
                    order_id, status.error().ToString());
            }
            ++it;
            continue;
        }
        if (status->filled_quantity < pending.expected) {
            // The acknowledgement assumed the size asked for; the venue
            // filled less (rounded, or a partial fill).
            context.Log().Warn("order {} filled {}, not the {} assumed",
                               order_id, status->filled_quantity.ToString(),
                               pending.expected.ToString());
            pending.expected = status->filled_quantity;
        }
        if (status->average_fill_price) {
            pending.price = *status->average_fill_price;
        }
        if (pending.expected <= pending.seen) {
            it = mUnconfirmed.erase(it);  // all of it has arrived
            continue;
        }
        if (now - pending.since < kBookUnseenAfter) {
            ++it;
            continue;
        }
        // Long past when the runner would deliver it: book it here, and
        // skip a copy that arrives after all.
        const Decimal missing = pending.expected - pending.seen;
        context.Log().Warn(
            "order {} filled {} but only {} arrived in {} s; booking the {} "
            "missing at {}",
            order_id, pending.expected.ToString(), pending.seen.ToString(),
            std::chrono::duration_cast<std::chrono::seconds>(now -
                                                             pending.since)
                .count(),
            missing.ToString(), pending.price.ToString());
        // Booked without a fee, which the venue's record of it would carry
        // (a fee in the coin then shows as a little more held than the
        // venue has).
        types::Fill fill;
        fill.fill_id = "unseen-" + order_id;
        fill.order_id = order_id;
        fill.symbol = mParams.symbol;
        fill.side = pending.side;
        fill.quantity = missing;
        fill.price = pending.price;
        fill.time = now;
        auto& booked = mBookedUnseen[order_id];
        booked.quantity += missing;
        booked.at = now;
        it = mUnconfirmed.erase(it);
        ApplyFill(context, fill);
    }
}

Decimal MarketMaker::UnconfirmedFills(Context& context, Side side,
                                      types::Timestamp now) {
    Decimal total;
    for (auto& [order_id, pending] : mUnconfirmed) {
        if (pending.side != side) {
            continue;
        }
        total += pending.expected - pending.seen;
        // Kept until the fill arrives, or the venue says it will not
        // (ReconcileUnconfirmed): until then that side is short of room
        // rather than over the limit.
        if (!pending.warned && now - pending.since >= kUnconfirmedWarning) {
            pending.warned = true;
            context.Log().Warn(
                "order {} filled {} but only {} has arrived after {} s; that "
                "side keeps the room for it",
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
    if (AnyResting() && !mQuietLogged) {
        mQuietLogged = true;
        context.Log().Warn(
            "no quote for {} for {} ms; pulling the quotes", mParams.symbol,
            std::chrono::duration_cast<std::chrono::milliseconds>(quiet)
                .count());
    }
    // A side that cannot be pulled yet (a cancel lost to a fill) is tried
    // again on the next tick.
    mQuietPulled = PullAll(context);
}

void MarketMaker::OnStop(Context& context) {
    SaveState(context, true);
    if (mHalfCount > 0) {
        context.Log().Info(
            "half-spread {:.2f} bps on average, {} at most, widened by "
            "volatility on {:.1f}% of quotes",
            mHalfSum / static_cast<double>(mHalfCount),
            mHalfMax.Normalized().ToString(),
            100.0 * static_cast<double>(mHalfWidened) /
                static_cast<double>(mHalfCount));
    }
    if (mCentreCount > 0) {
        context.Log().Info(
            "the microprice moved the centre {:.2f} bps from the mid on "
            "average",
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
        context.Log().Info(
            "order flow pulled the bid {} time(s) ({:.1f}% of the run) and the "
            "ask {} ({:.1f}%); {} trade(s) without a side or size not counted",
            mFlowBid.count, share(mFlowBid), mFlowAsk.count, share(mFlowAsk),
            mFlowUncounted);
    }
    if (mWouldTake > 0) {
        context.Log().Info(
            "{} post-only quote(s) refused as they would have taken liquidity",
            mWouldTake);
    }
    std::vector<Resting*> quotes;
    for (auto* side : {&mBids, &mAsks}) {
        for (Resting& quote : *side) {
            quotes.push_back(&quote);
        }
    }
    for (Resting* quote : quotes) {
        if (!quote->order_id.empty() && !quote->cancel_raced) {
            auto outcome = context.Venue().CancelOrder(quote->order_id);
            if (outcome) {
                context.Log().Info(
                    "stopping, {} {} @ {}",
                    *outcome == trading::CancelOutcome::kCancelled
                        ? "cancelled"
                        : "already closed:",
                    quote->order_id, quote->price.ToString());
            } else {
                context.Log().Warn("stopping, cancel of {} failed: {}",
                                   quote->order_id,
                                   outcome.error().ToString());
            }
        }
    }
}

}  // namespace kestrel::strategy
