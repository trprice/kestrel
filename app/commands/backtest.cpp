#include "kestrel/backtest/backtest.hpp"

#include <spdlog/spdlog.h>

#include <fstream>
#include <iostream>

#include "command.hpp"
#include "common.hpp"
#include "kestrel/backtest/replay_data_source.hpp"
#include "kestrel/factory.hpp"

namespace kestrel::app {

namespace {

class BacktestCommand : public Command {
   public:
    BacktestCommand(std::string strategy_name, types::Timestamp from,
                    types::Timestamp to, bool quotes_from_trades,
                    bool fill_at_close,
                    std::optional<types::Timeframe> fill_timeframe,
                    std::string equity_csv)
        : mName(std::move(strategy_name)),
          mFrom(from),
          mTo(to),
          mQuotesFromTrades(quotes_from_trades),
          mFillAtClose(fill_at_close),
          mFillTimeframe(fill_timeframe),
          mEquityCsv(std::move(equity_csv)) {}

    int Run(const config::Configuration& config) override {
        // Opened first, so a bad path is refused before any data is loaded.
        std::ofstream csv;
        if (!mEquityCsv.empty()) {
            csv.open(mEquityCsv);
            if (!csv) {
                spdlog::error("--equity-csv: cannot write {}", mEquityCsv);
                return 2;
            }
        }
        auto limits = ResolveRiskLimits(mName, config);
        if (!limits) {
            spdlog::error("risk: {}", limits.error().ToString());
            return 1;
        }
        auto strategy = CreateStrategy(mName, config);
        if (!strategy) {
            spdlog::error("strategy: {}", strategy.error().ToString());
            return 1;
        }
        auto source = CreateDataSource(config);
        if (!source) {
            spdlog::error("startup: {}", source.error().ToString());
            return 1;
        }
        // The database is optional: without a [database] section, or if it
        // cannot be reached, everything comes from the venue.
        std::unique_ptr<db::BarStore> bar_store;
        std::unique_ptr<db::QuoteStore> quote_store;
        std::unique_ptr<db::MarketTradeStore> trade_store;
        backtest::StoredData stored;
        stored.quotes_from_trades = mQuotesFromTrades;
        auto database = OpenDatabase(config);
        if (database) {
            bar_store = std::make_unique<db::BarStore>(*database);
            quote_store = std::make_unique<db::QuoteStore>(*database);
            trade_store = std::make_unique<db::MarketTradeStore>(*database);
            stored.bars = bar_store.get();
            stored.quotes = quote_store.get();
            stored.trades = trade_store.get();
        } else if (database.error().category !=
                   types::ErrorCategory::kNotFound) {
            spdlog::warn("database unavailable, using the venue: {}",
                         database.error().ToString());
        }
        // The fill timeframe's bars are loaded with the strategy's own.
        strategy::Requirements needs = (*strategy)->Needs();
        if (auto checked = backtest::CheckFillTimeframe(needs, mFillTimeframe);
            !checked) {
            return Fail("--fill-timeframe", checked.error());
        }
        if (mFillTimeframe &&
            std::ranges::find(needs.bar_timeframes, *mFillTimeframe) ==
                needs.bar_timeframes.end()) {
            needs.bar_timeframes.push_back(*mFillTimeframe);
        }
        auto replay =
            backtest::LoadReplayData(**source, needs, mFrom, mTo, stored);
        if (!replay) {
            return Fail("data", replay.error());
        }

        backtest::BacktestOptions options;
        options.paper = config.GetPaperOptions();
        options.risk = *limits;
        options.start = mFrom;
        options.end = mTo;
        options.fill_at_close = mFillAtClose;
        options.fill_timeframe = mFillTimeframe;
        auto result =
            backtest::RunBacktest(*replay, std::move(*strategy), options);
        if (!result) {
            return Fail("backtest", result.error());
        }
        PrintReport(*result);
        if (!result->summary) {
            std::cout << "summary: unavailable: "
                      << result->summary.error().message << '\n';
        } else {
            PrintSummary(*result->summary);
            if (csv.is_open()) {
                WriteCurve(csv, *result->summary);
                if (!csv) {
                    spdlog::error("--equity-csv: writing {} failed",
                                  mEquityCsv);
                    return 1;
                }
                spdlog::info("equity curve: {} points written to {}",
                             result->summary->curve.size(), mEquityCsv);
            }
        }
        return 0;
    }

