#include "kestrel/data/schwab_data_source.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>

#include "kestrel/data/symbols.hpp"
#include "kestrel/util/json.hpp"

namespace kestrel::data {

using types::ErrorCategory;
using types::MakeError;

namespace {

using namespace std::chrono;

// How much one price-history request covers. Schwab answers whole days, and
// takes ten days of minute bars and decades of daily ones in one request.
constexpr days kMinuteWindow{10};
constexpr days kLongWindow{20 * 365};
// Each request reaches a day further either side, as Schwab's days are not
// UTC days; the bars are trimmed to the range afterwards.
constexpr days kPad{1};
// An intraday range whose first bar comes later than this after its start
// began before Schwab's history (no market closes this long).
constexpr days kHistoryGap{7};

// When a bar that opens at `open` closes.
types::Timestamp BarEnd(types::Timestamp open, types::Timeframe tf) {
    if (const auto length = types::FixedDuration(tf)) {
        return open + *length;
    }
    // Monthly: the first of the next month.
    const year_month_day day{floor<days>(open)};
    const auto next = day.year() / day.month() / 1d + months(1);
    return sys_days{next};
}

}  // namespace

types::Result<SchwabFrequency> SchwabFrequencyFor(types::Timeframe tf) {
    using types::Timeframe;
    switch (tf) {
        case Timeframe::kM1:
            return SchwabFrequency{"minute", 1};
        case Timeframe::kM5:
            return SchwabFrequency{"minute", 5};
        case Timeframe::kM10:
            return SchwabFrequency{"minute", 10};
        case Timeframe::kM15:
            return SchwabFrequency{"minute", 15};
        case Timeframe::kM30:
            return SchwabFrequency{"minute", 30};
        case Timeframe::kD1:
            return SchwabFrequency{"daily", 1};
        case Timeframe::kW1:
            return SchwabFrequency{"weekly", 1};
        case Timeframe::kMo1:
            return SchwabFrequency{"monthly", 1};
        default:
            return MakeError(ErrorCategory::kUnsupported,
                             "Schwab does not offer " +
                                 std::string(types::ToString(tf)) + " bars");
    }
}

SchwabDataSource::SchwabDataSource(std::shared_ptr<SchwabApi> api,
                                   Options options)
    : mApi(std::move(api)),
      mOptions(std::move(options)),
      mStreamer(std::make_shared<SchwabStreamer>(
          mApi, mOptions.trader_url, mOptions.make_socket,
          mOptions.takeover_reconnect, mOptions.clock)) {}

types::Result<std::vector<types::Bar>> SchwabDataSource::GetBars(
    const types::Symbol& symbol, types::Timeframe tf, types::Timestamp start,
    types::Timestamp end) {
    auto frequency = SchwabFrequencyFor(tf);
    if (!frequency) {
        return std::unexpected(frequency.error());
    }
    auto native = ToSchwabSymbol(symbol);
    if (!native) {
        return std::unexpected(native.error());
    }
    if (end < start) {
        return MakeError(ErrorCategory::kInvalidArgument,
                         "GetBars: end precedes start");
    }
    const bool intraday = frequency->frequency_type == "minute";
    const auto window = intraday ? kMinuteWindow : kLongWindow;
    const types::Timestamp now = mOptions.clock();

    std::vector<types::Bar> bars;
    for (types::Timestamp since = start; since < end;) {
        const types::Timestamp until = std::min<types::Timestamp>(
            end, since + duration_cast<milliseconds>(window));
        net::HttpRequest request;
        request.url = mOptions.base_url + "/pricehistory";
        request.query = {
            {"symbol", *native},
            // Minute bars come only with periodType=day; daily, weekly and
            // monthly all come with year.
            {"periodType", intraday ? "day" : "year"},
            {"frequencyType", std::string(frequency->frequency_type)},
            {"frequency", std::to_string(frequency->frequency)},
            {"startDate", std::to_string(types::ToEpochMillis(since - kPad))},
            {"endDate", std::to_string(types::ToEpochMillis(until + kPad))}};
        if (intraday) {
            // Schwab's default is true.
            request.query["needExtendedHoursData"] =
                mOptions.extended_hours ? "true" : "false";
        }
        auto response = mApi->Send(std::move(request));
        if (!response) {
            auto error = response.error();
            error.message =
                "schwab pricehistory " + *native + ": " + error.message;
            return std::unexpected(std::move(error));
        }
        auto json = util::ParseJson(response->body);
        if (!json) {
            return std::unexpected(json.error());
        }
        const auto candles = json->find("candles");
        if (!json->is_object() || candles == json->end() ||
            !candles->is_array()) {
            return MakeError(ErrorCategory::kParse,
                             "schwab pricehistory: no candles array");
        }
        for (const auto& candle : *candles) {
            const auto datetime = candle.find("datetime");
            if (!candle.is_object() || datetime == candle.end() ||
                !datetime->is_number_integer()) {
                return MakeError(ErrorCategory::kParse,
                                 "schwab pricehistory: malformed candle");
            }
            types::Bar bar;
            bar.open_time =
                types::FromEpochMillis(datetime->get<std::int64_t>());
            if (!intraday) {
                bar.open_time = floor<days>(bar.open_time);
            }
            for (auto [field, out] : {std::pair{"open", &bar.open},
                                      {"high", &bar.high},
                                      {"low", &bar.low},
                                      {"close", &bar.close},
                                      {"volume", &bar.volume}}) {
                const auto value = candle.find(field);
                if (value == candle.end()) {
                    return MakeError(ErrorCategory::kParse,
                                     std::string("schwab pricehistory: "
                                                 "candle without ") +
                                         field);
                }
                auto decimal = util::JsonDecimal(*value);
                if (!decimal) {
                    return std::unexpected(decimal.error());
                }
                *out = *decimal;
            }
            if (bar.open_time >= start && bar.open_time < end &&
                BarEnd(bar.open_time, tf) <= now) {
                bars.push_back(bar);
            }
        }
        since = until;
    }
    // The padded requests overlap.
    std::ranges::sort(bars, {}, &types::Bar::open_time);
    const auto dupes = std::ranges::unique(bars, {}, &types::Bar::open_time);
    bars.erase(dupes.begin(), dupes.end());

    if (intraday && start + kHistoryGap < std::min(end, now) &&
        (bars.empty() || bars.front().open_time > start + kHistoryGap)) {
        spdlog::warn(
            "schwab: {} {} bars start {} although {} was asked for; Schwab "
            "keeps a limited intraday history (about 48 days of 1-minute "
            "bars)",
            *native, types::ToString(tf),
            bars.empty() ? std::string("nowhere")
                         : types::FormatIso8601(bars.front().open_time),
            types::FormatIso8601(start));
    }
    return bars;
}

types::Result<std::unique_ptr<Subscription>> SchwabDataSource::StreamQuotes(
    const std::vector<std::string>& symbols, QuoteCallback callback) {
    if (symbols.empty()) {
        return MakeError(ErrorCategory::kInvalidArgument,
                         "StreamQuotes: no symbols");
    }
    if (!callback) {
        return MakeError(ErrorCategory::kInvalidArgument,
                         "StreamQuotes: no callback");
    }
    auto keys = Keys(symbols);
    if (!keys) {
        return std::unexpected(keys.error());
    }
    return mStreamer->SubscribeQuotes(*keys, std::move(callback));
}

types::Result<std::unique_ptr<Subscription>> SchwabDataSource::StreamBars(
    const std::vector<std::string>& symbols, types::Timeframe timeframe,
    BarCallback callback) {
    using types::Timeframe;
    if (symbols.empty()) {
        return MakeError(ErrorCategory::kInvalidArgument,
                         "StreamBars: no symbols");
    }
    if (!callback) {
        return MakeError(ErrorCategory::kInvalidArgument,
                         "StreamBars: no callback");
    }
    if (timeframe != Timeframe::kM1 && timeframe != Timeframe::kM5 &&
        timeframe != Timeframe::kM10 && timeframe != Timeframe::kM15 &&
        timeframe != Timeframe::kM30) {
        return MakeError(ErrorCategory::kUnsupported,
                         "Schwab streams 1m, 5m, 10m, 15m and 30m bars, not " +
                             std::string(types::ToString(timeframe)));
    }
    auto keys = Keys(symbols);
    if (!keys) {
        return std::unexpected(keys.error());
    }
    if (!mOptions.extended_hours) {
        try {
            (void)InUsRegularSession(types::Now());
        } catch (const std::exception& e) {
            return MakeError(ErrorCategory::kUnsupported,
                             "StreamBars needs New York time for the regular "
                             "session, and the time zone database is "
                             "missing (install tzdata): " +
                                 std::string(e.what()));
        }
        callback = [inner = std::move(callback)](const types::Symbol& symbol,
                                                 Timeframe tf,
                                                 const types::Bar& bar) {
            // The session's edges are on every timeframe's boundaries, so a
            // bar is all in it or all out of it.
            if (InUsRegularSession(bar.open_time)) {
                inner(symbol, tf, bar);
            }
        };
    }
    return mStreamer->SubscribeBars(*keys, timeframe, std::move(callback));
}

types::Result<std::vector<std::string>> SchwabDataSource::Keys(
    const std::vector<std::string>& symbols) const {
    std::vector<std::string> keys;
    for (const auto& symbol : symbols) {
        auto normalized = types::NormalizeSymbol(symbol);
        if (!normalized) {
            return std::unexpected(normalized.error());
        }
        auto native = ToSchwabSymbol(*normalized);
        if (!native) {
            return std::unexpected(native.error());
        }
        keys.push_back(std::move(*native));
    }
    return keys;
}

types::Result<types::SymbolInfo> SchwabDataSource::GetSymbolInfo(
    const types::Symbol& symbol) {
    auto normalized = types::NormalizeSymbol(symbol);
    if (!normalized) {
        return std::unexpected(normalized.error());
    }
    if (auto native = ToSchwabSymbol(*normalized); !native) {
        return std::unexpected(native.error());
    }
    return types::SymbolInfo{*normalized, types::Decimal(1, 2),
                             types::Decimal::FromInt(1),
                             types::Decimal::FromInt(1), types::Decimal()};
}

}  // namespace kestrel::data
