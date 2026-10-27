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

    // The orders last seen active, by venue order ID.
    std::map<std::string, Seen> Active() const {
        std::lock_guard lock(mMutex);
        return mActive;
    }
    // Records `status` for a tracked order and returns whether it differs from
    // what was last seen; a closed order is forgotten.
    bool Note(const types::OrderStatus& status) {
        std::lock_guard lock(mMutex);
        auto it = mActive.find(status.order_id);
        if (it == mActive.end()) {
            return false;
        }
        const bool changed = it->second.state != status.state ||
                             it->second.filled != status.filled_quantity;
        if (types::IsActive(status.state)) {
            it->second = Seen{status.state, status.filled_quantity};
        } else {
            mActive.erase(it);
        }
        return changed;
    }
    void Forget(const std::string& order_id) {
        std::lock_guard lock(mMutex);
        mActive.erase(order_id);
    }

   private:
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
      mClock(std::move(clock)) {
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
    }
}

Runner::~Runner() = default;

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

    mFillCursor = mClock();
    mSeenFills.clear();
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

    const auto interval = mOptions.fill_poll_interval;
    const bool polling = interval > std::chrono::milliseconds::zero();
    auto next_poll = std::chrono::steady_clock::now() + interval;
    for (bool running = true; running;) {
        auto event = Pop(polling ? std::optional(next_poll) : std::nullopt);
        if (event) {
            running = Handle(*event);
        }
        if (polling && std::chrono::steady_clock::now() >= next_poll) {
            Poll();
            next_poll = std::chrono::steady_clock::now() + interval;
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

bool Runner::Handle(const Event& event) {
    if (const auto* bar = std::get_if<BarEvent>(&event)) {
        HandleBar(*bar);
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
    return fills + PollOrders();
}

std::size_t Runner::PollOrders() {
    const auto tracked = mTracker->Active();
    if (tracked.empty()) {
        return 0;
    }
    auto open = mVenue->GetOpenOrders();
    if (!open) {
        spdlog::warn("strategy {}: could not read open orders: {}",
                     mStrategy->Name(), open.error().ToString());
        return 0;
    }
    std::map<std::string, const types::OrderStatus*> open_by_id;
    for (const types::OrderStatus& status : *open) {
        open_by_id[status.order_id] = &status;
    }

    std::size_t delivered = 0;
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
                             mStrategy->Name(), order_id,
                             closed.error().ToString());
                if (closed.error().category == ErrorCategory::kNotFound) {
                    mTracker->Forget(order_id);  // it will never be found
                }
                continue;
            }
            status = std::move(*closed);
        }
        if (mTracker->Note(status)) {
            ++delivered;
            Guarded("OnOrderUpdate",
                    [&] { mStrategy->OnOrderUpdate(*this, status); });
        }
    }
    return delivered;
}

std::size_t Runner::PollFills() {
    auto fills = mVenue->GetFills(mFillCursor);
    if (!fills) {
        spdlog::warn("strategy {}: could not read fills: {}", mStrategy->Name(),
                     fills.error().ToString());
        return 0;
    }
    const auto by_time = [](const types::Fill& a, const types::Fill& b) {
        return a.time < b.time;
    };
    // Venues return fills oldest first, so this is usually a no-op (and a
    // sort allocates a buffer on every poll).
    if (!std::ranges::is_sorted(*fills, by_time)) {
        std::stable_sort(fills->begin(), fills->end(), by_time);
    }

    std::size_t delivered = 0;
    for (const types::Fill& fill : *fills) {
        // GetFills includes fills at exactly the cursor, so the last poll's
        // newest fills come back every time.
        if (!mSeenFills.emplace(fill.fill_id, fill.time).second) {
            continue;
        }
        mFillCursor = std::max(mFillCursor, fill.time);
        ++delivered;
        if (mRecorder) {
            mRecorder->NoteFill(fill);
        }
        Guarded("OnFill", [&] { mStrategy->OnFill(*this, fill); });
    }
    if (mRecorder) {
        mRecorder->Flush();
    }
    std::erase_if(mSeenFills, [this](const auto& seen) {
        return seen.second < mFillCursor;
    });
    return delivered;
}

}  // namespace kestrel::strategy