   private:
    // A bad range is a usage error; anything else a failure.
    static int Fail(const char* what, const types::Error& error) {
        spdlog::error("{}: {}", what, error.ToString());
        return error.category == types::ErrorCategory::kInvalidArgument ? 2 : 1;
    }

    static void PrintReport(const backtest::BacktestResult& result) {
        std::cout << "bars replayed: " << result.bars_replayed
                  << "\nquotes replayed: " << result.quotes_replayed;
        if (result.fill_bars_replayed > 0 ||
            result.bars_walked_without_fill_bars > 0) {
            std::cout << "\nfill bars replayed: " << result.fill_bars_replayed
                      << " (strategy bars walked without them: "
                      << result.bars_walked_without_fill_bars << ')';
        }
        std::cout << "\nfills: " << result.fills.size() << '\n';
        for (const auto& f : result.fills) {
            std::cout << types::FormatIso8601(f.time) << ' ' << f.symbol << ' '
                      << (f.side == types::Side::kBuy ? "buy" : "sell") << ' '
                      << f.quantity << " @ " << f.price << " fee=" << f.fee
                      << ' ' << f.fee_asset << '\n';
        }
        for (const auto& p : result.positions) {
            std::cout << "position " << p.symbol << ": qty=" << p.quantity
                      << " avg=" << p.average_price
                      << " realized=" << p.realized_pnl << '\n';
        }
        for (const auto& b : result.balances) {
            std::cout << "balance " << b.asset << ": total=" << b.total << '\n';
        }
    }

    // Amounts to 8 places without trailing zeros; fractions as percentages.
    static std::string Amount(const types::Decimal& d) {
        return d.Rescale(8, types::Rounding::kNearest).Normalized().ToString();
    }
    static std::string Signed(const types::Decimal& d) {
        return (d.Sign() > 0 ? "+" : "") + Amount(d);
    }
    static std::string Percent(const types::Decimal& fraction) {
        const auto pct = (fraction * types::Decimal(100, 0))
                             .Rescale(2, types::Rounding::kNearest);
        return (pct.Sign() > 0 ? "+" : "") + pct.ToString() + "%";
    }

    static void PrintSummary(const backtest::BacktestSummary& s) {
        std::cout << "summary (" << s.quote_asset << "), "
                  << types::FormatIso8601(s.start_time) << " to "
                  << types::FormatIso8601(s.end_time) << '\n';
        std::cout << "  start equity   " << Amount(s.start_equity) << '\n'
                  << "  end equity     " << Amount(s.end_equity) << '\n'
                  << "  return         " << Percent(s.return_fraction) << " ("
                  << Signed(s.end_equity - s.start_equity) << ")\n"
                  << "  hold start     " << Amount(s.hold_equity)
                  << "  (the starting balances valued at the end)\n"
                  << "  vs hold        " << Signed(s.end_equity - s.hold_equity)
                  << '\n'
                  << "  max drawdown   " << Percent(-s.max_drawdown_fraction)
                  << " (" << Signed(-s.max_drawdown);
        if (!s.max_drawdown.IsZero()) {
            std::cout << ", " << types::FormatIso8601(s.drawdown_peak_time)
                      << " to " << types::FormatIso8601(s.drawdown_trough_time);
        }
        std::cout << ")\n"
                  << "  realized       " << Signed(s.realized) << '\n'
                  << "  unrealized     " << Signed(s.unrealized) << '\n'
                  << "  fills          " << s.fills << " (" << s.buys
                  << " buy, " << s.sells << " sell; " << s.maker_fills
                  << " maker, " << s.taker_fills << " taker)\n"
                  << "  volume         " << Amount(s.volume) << '\n'
                  << "  fees           " << Amount(s.fees) << '\n'
                  << "  closing sells  " << s.closing_sells << ", "
                  << s.winning_sells << " won";
        if (s.closing_sells > 0) {
            std::cout << " (" << s.winning_sells * 100 / s.closing_sells
                      << "%)";
        }
        std::cout << '\n';
        if (s.fills > 0) {
            std::cout
                << "  markouts       against the mid, fees not included\n";
            for (const auto& m : s.markouts) {
                std::string label =
                    !m.horizon                ? "at the end"
                    : m.horizon->count() == 0 ? "at the fill"
                    : m.horizon->count() % 60000 == 0
                        ? std::to_string(m.horizon->count() / 60000) + " min"
                        : std::to_string(m.horizon->count() / 1000) + " s";
                label.resize(std::max<std::size_t>(label.size(), 12), ' ');
                std::cout << "    " << label << ' '
                          << (m.bps.Sign() > 0 ? "+" : "") << m.bps.ToString()
                          << " bps (" << Signed(m.pnl) << ", " << m.fills
                          << " fills)\n";
            }
        }
        for (const auto& sym : s.symbols) {
            std::cout << "  " << sym.symbol << "  " << Amount(sym.first_mark)
                      << " -> " << Amount(sym.last_mark) << "; held "
                      << Amount(sym.min_held) << " .. " << Amount(sym.max_held)
                      << ' ' << sym.base << " (start " << Amount(sym.start_held)
                      << ", end " << Amount(sym.end_held) << ")\n";
        }
    }

