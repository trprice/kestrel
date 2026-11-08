#include "kestrel/strategy/runner.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>

#include "kestrel/data/recent_bars.hpp"
#include "kestrel/strategy/run_recorder.hpp"
#include "kestrel/trading/session_gated_venue.hpp"

namespace kestrel::strategy {

using types::Decimal;
using types::ErrorCategory;
using types::MakeError;

namespace {

// What a market order should be valued at: the midpoint when both sides of the
// book are known, otherwise whichever price the quote has.
std::optional<Decimal> QuotePrice(const types::Quote& quote) {
    if (quote.bid && quote.ask) {
        return Decimal::Divide(*quote.bid + *quote.ask, Decimal::FromInt(2), 8,
                               types::Rounding::kNearest);
    }
    if (quote.last) {
        return quote.last;
    }
    return quote.ask ? quote.ask : quote.bid;
}

}  // namespace

// Passes everything to the venue it wraps and remembers the orders that were
// still active when acknowledged.
class OrderTracker : public trading::TradingVenue {
   public:
    struct Seen {
        types::OrderState state{};
        Decimal filled;
    };

    explicit OrderTracker(std::shared_ptr<trading::TradingVenue> venue)
        : mVenue(std::move(venue)) {}

    std::string_view Name() const override { return mVenue->Name(); }
    types::Result<types::OrderAcknowledgement> PlaceOrder(
        const types::OrderRequest& request) override {
        auto ack = mVenue->PlaceOrder(request);
        if (ack && types::IsActive(ack->state)) {
            std::lock_guard lock(mMutex);
            mActive[ack->order_id] = Seen{ack->state, Decimal()};
        }
        return ack;
    }
    types::Result<trading::CancelOutcome> CancelOrder(
        const std::string& order_id) override {
        return mVenue->CancelOrder(order_id);
    }
    types::Result<std::vector<std::string>> CancelAll() override {
        return mVenue->CancelAll();
    }
    types::Result<std::vector<types::OrderStatus>> GetOpenOrders() override {
        return mVenue->GetOpenOrders();
    }
    types::Result<types::OrderStatus> GetOrder(
        const std::string& order_id) override {
        return mVenue->GetOrder(order_id);
    }
    types::Result<std::vector<types::Fill>> GetFills(
        types::Timestamp since) override {
        return mVenue->GetFills(since);
    }
    types::Result<std::vector<types::Position>> GetPositions() override {
        return mVenue->GetPositions();
    }
    types::Result<std::vector<types::Balance>> GetBalances() override {
        return mVenue->GetBalances();
    }
    types::Result<std::unique_ptr<data::Subscription>> StreamExecutions(
        trading::ExecutionCallback callback) override {
        return mVenue->StreamExecutions(std::move(callback));
    }

    // Whether `status` is ahead of what was last seen for a tracked order;
    // changes nothing.
    bool Changed(const types::OrderStatus& status) const {
        std::lock_guard lock(mMutex);
        auto it = mActive.find(status.order_id);
        return it != mActive.end() && Ahead(it->second, status);
    }
    // The orders last seen active, by venue order ID.
    std::map<std::string, Seen> Active() const {
        std::lock_guard lock(mMutex);
        return mActive;
    }
    // Records `status` for a tracked order and returns whether it is ahead of
    // what was last seen; a closed order is forgotten. A status behind the
    // last one seen (a poll's snapshot overtaken by a pushed update while it
    // was queued) is ignored, so the strategy never sees an order go back.
    bool Note(const types::OrderStatus& status) {
        std::lock_guard lock(mMutex);
        auto it = mActive.find(status.order_id);
        if (it == mActive.end() || !Ahead(it->second, status)) {
            return false;
        }
        if (types::IsActive(status.state)) {
            it->second = Seen{status.state, status.filled_quantity};
        } else {
            mActive.erase(it);
        }
        return true;
    }
    void Forget(const std::string& order_id) {
        std::lock_guard lock(mMutex);
        mActive.erase(order_id);
    }

   private:
    // An order only moves forward: more filled, or from new to partially
    // filled, or closed. A closed status is always ahead (it is final, and a
    // closed order is no longer tracked).
    static bool Ahead(const Seen& seen, const types::OrderStatus& status) {
        if (!types::IsActive(status.state)) {
            return true;
        }
        if (status.filled_quantity != seen.filled) {
            return status.filled_quantity > seen.filled;
        }
        return status.state == types::OrderState::kPartiallyFilled &&
               seen.state == types::OrderState::kNew;
    }

