#include "kestrel/strategy/runner.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>

#include "kestrel/strategy/run_recorder.hpp"

namespace kestrel::strategy {

using types::Decimal;
using types::ErrorCategory;
using types::MakeError;

namespace {

// Stand-in bar length for calendar-based timeframes when sizing the warmup
// request; the request is padded, and only the newest bars are kept.
constexpr std::chrono::hours kMonthEstimate{24 * 31};

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
        types::OrderState state;
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

Runner::Event Runner::TakeFront() {
    Event event = std::move(mEvents.front());
    mEvents.pop_front();
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
                   const types::Bar& bar) { Push(BarEvent{symbol, tf, bar}); });
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
    return {};
}

void Runner::End() {
    if (!mBegun) {
        return;
    }
    mBegun = false;
    // Nothing is queued by the poller from here on. A poll it queued behind
    // the StopEvent is never handled: its fills are found again by the poll
    // below (only with a recorder), its order changes are dropped, since the
    // strategy is stopping.
    StopPoller();
    if (mRecorder) {
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
        // The deadline only paces the recorder's ticks when nothing arrives.
        auto event = Pop(polling ? std::optional(
                                       std::chrono::steady_clock::now() + interval)
                                 : std::nullopt);
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
        const std::chrono::seconds step =
            types::FixedDuration(timeframe).value_or(kMonthEstimate);
        for (const types::Symbol& symbol : requirements.symbols) {
            auto& window = mBars[{symbol, timeframe}];
            if (requirements.warmup_bars == 0) {
                continue;
            }
            // One bar of padding in case the newest slot has no bar yet.
            const types::Timestamp start =
                now -
                step * static_cast<std::int64_t>(requirements.warmup_bars + 1);
            auto bars = mData->GetBars(symbol, timeframe, start, now);
            if (!bars) {
                return std::unexpected(bars.error());
            }
            const std::size_t keep =
                std::min(bars->size(), requirements.warmup_bars);
            window.assign(bars->end() - keep, bars->end());
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
        if (event.bar.open_time > expected) {
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
