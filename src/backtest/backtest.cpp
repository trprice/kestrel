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
    // A bar of BacktestOptions::fill_timeframe: for the venue only.
    bool fill_only = false;
    const types::Symbol* symbol = nullptr;  // in the strategy's Requirements
    types::Timeframe timeframe{};
    types::Bar bar;
    const types::Quote* quote = nullptr;  // in the ReplayDataSource
};

// Whether `bars` (oldest first) reaches from `first` to the end of the window
// [first, to), give or take a bar at each end. On a market that closes, the
// ends are those of the window's first and last sessions, so intraday bars
// of a window that starts or ends while it is closed can cover it.
bool Covers(const std::vector<types::Bar>& bars, types::Timestamp first,
            types::Timestamp to, std::chrono::seconds length,
            const types::MarketHours* hours) {
    if (bars.empty()) {
        return false;
    }
    if (hours && !hours->IsAlwaysOpen()) {
        auto session = hours->CurrentOrNext(first);
        if (session && session->start < to) {
            first = std::max(first, session->start);
            types::Timestamp last_end = first;
            while (session && session->start < to) {
                last_end = std::min(session->end, to);
                session = hours->CurrentOrNext(session->end);
            }
            to = last_end;
        }
    }
    // Only the ends: a gap inside is the venue's (an outage leaves no bars),
    // and fetching again would find the same gap.
    return bars.front().open_time <= first + length &&
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
                } else if (Covers(*saved, first, to, *length,
                                  replay->Hours().get())) {
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

types::Result<void> CheckFillTimeframe(
    const strategy::Requirements& needs,
    std::optional<types::Timeframe> fill_timeframe) {
    if (!fill_timeframe) {
        return {};
    }
    const auto length = types::FixedDuration(*fill_timeframe);
    if (!length) {
        return MakeError(ErrorCategory::kUnsupported,
                         "backtests cannot replay calendar timeframe " +
                             std::string(types::ToString(*fill_timeframe)));
    }
    if (needs.quotes) {
        return MakeError(ErrorCategory::kInvalidArgument,
                         "a strategy that takes quotes fills on them, "
                         "not on bars of a fill timeframe");
    }
    for (types::Timeframe timeframe : needs.bar_timeframes) {
        const auto own = types::FixedDuration(timeframe);
        if (own && *own < *length) {
            return MakeError(ErrorCategory::kInvalidArgument,
                             "the fill timeframe " +
                                 std::string(types::ToString(*fill_timeframe)) +
                                 " is coarser than the strategy's " +
                                 std::string(types::ToString(timeframe)));
        }
    }
    return {};
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
    if (auto checked = CheckFillTimeframe(needs, options.fill_timeframe);
        !checked) {
        return std::unexpected(checked.error());
    }

    // Every bar and quote to replay, ordered by when it closes / arrived; ties
    // put quotes first, then fill-timeframe bars (so the venue has walked up
    // to a strategy bar's close before the strategy sees it), then keep the
    // order of the strategy's symbols and timeframes.
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
    const std::size_t bar_count = timeline.size();
    // A fill timeframe the strategy also takes already drives the venue.
    const bool fill_bars =
        options.fill_timeframe &&
        std::ranges::find(needs.bar_timeframes, *options.fill_timeframe) ==
            needs.bar_timeframes.end();
    if (fill_bars) {
        const auto step = *types::FixedDuration(*options.fill_timeframe);
        for (const types::Symbol& symbol : needs.symbols) {
            for (const types::Bar& bar :
                 data->Series(symbol, *options.fill_timeframe)) {
                if (bar.open_time >= options.start &&
                    bar.open_time < options.end) {
                    Replayed item;
                    item.time = bar.open_time + step;
                    item.fill_only = true;
                    item.symbol = &symbol;
                    item.timeframe = *options.fill_timeframe;
                    item.bar = bar;
                    timeline.push_back(item);
                }
            }
        }
    }
    const std::size_t fill_bar_count = timeline.size() - bar_count;
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
                         const auto rank = [](const Replayed& r) {
                             return r.is_quote ? 0 : r.fill_only ? 1 : 2;
                         };
                         return a.time != b.time ? a.time < b.time
                                                 : rank(a) < rank(b);
                     });

    types::Timestamp now = options.start;
    data->SetTime(now);
    auto clock = [&now] { return now; };
    auto venue = std::make_shared<trading::PaperTradingVenue>(
        data, options.paper, clock);

    strategy::RunnerOptions runner_options;
    runner_options.risk = options.risk;
    if (runner_options.risk) {
        runner_options.risk->shorts = options.paper.allow_short;
        runner_options.risk->default_quote_asset =
            options.paper.default_quote_asset;
    }
    runner_options.max_queued_bars = 0;  // each bar is drained as it is emitted
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

    // Session hooks and timer ticks due before `until` run at their own
    // times.
    const auto run_sessions = [&](types::Timestamp until) {
        while (true) {
            auto due = runner.NextSessionEvent();
            if (const auto timer = runner.NextTimer()) {
                due = due ? std::min(*due, *timer) : *timer;
            }
            if (!due || *due >= until) {
                break;
            }
            now = std::max(now, *due);
            data->SetTime(now);
            runner.Drain();
        }
    };
    // Without recorded quotes the venue trades on the bars of each symbol's
    // finest series that has any (a coarser one would replay prices it has
    // already seen), so a symbol missing the finer data still trades.
    std::vector<std::optional<types::Timeframe>> driving(needs.symbols.size());
    for (const Replayed& item : timeline) {
        if (item.is_quote || item.fill_only) {
            continue;
        }
        auto& finest = driving[static_cast<std::size_t>(item.symbol -
                                                        needs.symbols.data())];
        if (!finest || *types::FixedDuration(item.timeframe) <
                           *types::FixedDuration(*finest)) {
            finest = item.timeframe;
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
    // On a market that closes, only the part of the bar in its first session
    // is walked (a daily bar trades between the open and the close, not
    // across the whole day), and session hooks due at its start run just
    // after its open: an order placed there trades against the open (a
    // buy-stop it gapped through fills at it, as does a market order).
    const auto hours = data->Hours();
    const auto walk = [&](const types::Symbol& symbol, const types::Bar& bar,
                          types::Timestamp close) {
        types::Timestamp open = bar.open_time;
        if (hours && !hours->IsAlwaysOpen()) {
            if (const auto session = hours->CurrentOrNext(open);
                session && session->start < close) {
                open = std::max(open, session->start);
                close = std::min(close, session->end);
            }
        }
        const bool high_first = bar.high - bar.open <= bar.open - bar.low;
        const std::array<Decimal, 4> points = {
            bar.open, high_first ? bar.high : bar.low,
            high_first ? bar.low : bar.high, bar.close};
        const auto leg_length = (close - open) / 3;
        quote_at(symbol, points[0], open);
        if (hours && !hours->IsAlwaysOpen()) {
            run_sessions(open + types::Timestamp::duration{1});
        }
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
    // With fill-timeframe bars, the open of the last one walked for each
    // symbol: a strategy bar none of them opened inside is walked itself.
    std::vector<std::optional<types::Timestamp>> last_fill(
        needs.symbols.size());
    std::vector<std::optional<types::Timestamp>> last_fill_end(
        needs.symbols.size());
    std::size_t walked_without = 0;
    for (const Replayed& item : timeline) {
        const auto index =
            static_cast<std::size_t>(item.symbol - needs.symbols.data());
        if (item.fill_only) {
            walk(*item.symbol, item.bar, item.time);
            last_fill[index] = item.bar.open_time;
            last_fill_end[index] = item.time;
        } else if (!item.is_quote && !needs.quotes &&
                   item.timeframe == driving[index]) {
            if (!fill_bars) {
                walk(*item.symbol, item.bar, item.time);  // ends at item.time
            } else if (!last_fill[index] ||
                       *last_fill[index] < item.bar.open_time) {
                walk(*item.symbol, item.bar, item.time);
                ++walked_without;
            } else if (!last_fill_end[index] ||
                       *last_fill_end[index] < item.time) {
                quote_at(*item.symbol, item.bar.close, item.time);
            }
        }
        run_sessions(item.time);
        now = std::max(now, item.time);
        data->SetTime(now);
        if (item.fill_only) {
            observe(*item.symbol, item.bar.close);
            continue;
        }

        if (item.is_quote) {
            venue->OnQuote(*item.quote);
            data->EmitQuote(*item.quote);
            runner.Drain();
            observe(*item.symbol, SummaryBuilder::MarkOf(*item.quote));
            continue;
        }
        // What the close calls for trades at the next price, not this one.
        venue->SetDeferMatching(!options.fill_at_close);
        data->Emit(*item.symbol, item.timeframe, item.bar);
        runner.Drain();
        venue->SetDeferMatching(false);
        // With quotes the venue trades on them, so they alone set the marks.
        observe(*item.symbol, needs.quotes ? std::nullopt
                                           : std::optional(item.bar.close));
    }
    // And those after the last event, up to the end.
    run_sessions(options.end);
    runner.End();

    BacktestResult result;
    result.bars_replayed = bar_count;
    result.fill_bars_replayed = fill_bar_count;
    result.quotes_replayed = timeline.size() - bar_count - fill_bar_count;
    result.bars_walked_without_fill_bars = walked_without;
    if (walked_without > 0) {
        spdlog::warn(
            "backtest: {} strategy bars had no {} bars inside them and were "
            "walked as a whole",
            walked_without, types::ToString(*options.fill_timeframe));
    }
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
