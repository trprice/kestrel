#include "kestrel/backtest/backtest.hpp"

#include <algorithm>

#include "kestrel/strategy/runner.hpp"

namespace kestrel::backtest {

using types::ErrorCategory;
using types::MakeError;

namespace {

struct Replayed {
    types::Timestamp close;
    types::Symbol symbol;
    types::Timeframe timeframe;
    types::Bar bar;
};

}  // namespace

types::Result<BacktestResult> RunBacktest(
    std::shared_ptr<ReplayDataSource> data,
    std::unique_ptr<strategy::Strategy> strategy, BacktestOptions options) {
    const strategy::Requirements needs = strategy->Needs();
    if (needs.quotes) {
        return MakeError(ErrorCategory::kUnsupported,
                         "backtests replay bars only; the strategy asks for "
                         "quotes");
    }

    // Every bar to replay, ordered by when it closes; ties keep the order of
    // the strategy's symbols and timeframes.
    std::vector<Replayed> timeline;
    for (types::Timeframe timeframe : needs.bar_timeframes) {
        const auto step = types::FixedDuration(timeframe);
        if (!step) {
            return MakeError(ErrorCategory::kUnsupported,
                             "backtests cannot replay calendar timeframe " +
                                 std::string(types::ToString(timeframe)));
        }
        for (const types::Symbol& symbol : needs.symbols) {
            for (const types::Bar& bar : data->Series(symbol, timeframe)) {
                if (bar.open_time >= options.start &&
                    bar.open_time < options.end) {
                    timeline.push_back(
                        {bar.open_time + *step, symbol, timeframe, bar});
                }
            }
        }
    }
    std::stable_sort(
        timeline.begin(), timeline.end(),
        [](const Replayed& a, const Replayed& b) { return a.close < b.close; });

    types::Timestamp now = options.start;
    data->SetTime(now);
    auto clock = [&now] { return now; };
    auto venue = std::make_shared<trading::PaperTradingVenue>(
        data, options.paper, clock);

    strategy::RunnerOptions runner_options;
    runner_options.risk = options.risk;
    strategy::Runner runner(data, venue, std::move(strategy), runner_options,
                            clock);
    if (auto begun = runner.Begin(); !begun) {
        return std::unexpected(begun.error());
    }

    for (const Replayed& item : timeline) {
        now = item.close;
        data->SetTime(now);

        types::Quote quote;
        quote.symbol = item.symbol;
        quote.venue = std::string(data->Name());
        quote.received_time = now;
        quote.bid = quote.ask = quote.last = item.bar.close;
        venue->OnQuote(quote);

        data->Emit(item.symbol, item.timeframe, item.bar);
        runner.Drain();
    }
    runner.End();

    BacktestResult result;
    result.bars_replayed = timeline.size();
    auto fills = venue->GetFills(options.start);
    auto positions = venue->GetPositions();
    auto balances = venue->GetBalances();
    if (!fills || !positions || !balances) {
        return MakeError(ErrorCategory::kInternal,
                         "could not read the paper venue's results");
    }
    result.fills = std::move(*fills);
    result.positions = std::move(*positions);
    result.balances = std::move(*balances);
    return result;
}

}  // namespace kestrel::backtest
