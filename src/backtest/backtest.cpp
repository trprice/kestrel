#include "kestrel/backtest/backtest.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>

#include "kestrel/strategy/runner.hpp"

namespace kestrel::backtest {

using types::Decimal;
using types::ErrorCategory;
using types::MakeError;

namespace {

// One thing to replay: a closed bar, or (when `is_quote`) a stored quote. It
// points at the symbol and quote rather than holding them, since a timeline of
// tens of thousands of these is sorted and every move would copy their
// strings.
struct Replayed {
    types::Timestamp time;  // when the bar closed / the quote arrived
    bool is_quote = false;
    const types::Symbol* symbol = nullptr;  // in the strategy's Requirements
    types::Timeframe timeframe{};
    types::Bar bar;
    const types::Quote* quote = nullptr;  // in the ReplayDataSource
};

// Whether `bars` (oldest first) reaches from `first` to the end of the window
// [first, to), give or take a bar at each end.
bool Covers(const std::vector<types::Bar>& bars, types::Timestamp first,
            types::Timestamp to, std::chrono::seconds length) {
    return !bars.empty() && bars.front().open_time <= first + length &&
           bars.back().open_time >= to - length * 2;
}

}  // namespace

types::Result<std::shared_ptr<ReplayDataSource>> LoadReplayData(
    data::DataSource& source, const strategy::Requirements& needs,
    types::Timestamp from, types::Timestamp to, StoredData stored) {
    if (to <= from) {
        return MakeError(ErrorCategory::kInvalidArgument,
                         "the end of a backtest must be after its start");
    }
    auto replay = std::make_shared<ReplayDataSource>();
    replay->SetHours(source.Hours());
    for (const types::Symbol& symbol : needs.symbols) {
        auto info = source.GetSymbolInfo(symbol);
        if (!info) {
            return std::unexpected(info.error());
        }
        replay->SetSymbolInfo(*info);
        for (const types::Timeframe timeframe : needs.bar_timeframes) {
            const auto length = types::FixedDuration(timeframe);
            if (!length) {
                return MakeError(ErrorCategory::kUnsupported,
                                 "backtests cannot replay calendar timeframe " +
                                     std::string(types::ToString(timeframe)));
            }
            const auto first = from - *length * (needs.warmup_bars + 1);
            const std::string name(types::ToString(timeframe));
            if (stored.bars) {
                auto saved = stored.bars->Load(std::string(source.Name()),
                                               symbol, timeframe, first, to);
                if (!saved) {
                    spdlog::warn("{} {}: database read failed ({}); using {}",
                                 symbol, name, saved.error().ToString(),
                                 source.Name());
                } else if (Covers(*saved, first, to, *length)) {
                    spdlog::info("{} {}: {} bars loaded from the database",
                                 symbol, name, saved->size());
                    replay->AddBars(symbol, timeframe, std::move(*saved));
                    continue;
                } else {
                    spdlog::info(
                        "{} {}: database has {} bars, not enough for the "
                        "range; using {}",
                        symbol, name, saved->size(), source.Name());
                }
            }
            auto bars = source.GetBars(symbol, timeframe, first, to);
            if (!bars) {
                return std::unexpected(bars.error());
            }
            spdlog::info("{} {}: {} bars loaded from {}", symbol, name,
                         bars->size(), source.Name());
            if (stored.bars && !bars->empty()) {
                // Saving is idempotent, so overlap with what is stored is fine.
                if (auto saved = stored.bars->Save(std::string(source.Name()),
                                                   symbol, timeframe, *bars);
                    !saved) {
                    spdlog::warn("{} {}: could not save bars to the database: {}",
                                 symbol, name, saved.error().ToString());
                }
            }
            replay->AddBars(symbol, timeframe, std::move(*bars));
        }
        if (needs.quotes && stored.quotes_from_trades) {
            if (!stored.trades) {
                return MakeError(ErrorCategory::kNotFound,
                                 "quotes from trades need the database's "
                                 "market_trades");
            }
            auto trades = stored.trades->Load(std::string(source.Name()),
                                              symbol, from, to);
            if (!trades) {
                return std::unexpected(trades.error());
            }
            if (trades->empty()) {
                return MakeError(ErrorCategory::kNotFound,
                                 "no stored trades for " + symbol + " from " +
                                     std::string(source.Name()) +
                                     " in the range; fetch them with "
                                     "`trades --store`");
            }
            auto quotes = QuotesFromTrades(symbol, std::string(source.Name()),
                                           *trades, info->tick_size);
            spdlog::info("{}: {} quotes built from {} stored trades", symbol,
                         quotes.size(), trades->size());
            replay->AddQuotes(symbol, std::move(quotes));
        } else if (needs.quotes) {
            if (!stored.quotes) {
                return MakeError(ErrorCategory::kNotFound,
                                 "the strategy needs quotes, which are only "
                                 "available from the database");
            }
            auto quotes = stored.quotes->Load(std::string(source.Name()),
                                              symbol, from, to);
            if (!quotes) {
                return std::unexpected(quotes.error());
            }
            if (quotes->empty()) {
                return MakeError(ErrorCategory::kNotFound,
                                 "no stored quotes for " + symbol + " from " +
                                     std::string(source.Name()) +
                                     " in the range");
            }
            spdlog::info("{}: {} quotes loaded from the database", symbol,
                         quotes->size());
            replay->AddQuotes(symbol, std::move(*quotes));
        }
    }
    return replay;
}

std::vector<types::Quote> QuotesFromTrades(
    const types::Symbol& symbol, const std::string& venue,
    std::span<const types::MarketTrade> trades, const types::Decimal& tick) {
    std::vector<types::Quote> quotes;
    quotes.reserve(trades.size());
    std::optional<Decimal> bid;
    std::optional<Decimal> ask;
    for (const types::MarketTrade& trade : trades) {
        if (trade.taker_side == types::Side::kSell) {
            bid = trade.price;  // the seller hit the bid
            if (ask && *ask <= *bid) {
                ask = *bid + tick;
            }
        } else {
            ask = trade.price;  // the buyer took the ask
            if (bid && *bid >= *ask) {
                bid = *ask - tick;
            }
        }
        types::Quote q;
        q.symbol = symbol;
        q.venue = venue;
        q.seq = static_cast<std::int64_t>(quotes.size()) + 1;
        q.exchange_time = trade.time;
        q.received_time = trade.time;
        q.bid = bid;
        q.ask = ask;
        q.last = trade.price;
        q.last_size = trade.quantity;
        q.last_taker_side = trade.taker_side;
        quotes.push_back(std::move(q));
    }
    return quotes;
}

types::Result<BacktestResult> RunBacktest(
    const std::shared_ptr<ReplayDataSource>& data,
    std::unique_ptr<strategy::Strategy> strategy,
    const BacktestOptions& options) {
    if (options.end <= options.start) {
        return MakeError(ErrorCategory::kInvalidArgument,
                         "the end of a backtest must be after its start");
    }
    const strategy::Requirements needs = strategy->Needs();

    // Every bar and quote to replay, ordered by when it closes / arrived; ties
    // put quotes first, then keep the order of the strategy's symbols and
    // timeframes.
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
                    Replayed item;
                    item.time = bar.open_time + *step;
                    item.symbol = &symbol;
                    item.timeframe = timeframe;
                    item.bar = bar;
                    timeline.push_back(item);
                }
            }
        }
    }
    std::size_t bar_count = timeline.size();
    if (needs.quotes) {
        for (const types::Symbol& symbol : needs.symbols) {
            for (const types::Quote& quote : data->QuotesFor(symbol)) {
                if (quote.received_time >= options.start &&
                    quote.received_time < options.end) {
                    Replayed item;
                    item.time = quote.received_time;
                    item.is_quote = true;
                    item.symbol = &symbol;
                    item.quote = &quote;
                    timeline.push_back(item);
                }
            }
        }
    }
    std::stable_sort(timeline.begin(), timeline.end(),
                     [](const Replayed& a, const Replayed& b) {
                         return a.time != b.time ? a.time < b.time
                                                 : a.is_quote && !b.is_quote;
                     });

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
    auto summary = SummaryBuilder::Create(
        needs.symbols, options.paper.default_quote_asset,
        options.paper.initial_balances, options.start, options.equity_interval);
    // Called after each event, once the strategy and the venue have seen it.
    auto observe = [&](const types::Symbol& symbol,
                       std::optional<Decimal> mark) {
        if (!summary) {
            return;
        }
        if (auto balances = venue->GetBalances()) {
            summary->Observe(now, symbol, mark, *balances);
        }
    };

    // Session hooks due before `until` run at their own times.
    const auto run_sessions = [&](types::Timestamp until) {
        while (const auto due = runner.NextSessionEvent()) {
            if (*due >= until) {
                break;
            }
            now = std::max(now, *due);
            data->SetTime(now);
            runner.Drain();
        }
    };
    // Without recorded quotes the venue trades on the bars of the finest
    // series (a coarser one would replay prices it has already seen).
    std::optional<types::Timeframe> driving;
    for (types::Timeframe timeframe : needs.bar_timeframes) {
        if (!driving || *types::FixedDuration(timeframe) <
                            *types::FixedDuration(*driving)) {
            driving = timeframe;
        }
    }
    // A quote at `price` for the venue at `t`, then the strategy runs.
    const auto quote_at = [&](const types::Symbol& symbol, const Decimal& price,
                              types::Timestamp t) {
        run_sessions(t);
        now = std::max(now, t);
        data->SetTime(now);
        types::Quote quote;
        quote.symbol = symbol;
        quote.venue = std::string(data->Name());
        quote.received_time = now;
        quote.bid = quote.ask = quote.last = price;
        venue->OnQuote(quote);
        runner.Drain();
    };
    // Walks a bar for the venue: open, the extreme nearer the open, the other
    // extreme, close, a third of the bar apart, and on each leg the price of
    // every resting order it passes, so a stop or limit inside the bar trades
    // at its own price (and one the open gaps through, at the open). The
    // strategy runs at each point, so an order it places after a fill there
    // takes part in the rest of the bar.
    const auto walk = [&](const types::Symbol& symbol, const types::Bar& bar,
                          types::Timestamp close) {
        const types::Timestamp open = bar.open_time;
        const bool high_first = bar.high - bar.open <= bar.open - bar.low;
        const std::array<Decimal, 4> points = {
            bar.open, high_first ? bar.high : bar.low,
            high_first ? bar.low : bar.high, bar.close};
        const auto leg_length = (close - open) / 3;
        quote_at(symbol, points[0], open);
        for (std::size_t leg = 0; leg < 3; ++leg) {
            const Decimal& from = points[leg];
            const Decimal& to = points[leg + 1];
            const bool up = from < to;
            const types::Timestamp leg_start =
                open + leg_length * static_cast<std::int64_t>(leg);
            // Each resting price strictly between, in the leg's direction,
            // at the leg's start time; asked again after each, since the
            // strategy may have placed or cancelled orders there.
            for (Decimal at = from;;) {
                std::optional<Decimal> next;
                for (const Decimal& price : venue->RestingPrices(symbol)) {
                    const bool ahead = up ? at < price && price < to
                                          : to < price && price < at;
                    if (ahead &&
                        (!next || (up ? price < *next : *next < price))) {
                        next = price;
                    }
                }
                if (!next) {
                    break;
                }
                at = *next;
                quote_at(symbol, at, leg_start);
            }
            quote_at(symbol, to, leg == 2 ? close : leg_start + leg_length);
        }
    };
    for (const Replayed& item : timeline) {
        if (!item.is_quote && !needs.quotes && item.timeframe == driving) {
            walk(*item.symbol, item.bar, item.time);  // ends at item.time
        }
        run_sessions(item.time);
        now = std::max(now, item.time);
        data->SetTime(now);

        if (item.is_quote) {
            venue->OnQuote(*item.quote);
            data->EmitQuote(*item.quote);
            runner.Drain();
            observe(*item.symbol, SummaryBuilder::MarkOf(*item.quote));
            continue;
        }
        data->Emit(*item.symbol, item.timeframe, item.bar);
        runner.Drain();
        // With quotes the venue trades on them, so they alone set the marks.
        observe(*item.symbol, needs.quotes ? std::nullopt
                                           : std::optional(item.bar.close));
    }
    // And those after the last event, up to the end.
    run_sessions(options.end);
    runner.End();

    BacktestResult result;
    result.bars_replayed = bar_count;
    result.quotes_replayed = timeline.size() - bar_count;
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
    result.summary = summary ? summary->Finish(options.end, result.fills)
                             : std::unexpected(summary.error());
    if (!result.summary) {
        spdlog::warn("backtest: no summary: {}",
                     result.summary.error().ToString());
    }
    return result;
}

}  // namespace kestrel::backtest
