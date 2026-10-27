#include "kestrel/backtest/replay_data_source.hpp"

#include <algorithm>

namespace kestrel::backtest {

using types::ErrorCategory;
using types::MakeError;

class ReplayDataSource::Subscription : public data::Subscription {
   public:
    Subscription(ReplayDataSource& source, types::Timeframe timeframe)
        : mSource(source), mTimeframe(timeframe) {}
    ~Subscription() override { Stop(); }
    void Stop() override { mSource.mStreams.erase(mTimeframe); }

   private:
    ReplayDataSource& mSource;
    types::Timeframe mTimeframe;
};

class ReplayDataSource::QuoteSubscription : public data::Subscription {
   public:
    explicit QuoteSubscription(ReplayDataSource& source) : mSource(source) {}
    ~QuoteSubscription() override { Stop(); }
    void Stop() override { mSource.mQuoteStream.reset(); }

   private:
    ReplayDataSource& mSource;
};

void ReplayDataSource::AddQuotes(const types::Symbol& symbol,
                                 std::vector<types::Quote> quotes) {
    const auto before = [](const types::Quote& a, const types::Quote& b) {
        return a.received_time != b.received_time
                   ? a.received_time < b.received_time
                   : a.seq < b.seq;
    };
    // Stored quotes arrive in order, and sorting copies strings: skip it then.
    if (!std::ranges::is_sorted(quotes, before)) {
        std::stable_sort(quotes.begin(), quotes.end(), before);
    }
    mQuotes[symbol] = std::move(quotes);
}

const std::vector<types::Quote>& ReplayDataSource::QuotesFor(
    const types::Symbol& symbol) const {
    static const std::vector<types::Quote> kNone;
    auto it = mQuotes.find(symbol);
    return it == mQuotes.end() ? kNone : it->second;
}

bool ReplayDataSource::EmitQuote(const types::Quote& quote) {
    if (!mQuoteStream || !mQuoteStream->symbols.contains(quote.symbol)) {
        return false;
    }
    mQuoteStream->callback(quote);
    return true;
}

void ReplayDataSource::AddBars(const types::Symbol& symbol,
                               types::Timeframe timeframe,
                               std::vector<types::Bar> bars) {
    std::sort(bars.begin(), bars.end(),
              [](const types::Bar& a, const types::Bar& b) {
                  return a.open_time < b.open_time;
              });
    mBars[{symbol, timeframe}] = std::move(bars);
}

void ReplayDataSource::SetSymbolInfo(const types::SymbolInfo& info) {
    mInfo[info.symbol] = info;
}

const std::vector<types::Bar>& ReplayDataSource::Series(
    const types::Symbol& symbol, types::Timeframe timeframe) const {
    static const std::vector<types::Bar> kNone;
    auto it = mBars.find({symbol, timeframe});
    return it == mBars.end() ? kNone : it->second;
}

bool ReplayDataSource::Emit(const types::Symbol& symbol,
                            types::Timeframe timeframe, const types::Bar& bar) {
    auto it = mStreams.find(timeframe);
    if (it == mStreams.end() || !it->second.symbols.contains(symbol)) {
        return false;
    }
    it->second.callback(symbol, timeframe, bar);
    return true;
}

types::Result<std::vector<types::Bar>> ReplayDataSource::GetBars(
    const types::Symbol& symbol, types::Timeframe timeframe,
    types::Timestamp start, types::Timestamp end) {
    const auto step = types::FixedDuration(timeframe);
    std::vector<types::Bar> out;
    for (const types::Bar& bar : Series(symbol, timeframe)) {
        if (bar.open_time < start || bar.open_time >= end) {
            continue;
        }
        if (mNow) {
            // Calendar-length bars (no fixed duration) are treated as closed
            // once the next one could have started.
            const types::Timestamp close = bar.open_time + step.value_or({});
            if (step ? close > *mNow : bar.open_time >= *mNow) {
                continue;
            }
        }
        out.push_back(bar);
    }
    return out;
}

types::Result<std::unique_ptr<data::Subscription>>
ReplayDataSource::StreamQuotes(const std::vector<std::string>& symbols,
                               data::QuoteCallback callback) {
    mQuoteStream =
        QuoteStream{std::set<types::Symbol>(symbols.begin(), symbols.end()),
                    std::move(callback)};
    return std::unique_ptr<data::Subscription>(new QuoteSubscription(*this));
}

types::Result<std::unique_ptr<data::Subscription>> ReplayDataSource::StreamBars(
    const std::vector<std::string>& symbols, types::Timeframe timeframe,
    data::BarCallback callback) {
    mStreams[timeframe] =
        Stream{std::set<types::Symbol>(symbols.begin(), symbols.end()),
               std::move(callback)};
    return std::unique_ptr<data::Subscription>(
        new Subscription(*this, timeframe));
}

types::Result<types::SymbolInfo> ReplayDataSource::GetSymbolInfo(
    const types::Symbol& symbol) {
    auto it = mInfo.find(symbol);
    if (it == mInfo.end()) {
        return MakeError(ErrorCategory::kNotFound,
                         "no symbol info for " + symbol + " in the replay");
    }
    return it->second;
}

}  // namespace kestrel::backtest
