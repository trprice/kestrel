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

SchwabCalendar::SchwabCalendar(std::shared_ptr<SchwabApi> api,
                               std::string base_url,
                               std::function<types::Timestamp()> clock)
    : mApi(std::move(api)),
      mBaseUrl(std::move(base_url)),
      mClock(std::move(clock)),
      mWorker([this] { Work(); }) {}

SchwabCalendar::~SchwabCalendar() {
    {
        std::lock_guard lock(mMutex);
        mStopping = true;
    }
    mWake.notify_all();
    mWorker.join();
}

year_month_day SchwabCalendar::DateAt(types::Timestamp t) const {
    return mRules.DateAt(t);
}

types::Result<std::optional<types::TradingDay>>
SchwabCalendar::ParseMarketHours(std::string_view body) {
    auto json = util::ParseJson(body);
    if (!json) {
        return std::unexpected(json.error());
    }
    // {"equity": {"EQ": {...}}}; the inner key is "equity" on a closed day.
    const auto equity = json->is_object() ? json->find("equity") : json->end();
    if (equity == json->end() || !equity->is_object() || equity->empty() ||
        !equity->begin()->is_object()) {
        return MakeError(ErrorCategory::kParse,
                         "schwab markets: no equity hours");
    }
    const auto& day = *equity->begin();
    const auto open = day.find("isOpen");
    if (open == day.end() || !open->is_boolean()) {
        return MakeError(ErrorCategory::kParse, "schwab markets: no isOpen");
    }
    if (!open->get<bool>()) {
        return std::optional<types::TradingDay>{};
    }
    // The first interval's start or the last one's end of a session; nullopt
    // if the session is absent, an error if it is there but malformed.
    const auto hours = day.find("sessionHours");
    const auto edge =
        [&](const char* session,
            bool start) -> types::Result<std::optional<types::Timestamp>> {
        if (hours == day.end() || !hours->is_object()) {
            return std::optional<types::Timestamp>{};
        }
        const auto list = hours->find(session);
        if (list == hours->end()) {
            return std::optional<types::Timestamp>{};
        }
        const char* field = start ? "start" : "end";
        if (!list->is_array() || list->empty()) {
            return MakeError(ErrorCategory::kParse,
                             std::string("schwab markets: bad ") + session);
        }
        const auto& interval = start ? list->front() : list->back();
        const auto text =
            interval.is_object() ? interval.find(field) : interval.end();
        if (text == interval.end() || !text->is_string()) {
            return MakeError(ErrorCategory::kParse,
                             std::string("schwab markets: ") + session +
                                 " without its " + field);
        }
        auto t = types::ParseIso8601(text->get<std::string>());
        if (!t) {
            return std::unexpected(t.error());
        }
        return std::optional(*t);
    };
    auto regular_open = edge("regularMarket", true);
    auto regular_close = edge("regularMarket", false);
    auto pre_open = edge("preMarket", true);
    auto post_close = edge("postMarket", false);
    for (const auto* r :
         {&regular_open, &regular_close, &pre_open, &post_close}) {
        if (!*r) {
            return std::unexpected(r->error());
        }
    }
    if (!*regular_open || !*regular_close) {
        return MakeError(ErrorCategory::kParse,
                         "schwab markets: an open day without regular hours");
    }
    return std::optional(types::TradingDay{
        pre_open->value_or(**regular_open), **regular_open, **regular_close,
        post_close->value_or(**regular_close)});
}

namespace {

std::string DateText(year_month_day date) {
    char text[16];
    std::snprintf(
        text, sizeof text, "%04d-%02u-%02u", static_cast<int>(date.year()),
        static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day()));
    return text;
}

std::string Describe(const std::optional<types::TradingDay>& day) {
    return day ? types::FormatIso8601(day->open) + " to " +
                     types::FormatIso8601(day->close)
               : std::string("closed");
}

}  // namespace

types::Result<std::optional<types::TradingDay>> SchwabCalendar::Ask(
    year_month_day date) const {
    net::HttpRequest request;
    request.url = mBaseUrl + "/markets";
    request.query = {{"markets", "equity"}, {"date", DateText(date)}};
    auto response = mApi->Send(std::move(request));
    if (!response) {
        return std::unexpected(response.error());
    }
    auto parsed = ParseMarketHours(response->body);
    if (parsed && *parsed && mRules.DateAt((**parsed).open) != date) {
        return MakeError(ErrorCategory::kParse,
                         "schwab markets: the hours are for another date");
    }
    return parsed;
}