    static void WriteCurve(std::ostream& out,
                           const backtest::BacktestSummary& s) {
        out << "time,equity,hold_equity";
        for (const auto& sym : s.symbols) {
            out << ",mark " << sym.symbol << ",held " << sym.base;
        }
        out << '\n';
        for (const auto& point : s.curve) {
            out << types::FormatIso8601(point.time) << ','
                << Amount(point.equity) << ',' << Amount(point.hold_equity);
            for (std::size_t i = 0; i < point.marks.size(); ++i) {
                out << ',' << Amount(point.marks[i]) << ','
                    << Amount(point.held[i]);
            }
            out << '\n';
        }
    }

    std::string mName;
    types::Timestamp mFrom;
    types::Timestamp mTo;
    bool mQuotesFromTrades;
    bool mFillAtClose;
    std::optional<types::Timeframe> mFillTimeframe;
    std::string mEquityCsv;
};

struct Args {
    std::string strategy;
    std::string from;
    std::string to;
    bool quotes_from_trades = false;
    bool fill_at_close = false;
    std::string fill_timeframe;
    std::string equity_csv;
};

}  // namespace

void AddBacktestCommand(CLI::App& app, CommandSlot& slot) {
    auto* sub = AddSubcommand(
        app, "backtest",
        "Backtest the named strategy over --from..--to using stored bars and "
        "quotes from the database, falling back to the provider for missing "
        "bars");
    auto args = std::make_shared<Args>();
    sub->add_option("strategy", args->strategy, "Strategy name")->required();
    sub->add_option("--from", args->from,
                    "Start, YYYY-MM-DD or an ISO 8601 time (UTC)")
        ->required()
        ->check(DateValidator())
        ->type_name("DATE");
    sub->add_option("--to", args->to, "End (exclusive), same format as --from")
        ->required()
        ->check(DateValidator())
        ->type_name("DATE");
    sub->add_flag("--quotes-from-trades", args->quotes_from_trades,
                  "Replay quotes rebuilt from stored trades (`trades --store`) "
                  "instead of recorded quotes; approximate, see "
                  "docs/database.md");
    sub->add_flag("--fill-at-close", args->fill_at_close,
                  "Fill an order placed on a bar's close at that close, not "
                  "at the next price replayed (the next bar's open); "
                  "optimistic");
    sub->add_option("--fill-timeframe", args->fill_timeframe,
                    "Fill orders on bars of this finer timeframe (e.g. 1m), "
                    "walked in place of the strategy's own, so the path within "
                    "a bar is the recorded one")
        ->check(TimeframeValidator(true))
        ->type_name("TIMEFRAME");
    sub->add_option("--equity-csv", args->equity_csv,
                    "Also write the equity curve, sampled every minute, to "
                    "this CSV file")
        ->type_name("FILE");
    sub->callback([&slot, args] {
        slot = std::make_unique<BacktestCommand>(
            args->strategy, *types::ParseTimestamp(args->from),
            *types::ParseTimestamp(args->to), args->quotes_from_trades,
            args->fill_at_close,
            args->fill_timeframe.empty()
                ? std::nullopt
                : std::optional(*types::ParseTimeframe(args->fill_timeframe)),
            args->equity_csv);
    });
}

}  // namespace kestrel::app
