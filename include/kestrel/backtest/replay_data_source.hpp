#pragma once

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "kestrel/data/data_source.hpp"

namespace kestrel::backtest {

// A DataSource over bars held in memory, for backtests. The driver advances a
// simulated clock with SetTime and hands bars to the strategy with Emit.
//
// GetBars never shows the future: once SetTime has been called it returns only
// bars that have closed by then. Quotes are replayed the same way as bars:
// AddQuotes stores them and EmitQuote hands one to the StreamQuotes
// subscription. Without a subscription nothing is delivered.
//
// Not thread-safe; everything is meant to happen on one thread.
class ReplayDataSource : public data::DataSource {
   public:
    // Stores the bars of one series, replacing any already there. They are
    // sorted by open time.
    void AddBars(const types::Symbol& symbol, types::Timeframe timeframe,
                 std::vector<types::Bar> bars);
    // Stores the quotes of one symbol, replacing any already there. They are
    // ordered by (received_time, seq), keeping arrival order for ties.
    void AddQuotes(const types::Symbol& symbol,
                   std::vector<types::Quote> quotes);
    void SetSymbolInfo(const types::SymbolInfo& info);
    void SetTime(types::Timestamp now) { mNow = now; }

    // The stored bars of a series, oldest first; empty if there are none.
    const std::vector<types::Bar>& Series(const types::Symbol& symbol,
                                          types::Timeframe timeframe) const;

    // The stored quotes of a symbol in arrival order; empty if there are none.
    const std::vector<types::Quote>& QuotesFor(const types::Symbol& symbol) const;

    // Delivers `quote` to the StreamQuotes subscription if it covers the
    // quote's symbol. Returns false if not.
    bool EmitQuote(const types::Quote& quote);

    // Delivers `bar` to the StreamBars subscription covering the series.
    // Returns false if nothing is subscribed to it.
    bool Emit(const types::Symbol& symbol, types::Timeframe timeframe,
              const types::Bar& bar);

    std::string_view Name() const override { return "replay"; }
    types::Result<std::vector<types::Bar>> GetBars(
        const types::Symbol& symbol, types::Timeframe timeframe,
        types::Timestamp start, types::Timestamp end) override;
    types::Result<std::unique_ptr<data::Subscription>> StreamQuotes(
        const std::vector<std::string>& symbols,
        data::QuoteCallback callback) override;
    types::Result<std::unique_ptr<data::Subscription>> StreamBars(
        const std::vector<std::string>& symbols, types::Timeframe timeframe,
        data::BarCallback callback) override;
    // kNotFound for a symbol without SetSymbolInfo.
    types::Result<types::SymbolInfo> GetSymbolInfo(
        const types::Symbol& symbol) override;

   private:
    class Subscription;
    class QuoteSubscription;
    struct QuoteStream {
        std::set<types::Symbol> symbols;
        data::QuoteCallback callback;
    };
    struct Stream {
        std::set<types::Symbol> symbols;
        data::BarCallback callback;
    };

    std::map<std::pair<types::Symbol, types::Timeframe>,
             std::vector<types::Bar>>
        mBars;
    std::map<types::Symbol, std::vector<types::Quote>> mQuotes;
    std::optional<QuoteStream> mQuoteStream;
    std::map<types::Symbol, types::SymbolInfo> mInfo;
    std::optional<types::Timestamp> mNow;
    std::map<types::Timeframe, Stream> mStreams;
};

}  // namespace kestrel::backtest
