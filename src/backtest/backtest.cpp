#include "kestrel/backtest/backtest.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

#include "kestrel/strategy/runner.hpp"
#include "kestrel/types/market_hours.hpp"

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

// Where to start loading so that `warmup` bars of `length` close before
// `from`. On a market that closes, they are counted in its sessions (a stock
// market has about 252 daily bars a year, not 365), back to the first one
// needed: a daily bar from its day's start (UTC), as it is stamped at
// midnight. Elsewhere, for bars longer than a day, or with sessions not
// found within a reasonable span, `warmup` + 1 lengths back (a bar more, as
// the newest slot may have none yet).
types::Timestamp WarmupStart(types::Timestamp from, std::chrono::seconds length,
                             std::size_t warmup,
                             const types::MarketHours* hours) {
    const auto calendar = from - length * static_cast<std::int64_t>(warmup + 1);
    constexpr auto kDay = std::chrono::days(1);
    if (!hours || hours->IsAlwaysOpen() || length > kDay || warmup == 0) {
        return calendar;
    }
    // A day at a time, newest first, over the sessions that open in it. A
    // stock market needs about 1.45 days a daily bar.
    std::size_t counted = 0;
    auto t = from;
    const auto max_days = static_cast<std::int64_t>(warmup) * 2 + 30;
    std::vector<types::MarketHours::Session> sessions;
    for (std::int64_t d = 0; d < max_days; ++d) {
        const auto start = t - kDay;
        sessions.clear();
        for (auto s = hours->CurrentOrNext(start); s && s->start < t;
             s = hours->CurrentOrNext(s->end)) {
            if (s->start >= start) {  // one that opened earlier counts there
                sessions.push_back(*s);
            }
        }
        for (auto it = sessions.rbegin(); it != sessions.rend(); ++it) {
            // Its bars a run can see at `from`: one is seen once its whole
            // length has passed (ReplayDataSource), so a daily bar, stamped
            // at its day's midnight (UTC), at the next midnight, not at the
            // close, and a session's short last bar a full length after it
            // opens.
            const auto day = std::chrono::floor<std::chrono::days>(it->start);
            std::size_t n = 0;
            if (length >= kDay) {
                n = day + kDay <= from ? 1 : 0;
            } else if (from > it->start) {
                const auto open = std::chrono::ceil<std::chrono::seconds>(
                    it->end - it->start);
                const auto bars = (open + length - std::chrono::seconds(1)) /
                                  length;  // the last may be short
                const auto passed =
                    std::chrono::floor<std::chrono::seconds>(from - it->start) /
                    length;
                n = static_cast<std::size_t>(std::min(bars, passed));
            }
            if (counted + n >= warmup) {
                if (length >= kDay) {
                    return day;
                }
                // From the start of the bar's slot on the epoch's grid too,
                // in case the bars are stamped on whole hours (as
                // Databento's are) rather than at the session's open.
                const auto older =
                    static_cast<std::int64_t>(counted + n - warmup);
                const auto slot = it->start + length * older;
                return slot - slot.time_since_epoch() % length;
            }
            counted += n;
        }
        t = start;
    }
    return calendar;
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
    // Where each timeframe's bars start, the same for every symbol.
    std::vector<types::Timestamp> firsts;
    for (const types::Timeframe timeframe : needs.bar_timeframes) {
        const auto length = types::FixedDuration(timeframe);
        if (!length) {
            return MakeError(ErrorCategory::kUnsupported,
                             "backtests cannot replay calendar timeframe " +
                                 std::string(types::ToString(timeframe)));
        }
        firsts.push_back(WarmupStart(from, *length, needs.warmup_bars,
                                     replay->Hours().get()));
    }
    if (stored.bars_only && !stored.bars) {
        return MakeError(ErrorCategory::kInvalidArgument,
                         "bars only from the database need a database");
    }
    for (const types::Symbol& symbol : needs.symbols) {
        auto info = source.GetSymbolInfo(symbol);
        // A pair the venue says it does not know, in its own answer (a
        // delisted one, not a 404 or an odd response), is replayed from the
        // database with FallbackSymbolInfo, but only if the database has bars
        // for it, at any time (a window before its listing has none): else
        // it is a mistyped symbol, and the venue's error.
        const bool unknown =
            !info && stored.bars_only &&
            info.error().category == ErrorCategory::kNotFound &&
            info.error().provider_code.has_value();
        if (!info && !unknown) {
            return std::unexpected(info.error());
        }
        bool stored_any = false;  // under bars_only, in the window
        for (std::size_t i = 0; i < needs.bar_timeframes.size(); ++i) {
            const types::Timeframe timeframe = needs.bar_timeframes[i];
            const auto length = types::FixedDuration(timeframe);
            const auto first = firsts[i];
            const std::string name(types::ToString(timeframe));
            if (stored.bars_only) {
                auto saved = stored.bars->Load(std::string(source.Name()),
                                               symbol, timeframe, first, to);
                if (!saved) {
                    return std::unexpected(saved.error());
                }
                if (saved->empty()) {
                    spdlog::warn("{} {}: no bars in the database for the range",
                                 symbol, name);
                } else {
                    stored_any = true;
                    spdlog::info(
                        "{} {}: {} bars loaded from the database, {} to {}",
                        symbol, name, saved->size(),
                        types::FormatIso8601(saved->front().open_time),
                        types::FormatIso8601(saved->back().open_time));
                }
                replay->AddBars(symbol, timeframe, std::move(*saved));
                continue;
            }
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
                    spdlog::warn(
                        "{} {}: could not save bars to the database: {}",
                        symbol, name, saved.error().ToString());
                }
            }
            replay->AddBars(symbol, timeframe, std::move(*bars));
        }
        if (unknown) {
            bool stored_ever = stored_any;
            for (std::size_t i = 0;
                 !stored_ever && i < needs.bar_timeframes.size(); ++i) {
                auto any = stored.bars->Any(std::string(source.Name()), symbol,
                                            needs.bar_timeframes[i]);
                if (!any) {
                    return std::unexpected(any.error());
                }
                stored_ever = *any;
            }
            if (!stored_ever) {
                return std::unexpected(info.error());
            }
            spdlog::warn(
                "{}: {} does not know it ({}); replayed from the database "
                "with 1e-8 lots and ticks, no minimum",
                symbol, source.Name(), info.error().ToString());
            info = FallbackSymbolInfo(symbol);
        }
        replay->SetSymbolInfo(*info);
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