    const std::shared_ptr<trading::TradingVenue> mVenue;
    mutable std::mutex mMutex;
    std::map<std::string, Seen> mActive;
};

Runner::Runner(std::shared_ptr<data::DataSource> data_source,
               std::shared_ptr<trading::TradingVenue> venue,
               std::unique_ptr<Strategy> strategy, RunnerOptions options,
               std::function<types::Timestamp()> clock)
    : mData(std::move(data_source)),
      mHours(mData->Hours()),
      mVenue(std::move(venue)),
      mStrategy(std::move(strategy)),
      mOptions(std::move(options)),
      mClock(std::move(clock)),
      mName(mStrategy->Name()) {
    if (mOptions.risk) {
        mGate = std::make_shared<trading::RiskGatedVenue>(
            mVenue, *mOptions.risk, mClock);
        mVenue = mGate;
    }
    if (!mHours->IsAlwaysOpen()) {
        mVenue = std::make_shared<trading::SessionGatedVenue>(mVenue, mHours,
                                                              mClock);
    }
    mTracker = std::make_shared<OrderTracker>(mVenue);
    mVenue = mTracker;
    if (mOptions.trade_log) {
        mRecorder = std::make_unique<RunRecorder>(mOptions.trade_log,
                                                  mOptions.pnl_interval);
        mRecorder->RecordLatency(mOptions.latency_interval,
                                 mOptions.latency_source);
    }
}

Runner::~Runner() { StopPoller(); }

void Runner::Push(Event event) {
    {
        std::lock_guard lock(mMutex);
        mEvents.push_back(std::move(event));
    }
    mWake.notify_one();
}

void Runner::PushQuote(const types::Quote& quote) {
    {
        std::lock_guard lock(mMutex);
        auto [it, inserted] = mPendingQuotes.try_emplace(quote.symbol, quote);
        if (!inserted) {
            it->second.MergeFrom(quote);
            ++mConflated;
            return;  // the queued event will carry the merged quote
        }
        mEvents.push_back(QuoteEvent{quote.symbol, {}});
    }
    mWake.notify_one();
}

void Runner::PushBar(BarEvent event) {
    std::size_t recovered = 0;
    bool dropped = false;
    bool first_drop = false;
    {
        std::lock_guard lock(mMutex);
        if (mOptions.max_queued_bars > 0 &&
            mQueuedBars >= mOptions.max_queued_bars) {
            ++mDroppedBars;
            dropped = true;
            first_drop = mDropping++ == 0;
        } else {
            recovered = std::exchange(mDropping, 0);
            mEvents.push_back(std::move(event));
            ++mQueuedBars;
        }
    }
    // Logged outside the lock, which the strategy thread waits on.
    if (dropped) {
        if (first_drop) {
            spdlog::warn(
                "strategy {}: {} bars queued, the limit: dropping new ones "
                "until it catches up (the gap is backfilled)",
                mName, mOptions.max_queued_bars);
        }
        return;
    }
    if (recovered > 0) {
        spdlog::info("strategy {}: bar queue has room again after dropping {}",
                     mName, recovered);
    }
    mWake.notify_one();
}

std::size_t Runner::DroppedBars() const {
    std::lock_guard lock(mMutex);
    return mDroppedBars;
}

Runner::Event Runner::TakeFront() {
    Event event = std::move(mEvents.front());
    mEvents.pop_front();
    if (std::holds_alternative<BarEvent>(event)) {
        --mQueuedBars;
    }
    if (auto* quote = std::get_if<QuoteEvent>(&event)) {
        auto pending = mPendingQuotes.extract(quote->symbol);
        quote->quote = std::move(pending.mapped());
    }
    return event;
}

std::size_t Runner::ConflatedQuotes() const {
    std::lock_guard lock(mMutex);
    return mConflated;
}

std::optional<Runner::Event> Runner::Pop(std::optional<SteadyTime> deadline) {
    std::unique_lock lock(mMutex);
    auto ready = [this] { return !mEvents.empty(); };
    if (deadline) {
        mWake.wait_until(lock, *deadline, ready);
    } else {
        mWake.wait(lock, ready);
    }
    if (mEvents.empty()) {
        return std::nullopt;
    }
    return TakeFront();
}

std::optional<Runner::Event> Runner::TryPop() {
    std::lock_guard lock(mMutex);
    if (mEvents.empty()) {
        return std::nullopt;
    }
    return TakeFront();
}

void Runner::Stop() { Push(StopEvent{}); }

std::span<const types::Bar> Runner::Bars(const types::Symbol& symbol,
                                         types::Timeframe timeframe) const {
    auto it = mBars.find({symbol, timeframe});
    if (it == mBars.end()) {
        return {};
    }
    return it->second;
}

void Runner::Guarded(const char* hook, const std::function<void()>& call) {
    try {
        call();
    } catch (const std::exception& e) {
        spdlog::error("strategy {}: {} threw: {}", mStrategy->Name(), hook,
                      e.what());
    } catch (...) {
        spdlog::error("strategy {}: {} threw a non-standard exception",
                      mStrategy->Name(), hook);
    }
}

types::Result<void> Runner::Begin() {
    const Requirements requirements = mStrategy->Needs();
    if (requirements.quotes) {
        for (const auto& symbol : requirements.symbols) {
            mLastQuote[symbol] = mClock();
        }
    }
    if (requirements.symbols.empty()) {
        return MakeError(
            ErrorCategory::kInvalidArgument,
            "strategy " + std::string(mStrategy->Name()) + " needs no symbols");
    }
    if (!requirements.quotes && requirements.bar_timeframes.empty()) {
        return MakeError(ErrorCategory::kInvalidArgument,
                         "strategy " + std::string(mStrategy->Name()) +
                             " subscribes to neither quotes nor bars");
    }
    mWindowSize = std::max<std::size_t>(requirements.warmup_bars, 1);

    // Each symbol's info, now rather than at its first order: an unknown
    // symbol stops the run before it starts (its bars and quotes would just
    // never come), and the orders do not wait on the request. Another
    // failure (a timeout, say) only warns; the first order asks again.
    for (const auto& symbol : requirements.symbols) {
        auto info = mData->GetSymbolInfo(symbol);
        if (!info && info.error().category == ErrorCategory::kNotFound) {
            types::Error error = info.error();
            error.message = "strategy " + std::string(mStrategy->Name()) +
                            ": " + symbol + ": " + error.message;
            return std::unexpected(std::move(error));
        }
        if (!info) {
            spdlog::warn("strategy {}: no symbol info for {} yet: {}",
                         mStrategy->Name(), symbol, info.error().ToString());
        }
    }

    // Orders left by an earlier run, or placed by hand, would hold funds and
    // could fill without the strategy or the risk gate knowing: the runner
    // starts from an account with nothing open.
    auto cancelled = mVenue->CancelAll();
    if (!cancelled) {
        types::Error error = cancelled.error();
        error.message = "cannot start with orders possibly left open: " +
                        error.message;
        return std::unexpected(std::move(error));
    }
    if (!cancelled->empty()) {
        std::string ids;
        for (const auto& id : *cancelled) {
            ids += ' ' + id;
        }
        spdlog::warn("strategy {}: cancelled {} order(s) already open:{}",
                     mStrategy->Name(), cancelled->size(), ids);
    }

    // Subscribe before the warmup fetch so a bar that closes in between is
    // queued rather than lost; DeliverBar drops the overlap.
    for (types::Timeframe timeframe : requirements.bar_timeframes) {
        auto sub = mData->StreamBars(
            requirements.symbols, timeframe,
            [this](const types::Symbol& symbol, types::Timeframe tf,
                   const types::Bar& bar) {
                PushBar(BarEvent{symbol, tf, bar});
            });
        if (!sub) {
            mSubscriptions.clear();
            return std::unexpected(sub.error());
        }
        mSubscriptions.push_back(std::move(*sub));
    }
    if (requirements.quotes) {
        auto sub = mData->StreamQuotes(
            requirements.symbols,
            [this](const types::Quote& quote) { PushQuote(quote); });
        if (!sub) {
            mSubscriptions.clear();
            return std::unexpected(sub.error());
        }
        mSubscriptions.push_back(std::move(*sub));
    }

    auto ready = Warmup(requirements);
    if (ready && mGate) {
        // Losses are counted from here; fails closed if positions can't be
        // read.
        ready = mGate->ResetDailyBaseline();
    }
    if (!ready) {
        mSubscriptions.clear();
        return ready;
    }

    mRunStart = mFillCursor = mClock();
    mPublishedCursor = mFillCursor.time_since_epoch().count();
    mSeenFills.clear();
    SubscribeExecutions();
    if (mRecorder) {
        // The venue's holdings are the cost basis. Without them a sale of
        // what was held at the start would realize nothing.
        auto positions = mVenue->GetPositions();
        if (!positions) {
            spdlog::warn(
                "strategy {}: recording P&L without the starting positions: {}",
                mStrategy->Name(), positions.error().ToString());
        }
        mRecorder->Start(
            positions ? *positions : std::vector<types::Position>{}, mClock());
    }
    mBegun = true;
    Guarded("OnStart", [this] { mStrategy->OnStart(*this); });
    StartSessions(mClock());
    mTimerInterval = requirements.timer_interval;
    if (mTimerInterval > std::chrono::milliseconds::zero()) {
        mNextTimer = mClock() + mTimerInterval;
    }
    return {};
}

void Runner::CheckTimer(types::Timestamp now) {
    if (!mNextTimer) {
        return;
    }
    if (*mNextTimer - now > mTimerInterval) {
        // The clock stepped back (NTP, a resumed VM): due an interval from
        // now, not when the clock catches up.
        mNextTimer = now + mTimerInterval;
    }
    if (now < *mNextTimer) {
        return;
    }
    Guarded("OnTimer", [this] { mStrategy->OnTimer(*this); });
    mNextTimer = mClock() + mTimerInterval;  // an interval after it ran
}

void Runner::StartSessions(types::Timestamp now) {
    if (mHours->IsAlwaysOpen()) {
        return;
    }
    mSession = mHours->CurrentOrNext(now);
    mOpenCalled = mSession && now >= mSession->start;
    mClosingCalled = false;
    CheckSession(now);  // in the closing window already: OnSessionClosing
}

void Runner::RefreshSession(types::Timestamp now) {
    if (!mSession || now >= mSession->end) {
        return;
    }
    // The calendar can learn of a restriction after the session was read
    // (Schwab's answer lands after the rules': an unscheduled early close, a
    // delayed open, a closed day). Answers only ever restrict, so adopt them.
    const auto fresh = mHours->CurrentOrNext(now);
    if (fresh && fresh->start < mSession->end) {
        // The same session (sessions never overlap), perhaps shorter.
        if (!mOpenCalled) {
            mSession->start = std::max(mSession->start, fresh->start);
        }
        mSession->end = std::min(mSession->end, fresh->end);
    } else {
        // It is gone, or already over: move on below.
        mSession->end = now;
    }
}

void Runner::CheckSession(types::Timestamp now) {
    RefreshSession(now);
    while (mSession) {
        if (now >= mSession->end) {
            // Over (a session the runner slept through gets no hooks).
            mSession = mHours->CurrentOrNext(mSession->end);
            mOpenCalled = false;
            mClosingCalled = false;
            continue;
        }
        if (!mOpenCalled && now >= mSession->start) {
            mOpenCalled = true;
            Guarded("OnSessionOpen",
                    [this] { mStrategy->OnSessionOpen(*this); });
        }
        if (mOpenCalled && !mClosingCalled && now >= mSession->end - Lead()) {
            mClosingCalled = true;
            const auto close = mSession->end;
            Guarded("OnSessionClosing", [this, close] {
                mStrategy->OnSessionClosing(*this, close);
            });
        }
        return;
    }
}

void Runner::NoteQuote(const types::Symbol& symbol, types::Timestamp now) {
    const auto it = mLastQuote.find(symbol);
    if (it == mLastQuote.end()) {
        return;
    }
    if (mStaleQuotes.erase(symbol) > 0) {
        spdlog::info(
            "strategy {}: quotes for {} again, after {} s of open market",
            mName, symbol,
            std::chrono::duration_cast<std::chrono::seconds>(
                mHours->OpenDuration(it->second, now))
                .count());
    }
    it->second = now;
}

void Runner::CheckQuoteFeeds(types::Timestamp now) {
    if (mLastQuote.empty() ||
        mOptions.stale_quote_warning <= std::chrono::milliseconds::zero() ||
        !mHours->IsOpen(now)) {
        return;
    }
    // Only open time counts: quiet from the close to the open is normal.
    for (const auto& [symbol, last] : mLastQuote) {
        const auto quiet = mHours->OpenDuration(last, now);
        if (quiet >= mOptions.stale_quote_warning &&
            mStaleQuotes.insert(symbol).second) {
            spdlog::warn(
                "strategy {}: no quote for {} for {} s of open market; the "
                "feed may be stale",
                mName, symbol,
                std::chrono::duration_cast<std::chrono::seconds>(quiet)
                    .count());
        }
    }
}

std::optional<types::Timestamp> Runner::NextSessionEvent() const {
    if (!mSession) {
        return std::nullopt;
    }
    if (!mOpenCalled) {
        return mSession->start;
    }
    if (!mClosingCalled) {
        return std::max(mSession->start, mSession->end - Lead());
    }
    return mSession->end;  // then the next session is found
}

EntryCancels EntriesToCancel(
    const std::vector<types::OrderStatus>& open,
    const std::vector<types::Symbol>& symbols,
    const std::optional<std::vector<types::Position>>& positions,
    const std::optional<std::vector<types::Balance>>& balances) {
    EntryCancels result;
    std::vector<types::Symbol> seen;
    for (const types::Symbol& symbol : symbols) {
        if (std::ranges::find(seen, symbol) != seen.end()) {
            continue;
        }
        seen.push_back(symbol);
        std::vector<const types::OrderStatus*> orders;
        for (const auto& order : open) {
            if (order.symbol == symbol) {
                orders.push_back(&order);
            }
        }
        // Held: the venue's position, or else the base asset held.
        std::optional<Decimal> held;
        if (positions && balances) {
            held = Decimal();
            bool found = false;
            for (const auto& position : *positions) {
                if (position.symbol == symbol) {
                    held = position.quantity;
                    found = true;
                }
            }
            if (!found) {
                if (const auto parts = types::SplitSymbol(symbol)) {
                    for (const auto& balance : *balances) {
                        if (balance.asset == parts->base) {
                            held = balance.total;
                        }
                    }
                } else {
                    held.reset();  // unknown, not flat
                }
            }
        }
        // Stops first (the protection), then oldest first.
        std::ranges::stable_sort(orders, [](const types::OrderStatus* a,
                                            const types::OrderStatus* b) {
            const bool a_stop = a->type == types::OrderType::kStop;
            const bool b_stop = b->type == types::OrderType::kStop;
            return a_stop != b_stop ? a_stop
                                    : a->created_time < b->created_time;
        });
        Decimal room = held ? held->Abs() : Decimal();
        const auto reducing = !held              ? types::Side::kSell
                              : held->Sign() < 0 ? types::Side::kBuy
                                                 : types::Side::kSell;
        bool closing = false;
        for (const types::OrderStatus* order : orders) {
            const Decimal remaining = order->quantity - order->filled_quantity;
            bool keep = false;
            if (order->side == reducing && (!held || held->Sign() != 0)) {
                if (!held) {
                    keep = true;
                } else if (remaining <= room) {
                    keep = true;
                    room = room - remaining;
                }
            }
            if (keep) {
                closing = true;
            } else {
                result.ids.push_back(order->order_id);
            }
        }
        if (held && held->Sign() != 0 && !closing) {
            result.unprotected.push_back(symbol);
        }
    }
    return result;
}

void Runner::CancelEntries() {
    const auto symbols = mStrategy->Needs().symbols;
    auto open = mVenue->GetOpenOrders();
    if (!open) {
        spdlog::error(
            "strategy {}: could not read open orders to cancel entries on "
            "stop; any left open stay open: {}",
            mName, open.error().ToString());
        return;
    }
    std::optional<std::vector<types::Position>> positions;
    std::optional<std::vector<types::Balance>> balances;
    auto fetched_positions = mVenue->GetPositions();
    auto fetched_balances = mVenue->GetBalances();
    if (fetched_positions && fetched_balances) {
        positions = std::move(*fetched_positions);
        balances = std::move(*fetched_balances);
    } else {
        spdlog::warn(
            "strategy {}: could not read holdings on stop; keeping open sells "
            "and cancelling open buys: {}",
            mName,
            (fetched_positions ? fetched_balances.error()
                               : fetched_positions.error())
                .ToString());
    }
    const auto plan = EntriesToCancel(*open, symbols, positions, balances);
    const auto& ids = plan.ids;
    std::size_t kept = 0;
    for (const auto& order : *open) {
        if (std::ranges::find(symbols, order.symbol) != symbols.end() &&
            std::ranges::find(ids, order.order_id) == ids.end()) {
            ++kept;
            spdlog::info("strategy {}: left open on stop: {} {} {} {}", mName,
                         order.order_id, order.symbol,
                         order.side == types::Side::kBuy ? "buy" : "sell",
                         (order.quantity - order.filled_quantity).ToString());
        }
    }
    std::size_t cancelled = 0;
    for (const auto& id : ids) {
        if (auto outcome = mVenue->CancelOrder(id); !outcome) {
            spdlog::error("strategy {}: could not cancel order {} on stop: {}",
                          mName, id, outcome.error().ToString());
        } else if (*outcome == trading::CancelOutcome::kCancelled) {
            ++cancelled;
        } else if (*outcome == trading::CancelOutcome::kAlreadyFilled) {
            spdlog::warn(
                "strategy {}: order {} filled before it could be cancelled on "
                "stop; the strategy hears of the fill before it stops",
                mName, id);
        }
    }
    for (const auto& symbol : plan.unprotected) {
        spdlog::warn(
            "strategy {}: {} is held with no order left open to close it",
            mName, symbol);
    }
    if (cancelled > 0 || kept > 0) {
        spdlog::info(
            "strategy {}: on stop, cancelled {} entry order(s), left {} "
            "closing order(s) open",
            mName, cancelled, kept);
    }
}

void Runner::End() {
    if (!mBegun) {
        return;
    }
    mBegun = false;
    // Nothing is queued by the poller from here on. A poll it queued behind
    // the StopEvent is never handled: its fills are found again by the poll
    // below (only with a recorder or the entry cancel), its order changes
    // are dropped, since the strategy is stopping.
    StopPoller();
    // Entries are cancelled before the last poll, so an entry that filled
    // first still reaches OnFill, where a strategy places its protective
    // stop.
    if (mOptions.cancel_entries_on_stop) {
        Guarded("the entry cancel", [this] { CancelEntries(); });
    }
    if (mRecorder || mOptions.cancel_entries_on_stop) {
        PollFills();  // fills that landed since the last poll
    }
    // Stop the streams first so nothing is enqueued after the last hook.
    for (auto& sub : mSubscriptions) {
        sub->Stop();
    }
    mSubscriptions.clear();
    Guarded("OnStop", [this] { mStrategy->OnStop(*this); });
    if (mRecorder) {
        mRecorder->Finish(mClock());
    }
}

types::Result<void> Runner::Run() {
    if (auto begun = Begin(); !begun) {
        return begun;
    }

    const auto interval =
        mPushed ? mOptions.fill_backstop_interval : mOptions.fill_poll_interval;
    const bool polling = interval > std::chrono::milliseconds::zero();
    if (polling) {
        StartPoller(interval);
    }
    for (bool running = true; running;) {
        // The deadline paces the recorder's ticks when nothing arrives, and
        // wakes the loop for the next session hook.
        const auto steady = std::chrono::steady_clock::now();
        std::optional<SteadyTime> deadline;
        if (polling) {
            deadline = steady + interval;
        }
        if (!mLastQuote.empty() &&
            mOptions.stale_quote_warning > std::chrono::milliseconds::zero()) {
            // Often enough to notice a feed gone quiet with nothing else
            // arriving.
            const SteadyTime check = steady + std::chrono::seconds(10);
            deadline = deadline ? std::min(*deadline, check) : check;
        }
        if (const auto due = NextSessionEvent()) {
            // At least every minute, so a session the calendar shortens
            // meanwhile is seen (CheckSession re-reads it).
            const auto wait = std::clamp<types::Timestamp::duration>(
                *due - mClock(), types::Timestamp::duration::zero(),
                std::chrono::minutes(1));
            const SteadyTime at = steady + wait;
            deadline = deadline ? std::min(*deadline, at) : at;
        }
        if (mNextTimer) {
            // At most an interval, so a clock that steps back cannot stall
            // the timer (CheckTimer re-arms it).
            const auto wait = std::clamp<types::Timestamp::duration>(
                *mNextTimer - mClock(), types::Timestamp::duration::zero(),
                mTimerInterval);
            const SteadyTime at = steady + wait;
            deadline = deadline ? std::min(*deadline, at) : at;
        }
        auto event = Pop(deadline);
        CheckSession(mClock());
        CheckQuoteFeeds(mClock());
        CheckTimer(mClock());
        if (event) {
            running = Handle(*event);
        }
        if (mRecorder) {
            mRecorder->Tick(mClock());
        }
    }

    End();
    return {};
}

bool Runner::Drain() {
    CheckSession(mClock());
    CheckTimer(mClock());
    do {
        while (auto event = TryPop()) {
            if (!Handle(*event)) {
                return false;
            }
        }
    } while (Poll() > 0);
    return true;
}

void Runner::SubscribeExecutions() {
    mPushed = false;
    auto sub = mVenue->StreamExecutions(
        [this](const trading::ExecutionUpdate& update) {
            Push(ExecutionEvent{update});
        });
    if (!sub) {
        if (sub.error().category != ErrorCategory::kNotImplemented) {
            spdlog::warn("strategy {}: no pushed executions, polling: {}",
                         mStrategy->Name(), sub.error().ToString());
        }
        return;
    }
    mPushed = true;
    mSubscriptions.push_back(std::move(*sub));
}

void Runner::HandleExecution(const trading::ExecutionUpdate& update) {
    if (update.fill) {
        DeliverFill(*update.fill);
        if (mRecorder) {
            mRecorder->Flush();
        }
    }
    if (update.order && mTracker->Note(*update.order)) {
        Guarded("OnOrderUpdate",
                [&] { mStrategy->OnOrderUpdate(*this, *update.order); });
    }
}

bool Runner::Handle(const Event& event) {
    if (const auto* bar = std::get_if<BarEvent>(&event)) {
        HandleBar(*bar);
    } else if (const auto* execution = std::get_if<ExecutionEvent>(&event)) {
        HandleExecution(execution->update);
    } else if (const auto* polled = std::get_if<PolledEvent>(&event)) {
        DeliverPolled(*polled);
    } else if (const auto* quote = std::get_if<QuoteEvent>(&event)) {
        NoteQuote(quote->quote.symbol, mClock());
        if (mGate || mRecorder) {
            if (auto price = QuotePrice(quote->quote)) {
                if (mGate) {
                    mGate->NoteReferencePrice(quote->quote.symbol, *price);
                }
                if (mRecorder) {
                    mRecorder->NotePrice(quote->quote.symbol, *price);
                }
            }
        }
        Guarded("OnQuote", [&] { mStrategy->OnQuote(*this, quote->quote); });
    } else {
        return false;
    }
    return true;
}

types::Result<void> Runner::Warmup(const Requirements& requirements) {
    const types::Timestamp now = mClock();
    for (types::Timeframe timeframe : requirements.bar_timeframes) {
        for (const types::Symbol& symbol : requirements.symbols) {
            auto& window = mBars[{symbol, timeframe}];
            if (requirements.warmup_bars == 0) {
                continue;
            }
            // The newest bars, however much closed time lies between them.
            auto bars = data::RecentBars(*mData, symbol, timeframe,
                                         requirements.warmup_bars, now);
            if (!bars) {
                return std::unexpected(bars.error());
            }
            window.assign(bars->begin(), bars->end());
            if (!window.empty()) {
                if (mGate) {
                    mGate->NoteReferencePrice(symbol, window.back().close);
                }
                if (mRecorder) {
                    mRecorder->NotePrice(symbol, window.back().close);
                }
            }
        }
    }
    return {};
}

void Runner::HandleBar(const BarEvent& event) {
    const auto step = types::FixedDuration(event.timeframe);
    const auto& window = mBars[{event.symbol, event.timeframe}];
    if (step && !window.empty()) {
        const types::Timestamp expected = window.back().open_time + *step;
        // A gap that is all closed time (a night, a weekend, a holiday) has
        // no bars to fetch.
        if (event.bar.open_time > expected &&
            mHours->AnyOpen(expected, event.bar.open_time)) {
            // Bars that closed while the stream was down are not replayed.
            auto gap = mData->GetBars(event.symbol, event.timeframe, expected,
                                      event.bar.open_time);
            if (gap) {
                for (const types::Bar& bar : *gap) {
                    DeliverBar(event.symbol, event.timeframe, bar);
                }
            } else {
                spdlog::warn("strategy {}: could not backfill {} {} bars: {}",
                             mStrategy->Name(), event.symbol,
                             types::ToString(event.timeframe),
                             gap.error().ToString());
            }
        }
    }
    DeliverBar(event.symbol, event.timeframe, event.bar);
}

void Runner::DeliverBar(const types::Symbol& symbol, types::Timeframe timeframe,
                        const types::Bar& bar) {
    auto& window = mBars[{symbol, timeframe}];
    if (!window.empty() && bar.open_time <= window.back().open_time) {
        return;
    }
    window.push_back(bar);
    if (window.size() > mWindowSize) {
        window.erase(window.begin());
    }
    if (mGate) {
        mGate->NoteReferencePrice(symbol, bar.close);
    }
    if (mRecorder) {
        mRecorder->NotePrice(symbol, bar.close);
    }
    Guarded("OnBar", [&] { mStrategy->OnBar(*this, symbol, timeframe, bar); });
}

std::size_t Runner::Poll() {
    const std::size_t fills = PollFills();
    return fills + DeliverPolled(PolledEvent{{}, FetchOrders()});
}

std::size_t Runner::PollFills() {
    return DeliverPolled(PolledEvent{FetchFills(), {}});
}

void Runner::StartPoller(std::chrono::milliseconds interval) {
    {
        std::lock_guard lock(mPollerMutex);
        mPollerStop = false;
    }
    mPoller = std::thread([this, interval] {
        std::unique_lock lock(mPollerMutex);
        while (!mPollerWake.wait_for(lock, interval,
                                     [this] { return mPollerStop; })) {
            lock.unlock();
            // Venues return errors rather than throw, but an exception here
            // would end the process: log it and poll again next time.
            try {
                PolledEvent polled{FetchFills(), FetchOrders()};
                if (!polled.fills.empty() || !polled.orders.empty()) {
                    Push(std::move(polled));
                }
            } catch (const std::exception& e) {
                spdlog::error("strategy {}: poll threw: {}", mName, e.what());
            } catch (...) {
                spdlog::error("strategy {}: poll threw a non-standard exception",
                              mName);
            }
            lock.lock();
        }
    });
}

void Runner::StopPoller() {
    if (!mPoller.joinable()) {
        return;
    }
    {
        std::lock_guard lock(mPollerMutex);
        mPollerStop = true;
    }
    mPollerWake.notify_one();
    mPoller.join();  // waits out a poll in flight
}

std::vector<types::OrderStatus> Runner::FetchOrders() {
    const auto tracked = mTracker->Active();
    if (tracked.empty()) {
        return {};
    }
    auto open = mVenue->GetOpenOrders();
    if (!open) {
        spdlog::warn("strategy {}: could not read open orders: {}", mName,
                     open.error().ToString());
        return {};
    }
    std::map<std::string, const types::OrderStatus*> open_by_id;
    for (const types::OrderStatus& status : *open) {
        open_by_id[status.order_id] = &status;
    }

    std::vector<types::OrderStatus> changed;
    for (const auto& entry : tracked) {
        const std::string& order_id = entry.first;
        types::OrderStatus status;
        if (auto it = open_by_id.find(order_id); it != open_by_id.end()) {
            status = *it->second;
        } else {
            // Not open any more: ask what became of it.
            auto closed = mVenue->GetOrder(order_id);
            if (!closed) {
                spdlog::warn("strategy {}: could not look up order {}: {}",
                             mName, order_id, closed.error().ToString());
                if (closed.error().category == ErrorCategory::kNotFound) {
                    mTracker->Forget(order_id);  // it will never be found
                }
                continue;
            }
            status = std::move(*closed);
        }
        // Checked again on delivery, where the tracker is updated.
        if (mTracker->Changed(status)) {
            changed.push_back(std::move(status));
        }
    }
    return changed;
}

std::vector<types::Fill> Runner::FetchFills() {
    // A pushed fill moves the cursor on, so a fill the push missed can be
    // older than it: with pushes, look back as far as the duplicate check
    // reaches.
    const types::Timestamp cursor{
        std::chrono::milliseconds(mPublishedCursor.load())};
    const types::Timestamp since =
        mPushed ? std::max(mRunStart, cursor - kSeenFillHorizon) : cursor;
    auto fills = mVenue->GetFills(since);
    if (!fills) {
        spdlog::warn("strategy {}: could not read fills: {}", mName,
                     fills.error().ToString());
        return {};
    }
    const auto by_time = [](const types::Fill& a, const types::Fill& b) {
        return a.time < b.time;
    };
    // Venues return fills oldest first, so this is usually a no-op (and a
    // sort allocates a buffer on every poll).
    if (!std::ranges::is_sorted(*fills, by_time)) {
        std::stable_sort(fills->begin(), fills->end(), by_time);
    }
    return std::move(*fills);
}

std::size_t Runner::DeliverPolled(const PolledEvent& polled) {
    std::size_t delivered = 0;
    for (const types::Fill& fill : polled.fills) {
        // GetFills includes fills at exactly the cursor, so the last poll's
        // newest fills come back every time; pushed ones come back too.
        if (DeliverFill(fill)) {
            ++delivered;
        }
    }
    if (mRecorder) {
        mRecorder->Flush();
    }
    for (const types::OrderStatus& status : polled.orders) {
        if (mTracker->Note(status)) {
            ++delivered;
            Guarded("OnOrderUpdate",
                    [&] { mStrategy->OnOrderUpdate(*this, status); });
        }
    }
    return delivered;
}

bool Runner::DeliverFill(const types::Fill& fill) {
    const types::Timestamp horizon = mFillCursor - kSeenFillHorizon;
    if (fill.time < horizon) {
        spdlog::warn(
            "strategy {}: dropping fill {} from {}, older than the "
            "duplicate check reaches",
            mStrategy->Name(), fill.fill_id, types::FormatIso8601(fill.time));
        return false;
    }
    if (!mSeenFills.emplace(fill.fill_id, fill.time).second) {
        return false;
    }
    if (fill.time > mFillCursor) {
        mFillCursor = fill.time;
        mPublishedCursor = mFillCursor.time_since_epoch().count();
        const types::Timestamp keep = mFillCursor - kSeenFillHorizon;
        std::erase_if(mSeenFills,
                      [keep](const auto& seen) { return seen.second < keep; });
    }
    if (mRecorder) {
        mRecorder->NoteFill(fill);
    }
    Guarded("OnFill", [&] { mStrategy->OnFill(*this, fill); });
    return true;
}

}  // namespace kestrel::strategy