void SchwabCalendar::Work() {
    std::unique_lock lock(mMutex);
    const auto ready = [this] { return mStopping || !mQueue.empty(); };
    for (;;) {
        if (!mDeferred.empty() && mClock() >= mRetryAt) {
            // The wait after a failure is over: ask about the dates it held
            // back, which no caller may ask about again.
            for (const year_month_day& date : mDeferred) {
                if (!mDays.contains(date) && mQueued.insert(date).second) {
                    mQueue.push_back(date);
                }
            }
            mDeferred.clear();
        }
        if (mDeferred.empty()) {
            mWake.wait(lock, ready);
        } else {
            // mRetryAt is on mClock, which need not be the system clock:
            // look again every 30 s.
            mWake.wait_for(lock, std::chrono::seconds(30), ready);
        }
        if (mStopping) {
            return;
        }
        if (mQueue.empty()) {
            continue;
        }
        const year_month_day date = mQueue.front();
        mQueue.pop_front();
        if (mClock() < mRetryAt) {
            mQueued.erase(date);
            mDeferred.insert(date);  // asked once the wait is over
            mIdle.notify_all();
            continue;
        }
        lock.unlock();
        auto schwab = Ask(date);
        // What to log, once the lock is released.
        std::optional<std::pair<spdlog::level::level_enum, std::string>> note;
        if (!schwab) {
            note.emplace(spdlog::level::warn,
                         "schwab: cannot read the market hours for " +
                             DateText(date) + " (" + schwab.error().ToString() +
                             "); using the US equity rules, and asking again "
                             "in 10 minutes");
        }
        lock.lock();
        mQueued.erase(date);
        if (!schwab) {
            mRetryAt = mClock() + minutes(10);
        } else {
            // Only dates the rules open are asked about. Schwab can only
            // restrict them: each time from whichever is more restrictive.
            const auto rules = mRules.On(date);
            std::optional<types::TradingDay> chosen;
            if (*schwab && rules) {
                types::TradingDay day{
                    std::max((*schwab)->pre_open, rules->pre_open),
                    std::max((*schwab)->open, rules->open),
                    std::min((*schwab)->close, rules->close),
                    std::min((*schwab)->post_close, rules->post_close)};
                if (day.open < day.close) {
                    chosen = day;
                }
            }
            if (*schwab != rules) {
                note.emplace(chosen != rules ? spdlog::level::info
                                             : spdlog::level::debug,
                             "schwab: market hours for " + DateText(date) +
                                 " differ from the US equity rules (Schwab: " +
                                 Describe(*schwab) +
                                 "; rules: " + Describe(rules) + "); using " +
                                 Describe(chosen));
            }
            mDays.emplace(date, chosen);
        }
        if (mQueue.empty()) {
            mIdle.notify_all();
        }
        if (note) {
            lock.unlock();
            spdlog::log(note->first, "{}", note->second);
            lock.lock();
        }
    }
}

void SchwabCalendar::WaitForRequests() const {
    std::unique_lock lock(mMutex);
    mIdle.wait(lock, [this] { return mQueued.empty(); });
}

std::optional<types::TradingDay> SchwabCalendar::On(year_month_day date) const {
    // A day the rules close stays closed whatever Schwab says.
    auto rules = mRules.On(date);
    if (!rules) {
        return std::nullopt;
    }
    std::lock_guard lock(mMutex);
    if (const auto it = mDays.find(date); it != mDays.end()) {
        return it->second;
    }
    const auto now = mClock();
    const sys_days today{mRules.DateAt(now)};
    // Schwab accepts 7 days back and a year ahead; keep inside both.
    const bool in_window = sys_days{date} >= today - days(6) &&
                           sys_days{date} <= today + days(364);
    if (in_window && now >= mRetryAt && mQueued.insert(date).second) {
        mQueue.push_back(date);
        mWake.notify_one();
    }
    return rules;  // until Schwab's answer is in
}