types::SymbolInfo FallbackSymbolInfo(const types::Symbol& symbol) {
    const Decimal tiny(1, 8);  // 1e-8
    return {symbol, tiny, tiny, Decimal(), Decimal()};
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

types::Result<void> CheckFillOptions(
    const strategy::Requirements& needs,
    std::optional<types::Timeframe> fill_timeframe,
    std::chrono::milliseconds fill_delay, bool fill_at_close,
    const types::MarketHours* hours) {
    if (auto checked = CheckFillTimeframe(needs, fill_timeframe); !checked) {
        return checked;
    }
    if (fill_delay.count() < 0) {
        return MakeError(ErrorCategory::kInvalidArgument,
                         "a fill delay cannot be negative");
    }
    if (fill_delay.count() == 0) {
        return {};
    }
    if (!fill_timeframe) {
        return MakeError(ErrorCategory::kInvalidArgument,
                         "a fill delay needs a fill timeframe");
    }
    if (fill_at_close) {
        return MakeError(ErrorCategory::kInvalidArgument,
                         "a fill delay and filling at the close "
                         "contradict each other");
    }
    const auto length = *types::FixedDuration(*fill_timeframe);
    if (fill_delay % length != std::chrono::milliseconds::zero()) {
        return MakeError(
            ErrorCategory::kInvalidArgument,
            "a fill delay must be a whole number of " +
                std::string(types::ToString(*fill_timeframe)) + " bars");
    }
    for (types::Timeframe timeframe : needs.bar_timeframes) {
        const auto own = types::FixedDuration(timeframe);
        if (!own) {
            return MakeError(ErrorCategory::kUnsupported,
                             "backtests cannot replay calendar timeframe " +
                                 std::string(types::ToString(timeframe)));
        }
        if (fill_delay >= *own) {
            return MakeError(
                ErrorCategory::kInvalidArgument,
                "a fill delay must be shorter than the strategy's " +
                    std::string(types::ToString(timeframe)) + " bars");
        }
    }
    if (hours && !hours->IsAlwaysOpen() && !needs.bar_timeframes.empty() &&
        std::ranges::all_of(needs.bar_timeframes, [](types::Timeframe tf) {
            const auto own = types::FixedDuration(tf);
            return own && *own >= std::chrono::hours(24);
        })) {
        return MakeError(
            ErrorCategory::kInvalidArgument,
            "a fill delay has no effect when the strategy's bars are daily "
            "or longer on a market that closes (orders are placed at the "
            "next open)");
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
    if (auto checked = CheckFillOptions(
            needs, options.fill_timeframe, options.fill_delay,
            options.fill_at_close, data->Hours().get());
        !checked) {
        return std::unexpected(checked.error());
    }

    // Every bar and quote to replay, ordered by when it closes / arrived; ties
    // put quotes first, then fill-timeframe bars (so the venue has walked up
    // to a strategy bar's close before the strategy sees it), then the
    // strategy's bars, finer timeframes first (the finest is the one walked,
    // so a coarser bar is seen once the venue has walked up to its close),
    // each timeframe's in the order of the strategy's symbols.
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
                         if (a.time != b.time) {
                             return a.time < b.time;
                         }
                         if (rank(a) != rank(b)) {
                             return rank(a) < rank(b);
                         }
                         // Strategy bars closing together: the finer
                         // timeframe first, as it is the one walked.
                         return rank(a) == 2 &&
                                *types::FixedDuration(a.timeframe) <
                                    *types::FixedDuration(b.timeframe);
                     });

    types::Timestamp now = options.start;
    data->SetTime(now);
    auto clock = [&now] { return now; };
    trading::PaperOptions paper = options.paper;
    paper.deferred_fill_delay = options.fill_delay;
    auto venue =
        std::make_shared<trading::PaperTradingVenue>(data, paper, clock);

    strategy::RunnerOptions runner_options;
    runner_options.risk = options.risk;
    if (runner_options.risk) {
        runner_options.risk->shorts = options.paper.allow_short;
        runner_options.risk->default_quote_asset =
            options.paper.default_quote_asset;
    }
    // Backtests have no live streams to go stale or fall behind.
    runner_options.stale_quote_warning = std::chrono::milliseconds::zero();
    runner_options.quote_stream_down = std::chrono::milliseconds::zero();
    runner_options.late_bar_error = std::chrono::milliseconds::zero();
    runner_options.max_queued_bars = 0;  // each bar is drained as it is emitted
    strategy::Runner runner(data, venue, std::move(strategy), runner_options,
                            clock);
    if (auto begun = runner.Begin(); !begun) {
        return std::unexpected(begun.error());
    }
    auto summary = SummaryBuilder::Create(
        needs.symbols, options.paper.default_quote_asset,
        options.paper.initial_balances, options.start, options.equity_interval);
    struct Fed {
        std::optional<Decimal> price;
        bool fresh = false;
    };
    std::vector<Fed> fed(needs.symbols.size());
    const auto symbol_index =
        [&](const types::Symbol& symbol) -> std::optional<std::size_t> {
        for (std::size_t k = 0; k < needs.symbols.size(); ++k) {
            if (needs.symbols[k] == symbol) {
                return k;
            }
        }
        return std::nullopt;
    };
    // Called after each event or simultaneous bar-close batch, once the
    // strategy and the venue have seen it.
    auto observe = [&](const types::Symbol& symbol,
                       std::optional<Decimal> mark) {
        if (!summary) {
            return;
        }
        if (auto balances = venue->GetBalances()) {
            summary->Observe(now, symbol, mark, *balances);
        }
    };
    auto observe_batch =
        [&](std::vector<std::pair<types::Symbol, Decimal>>& marks) {
            if (!summary) {
                return;
            }
            for (const auto& [symbol, mark] : marks) {
                if (const auto idx = symbol_index(symbol)) {
                    fed[*idx] = {mark, false};
                }
            }
            // A symbol walking a coarser bar may have traded mid-bar with no
            // bar closing here; include the latest price fed to it so a new
            // balance is not valued against a stale mark.
            if (!needs.quotes) {
                for (std::size_t k = 0; k < needs.symbols.size(); ++k) {
                    if (fed[k].fresh && fed[k].price) {
                        marks.emplace_back(needs.symbols[k], *fed[k].price);
                        fed[k].fresh = false;
                    }
                }
            }
            if (auto balances = venue->GetBalances()) {
                summary->Observe(now, marks, *balances);
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
    // A quote at `price` for the venue at `t`; the strategy has not run yet.
    const auto feed = [&](const types::Symbol& symbol, const Decimal& price,
                          types::Timestamp t) {
        run_sessions(t);
        now = std::max(now, t);
        data->SetTime(now);
        if (const auto idx = symbol_index(symbol)) {
            fed[*idx] = {price, true};
        }
        types::Quote quote;
        quote.symbol = symbol;
        quote.venue = std::string(data->Name());
        quote.received_time = now;
        quote.bid = quote.ask = quote.last = price;
        venue->OnQuote(quote);
    };
    // The same, then the strategy runs.
    const auto quote_at = [&](const types::Symbol& symbol, const Decimal& price,
                              types::Timestamp t) {
        feed(symbol, price, t);
        runner.Drain();
    };
    // Walks each driving or fill-timeframe bar for the venue: open, the
    // extreme nearer the open, the other extreme, close, a third of the bar
    // apart, and on each leg the price of every resting order it passes, so a
    // stop or limit inside the bar trades at its own price (and one the open
    // gaps through, at the open). Every step is scheduled at its own time and
    // merged across symbols, so symbols sharing a timeframe walk side by side
    // and a symbol falling back to a coarser bar still walks at its real open
    // and thirds instead of waiting until its close.
    // On a market that closes, only the part of the bar in its first session
    // is walked (a daily bar trades between the open and the close, not
    // across the whole day), and session hooks due at its start run inside
    // Drain() right after its open: an order placed there trades against the
    // open (a buy-stop it gapped through fills at it, as does a market order).
    const auto hours = data->Hours();
    struct Walk {
        const types::Symbol* symbol = nullptr;
        std::array<Decimal, 4> points;
    };
    struct Step {
        types::Timestamp time;
        std::size_t walk = 0;
        // 0: open + leg 0; 1: point 1 + leg 1; 2: point 2 + leg 2;
        // 3: close; 4: close alone (points[3], no leg).
        int stage = 0;
    };
    std::vector<Walk> all_walks;
    std::vector<Step> steps;
    const auto add_walk = [&](const types::Symbol& symbol,
                              const types::Bar& bar, types::Timestamp close) {
        types::Timestamp open = bar.open_time;
        if (hours && !hours->IsAlwaysOpen()) {
            if (const auto session = hours->CurrentOrNext(open);
                session && session->start < close) {
                open = std::max(open, session->start);
                close = std::min(close, session->end);
            }
        }
        const bool high_first = bar.high - bar.open <= bar.open - bar.low;
        const std::size_t id = all_walks.size();
        all_walks.push_back(
            Walk{&symbol,
                 {bar.open, high_first ? bar.high : bar.low,
                  high_first ? bar.low : bar.high, bar.close}});
        const auto leg = (close - open) / 3;
        const auto t1 =
            leg > types::Timestamp::duration::zero() ? open + leg : close;
        const auto t2 =
            leg > types::Timestamp::duration::zero() ? open + leg * 2 : close;
        steps.push_back({open, id, 0});
        steps.push_back({t1, id, 1});
        steps.push_back({t2, id, 2});
        steps.push_back({close, id, 3});
    };
    const auto add_close = [&](const types::Symbol& symbol,
                               const Decimal& price, types::Timestamp close) {
        const std::size_t id = all_walks.size();
        all_walks.push_back(Walk{&symbol, {price, price, price, price}});
        steps.push_back({close, id, 4});
    };
    // With fill-timeframe bars, the open of the last one walked for each
    // symbol: a strategy bar none of them opened inside is walked itself.
    std::vector<std::optional<types::Timestamp>> last_fill(
        needs.symbols.size());
    std::vector<std::optional<types::Timestamp>> last_fill_end(
        needs.symbols.size());
    std::size_t walked_without = 0;
    const auto index_of = [&](const Replayed& item) {
        return static_cast<std::size_t>(item.symbol - needs.symbols.data());
    };
    for (const Replayed& item : timeline) {
        if (item.is_quote) {
            continue;
        }
        const auto index = index_of(item);
        if (item.fill_only) {
            add_walk(*item.symbol, item.bar, item.time);
            last_fill[index] = item.bar.open_time;
            last_fill_end[index] = item.time;
        } else if (!needs.quotes && item.timeframe == driving[index]) {
            if (!fill_bars) {
                add_walk(*item.symbol, item.bar, item.time);
            } else if (!last_fill[index] ||
                       *last_fill[index] < item.bar.open_time) {
                add_walk(*item.symbol, item.bar, item.time);
                ++walked_without;
            } else if (!last_fill_end[index] ||
                       *last_fill_end[index] < item.time) {
                add_close(*item.symbol, item.bar.close, item.time);
            }
        }
    }
    std::stable_sort(steps.begin(), steps.end(),
                     [](const Step& a, const Step& b) {
                         if (a.time != b.time) {
                             return a.time < b.time;
                         }
                         // Steps inside or ending an open bar (1..4) run
                         // before a new bar opens at the same instant (0),
                         // with strategy closes delivered in between; a
                         // zero-length bar orders stages 1, 2, 3 across walks.
                         const auto rank = [](int stage) {
                             return stage == 0 ? 4 : std::min(stage, 3);
                         };
                         return rank(a.stage) < rank(b.stage);
                     });
    std::vector<Decimal> leg_at;
    const auto run_steps = [&](std::size_t from, std::size_t until,
                               types::Timestamp t) {
        if (from == until) {
            return;
        }
        for (std::size_t k = from; k < until; ++k) {
            const Step& step = steps[k];
            const Walk& w = all_walks[step.walk];
            const int point = step.stage == 4 ? 3 : step.stage;
            feed(*w.symbol, w.points[static_cast<std::size_t>(point)], t);
        }
        runner.Drain();
        leg_at.resize(until - from);
        for (std::size_t k = from; k < until; ++k) {
            if (steps[k].stage < 3) {
                leg_at[k - from] =
                    all_walks[steps[k].walk]
                        .points[static_cast<std::size_t>(steps[k].stage)];
            }
        }
        // Re-scan every leg in the group after each resting fill so an order
        // placed on an earlier walk by a later walk's fill triggers on the
        // same leg too.
        for (bool progressed = true; progressed;) {
            progressed = false;
            for (std::size_t k = from; k < until; ++k) {
                const Step& step = steps[k];
                if (step.stage >= 3) {
                    continue;
                }
                const Walk& w = all_walks[step.walk];
                const Decimal& start =
                    w.points[static_cast<std::size_t>(step.stage)];
                const Decimal& goal =
                    w.points[static_cast<std::size_t>(step.stage + 1)];
                const bool up = start < goal;
                Decimal& at = leg_at[k - from];
                std::optional<Decimal> next;
                for (const Decimal& price : venue->RestingPrices(*w.symbol)) {
                    const bool ahead = up ? at < price && price < goal
                                          : goal < price && price < at;
                    if (ahead &&
                        (!next || (up ? price < *next : *next < price))) {
                        next = price;
                    }
                }
                if (next) {
                    at = *next;
                    quote_at(*w.symbol, at, t);
                    progressed = true;
                }
            }
        }
    };
    std::vector<std::size_t> walk_group(all_walks.size(), 0);
    std::size_t next_group = 0;
    const auto next_step_batch = [&](std::size_t from, types::Timestamp t,
                                     bool opening) {
        const std::size_t stamp = ++next_group;
        std::size_t end = from;
        while (end < steps.size() && steps[end].time == t &&
               (steps[end].stage == 0) == opening &&
               walk_group[steps[end].walk] != stamp) {
            walk_group[steps[end].walk] = stamp;
            ++end;
        }
        return end;
    };
    std::vector<std::pair<types::Symbol, Decimal>> batch_marks;
    for (std::size_t s = 0, i = 0; s < steps.size() || i < timeline.size();) {
        types::Timestamp t = s < steps.size() ? steps[s].time
                                              : timeline[i].time;
        if (i < timeline.size() && timeline[i].time < t) {
            t = timeline[i].time;
        }
        while (i < timeline.size() && timeline[i].time == t &&
               timeline[i].is_quote) {
            const Replayed& item = timeline[i++];
            run_sessions(item.time);
            now = std::max(now, item.time);
            data->SetTime(now);
            data->EmitQuote(*item.quote);
            runner.Drain();
            observe(*item.symbol, SummaryBuilder::MarkOf(*item.quote));
        }
        // Stop before repeating a walk so zero-length bars step stage by stage.
        while (s < steps.size() && steps[s].time == t && steps[s].stage > 0) {
            const std::size_t s_end = next_step_batch(s, t, /*opening=*/false);
            run_steps(s, s_end, t);
            s = s_end;
        }
        while (i < timeline.size() && timeline[i].time == t) {
            const Replayed& first = timeline[i];
            std::size_t end = i + 1;
            while (end < timeline.size() && !timeline[end].is_quote &&
                   timeline[end].time == t &&
                   timeline[end].fill_only == first.fill_only &&
                   timeline[end].timeframe == first.timeframe) {
                ++end;
            }
            run_sessions(t);
            now = std::max(now, t);
            data->SetTime(now);
            batch_marks.clear();
            if (first.fill_only) {
                for (std::size_t k = i; k < end; ++k) {
                    batch_marks.emplace_back(*timeline[k].symbol,
                                             timeline[k].bar.close);
                }
                observe_batch(batch_marks);
                i = end;
                continue;
            }
            for (std::size_t k = i; k < end; ++k) {
                const Replayed& item = timeline[k];
                // What the close calls for trades at the next price, not this
                // one.
                venue->SetDeferMatching(!options.fill_at_close);
                data->Emit(*item.symbol, item.timeframe, item.bar);
                runner.Drain();
                venue->SetDeferMatching(false);
                // With quotes the venue trades on them, so they alone set the
                // marks.
                if (!needs.quotes) {
                    batch_marks.emplace_back(*item.symbol, item.bar.close);
                }
            }
            observe_batch(batch_marks);
            i = end;
        }
        while (s < steps.size() && steps[s].time == t && steps[s].stage == 0) {
            const std::size_t s_end = next_step_batch(s, t, /*opening=*/true);
            run_steps(s, s_end, t);
            s = s_end;
        }
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