SchwabDataSource::SchwabDataSource(std::shared_ptr<SchwabApi> api,
                                   Options options)
    : mApi(std::move(api)),
      mOptions(std::move(options)),
      mStreamer(std::make_shared<SchwabStreamer>(
          mApi, mOptions.trader_url, mOptions.make_socket,
          mOptions.takeover_reconnect, mOptions.clock)) {
    auto calendar = std::make_shared<const SchwabCalendar>(
        mApi, mOptions.base_url, mOptions.clock);
    mHours = std::make_shared<const types::MarketHours>(
        calendar, mOptions.extended_hours);
    mRegular = std::make_shared<const types::MarketHours>(calendar, false);
}

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
        auto response = util::Retry(
            mOptions.retry, [&] { return mApi->Send(request); },
            mOptions.sleeper);
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
            // Loads the time zone, without asking Schwab anything.
            (void)types::UsEquityCalendar().DateAt(types::Now());
        } catch (const std::exception& e) {
            return MakeError(ErrorCategory::kUnsupported,
                             "StreamBars needs New York time for the regular "
                             "session, and the time zone database is "
                             "missing (install tzdata): " +
                                 std::string(e.what()));
        }
        callback = [inner = std::move(callback), regular = mRegular](
                       const types::Symbol& symbol, Timeframe tf,
                       const types::Bar& bar) {
            // The session's edges (09:30, 13:00, 16:00) are on every
            // timeframe's boundaries, so a bar is all in it or all out of it.
            if (regular->IsOpen(bar.open_time)) {
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
    auto native = ToSchwabSymbol(*normalized);
    if (!native) {
        return std::unexpected(native.error());
    }
    {
        std::lock_guard lock(mSymbolMutex);
        if (const auto it = mSymbols.find(*normalized); it != mSymbols.end()) {
            return it->second;
        }
    }
    // A quote says whether Schwab knows the symbol, what it is, and its
    // price, which sets the tick.
    net::HttpRequest request;
    request.url = mOptions.base_url + "/quotes";
    request.query = {{"symbols", *native}, {"fields", "quote"}};
    auto response = util::Retry(
        mOptions.retry, [&] { return mApi->Send(request); }, mOptions.sleeper);
    if (!response) {
        auto error = response.error();
        error.message = "schwab quotes " + *native + ": " + error.message;
        return std::unexpected(std::move(error));
    }
    auto json = util::ParseJson(response->body);
    if (!json || !json->is_object()) {
        return MakeError(ErrorCategory::kParse, "schwab quotes: not an object");
    }
    const auto entry = json->find(*native);
    if (entry == json->end() || !entry->is_object()) {
        // Unknown symbols come back as {"errors":{"invalidSymbols":[...]}}.
        return MakeError(ErrorCategory::kNotFound,
                         "Schwab does not know the symbol " + *normalized);
    }
    const std::string type = entry->value("assetMainType", "");
    if (type != "EQUITY") {
        return MakeError(
            ErrorCategory::kUnsupported,
            *normalized + " is " +
                (type.empty() ? std::string("of unknown type") : "a " + type) +
                " at Schwab; only stocks and ETFs are supported");
    }
    // The last price, or the bid or ask if nothing has traded.
    std::optional<types::Decimal> price;
    if (const auto quote = entry->find("quote");
        quote != entry->end() && quote->is_object()) {
        for (const char* field : {"lastPrice", "bidPrice", "askPrice"}) {
            const auto it = quote->find(field);
            if (it == quote->end()) {
                continue;
            }
            if (auto d = util::JsonDecimal(*it); d && d->Sign() > 0) {
                price = *d;
                break;
            }
        }
    }
    // US stocks quote in cents from $1, in hundredths of a cent below it.
    const bool sub_dollar = price && *price < types::Decimal::FromInt(1);
    types::SymbolInfo info{
        *normalized, sub_dollar ? types::Decimal(1, 4) : types::Decimal(1, 2),
        types::Decimal::FromInt(1), types::Decimal::FromInt(1),
        types::Decimal()};
    std::lock_guard lock(mSymbolMutex);
    mSymbols.emplace(*normalized, info);
    return info;
}

}  // namespace kestrel::data
