#include "kestrel/data/binanceus_data_source.hpp"

#include <spdlog/spdlog.h>

#include "kestrel/data/binanceus_bar_stream.hpp"
#include "kestrel/data/binanceus_quote_stream.hpp"
#include "kestrel/data/symbols.hpp"
#include "kestrel/util/json.hpp"

namespace kestrel::data {

using nlohmann::json;
using types::ErrorCategory;
using types::MakeError;
using types::Result;

namespace {

constexpr std::size_t kMaxBarsPerRequest = 1000;

// Binance error bodies look like {"code":-1121,"msg":"Invalid symbol."}.
// Refines the generic HTTP-status error using that body when present.
types::Error RefineError(types::Error err) {
    const auto brace = err.message.find('{');
    if (brace == std::string::npos) {
        return err;
    }
    auto body = util::ParseJson(std::string_view(err.message).substr(brace));
    if (!body || !body->is_object() || !body->contains("code") ||
        !(*body)["code"].is_number_integer()) {
        return err;
    }
    const auto code = (*body)["code"].get<std::int64_t>();
    err.provider_code = std::to_string(code);
    if (code == -1121) {
        err.category = ErrorCategory::kNotFound;
    } else if (code == -1003) {
        err.category = ErrorCategory::kRateLimit;
    } else if (code == -2014 || code == -2015 || code == -1022) {
        err.category = ErrorCategory::kAuth;
    }
    return err;
}

}  // namespace

types::Result<std::string_view> BinanceUSInterval(types::Timeframe tf) {
    using types::Timeframe;
    switch (tf) {
        case Timeframe::kM1:
            return "1m";
        case Timeframe::kM3:
            return "3m";
        case Timeframe::kM5:
            return "5m";
        case Timeframe::kM15:
            return "15m";
        case Timeframe::kM30:
            return "30m";
        case Timeframe::kH1:
            return "1h";
        case Timeframe::kH2:
            return "2h";
        case Timeframe::kH4:
            return "4h";
        case Timeframe::kH6:
            return "6h";
        case Timeframe::kH8:
            return "8h";
        case Timeframe::kH12:
            return "12h";
        case Timeframe::kD1:
            return "1d";
        case Timeframe::kD3:
            return "3d";
        case Timeframe::kW1:
            return "1w";
        case Timeframe::kMo1:
            return "1M";
        default:
            return MakeError(ErrorCategory::kUnsupported,
                             "Binance.US does not offer " +
                                 std::string(types::ToString(tf)) + " bars");
    }
}

BinanceUSDataSource::BinanceUSDataSource(std::shared_ptr<net::HttpClient> http,
                                         Options options)
    : mHttp(std::move(http)),
      mOptions(std::move(options)),
      mLimiter(mOptions.requests_per_second, mOptions.request_burst) {}

BinanceUSDataSource::BinanceUSDataSource(std::shared_ptr<net::HttpClient> http)
    : BinanceUSDataSource(std::move(http), Options{}) {}

BinanceUSDataSource::BinanceUSDataSource()
    : BinanceUSDataSource(std::make_shared<net::CprHttpClient>(), Options{}) {}

Result<json> BinanceUSDataSource::Call(
    const std::string& path, std::map<std::string, std::string> query) {
    net::HttpRequest request;
    request.url = mOptions.rest_base_url + "/api/v3/" + path;
    request.query = std::move(query);

    return util::Retry(
        mOptions.retry,
        [&]() -> Result<json> {
            mLimiter.Acquire();
            auto response = mHttp->Send(request);
            if (!response) {
                auto err = RefineError(response.error());
                if (err.category == ErrorCategory::kRateLimit) {
                    mLimiter.Penalize();
                }
                spdlog::debug("binanceus {}: {}", path, err.ToString());
                return std::unexpected(std::move(err));
            }
            return util::ParseJson(response->body);
        },
        mOptions.sleeper);
}

Result<std::vector<types::Bar>> BinanceUSDataSource::GetBars(
    const types::Symbol& symbol, types::Timeframe timeframe,
    types::Timestamp start, types::Timestamp end) {
    using namespace std::chrono;
    auto interval = BinanceUSInterval(timeframe);
    if (!interval) {
        return std::unexpected(interval.error());
    }
    auto native = ToBinanceUSSymbol(symbol);
    if (!native) {
        return std::unexpected(native.error());
    }
    if (end < start) {
        return MakeError(ErrorCategory::kInvalidArgument,
                         "GetBars: end precedes start");
    }

    const types::Timestamp now = mOptions.clock();

    std::vector<types::Bar> bars;
    types::Timestamp since = start;
    while (since < end) {
        // endTime is inclusive, while `end` is exclusive.
        auto result =
            Call("klines",
                 {{"symbol", *native},
                  {"interval", std::string(*interval)},
                  {"startTime", std::to_string(types::ToEpochMillis(since))},
                  {"endTime", std::to_string(types::ToEpochMillis(end) - 1)},
                  {"limit", std::to_string(kMaxBarsPerRequest)}});
        if (!result) {
            return std::unexpected(result.error());
        }
        if (!result->is_array()) {
            return MakeError(ErrorCategory::kParse,
                             "binanceus klines: response is not an array");
        }

        types::Timestamp last_open = since;
        for (const auto& row : *result) {
            // [openTime, open, high, low, close, volume, closeTime, ...]
            if (!row.is_array() || row.size() < 7 ||
                !row[0].is_number_integer() || !row[6].is_number_integer()) {
                return MakeError(ErrorCategory::kParse,
                                 "binanceus klines: malformed kline");
            }
            types::Bar bar;
            bar.open_time = types::FromEpochMillis(row[0].get<std::int64_t>());
            const auto close_time =
                types::FromEpochMillis(row[6].get<std::int64_t>());
            auto o = util::JsonDecimal(row[1]), h = util::JsonDecimal(row[2]),
                 l = util::JsonDecimal(row[3]), c = util::JsonDecimal(row[4]),
                 v = util::JsonDecimal(row[5]);
            for (const auto* d : {&o, &h, &l, &c, &v}) {
                if (!*d) {
                    return std::unexpected(d->error());
                }
            }
            bar.open = *o;
            bar.high = *h;
            bar.low = *l;
            bar.close = *c;
            bar.volume = *v;

            last_open = bar.open_time;
            const bool forming = close_time >= now;
            if (bar.open_time >= start && bar.open_time < end && !forming &&
                (bars.empty() || bar.open_time > bars.back().open_time)) {
                bars.push_back(bar);
            }
        }

        // A short page means the range is exhausted.
        if (result->size() < kMaxBarsPerRequest) {
            break;
        }
        const types::Timestamp next = last_open + milliseconds(1);
        if (next <= since) {
            break;
        }
        since = next;
    }
    return bars;
}

Result<std::unique_ptr<Subscription>> BinanceUSDataSource::StreamQuotes(
    const std::vector<std::string>& symbols, QuoteCallback callback) {
    if (symbols.empty()) {
        return MakeError(ErrorCategory::kInvalidArgument,
                         "StreamQuotes: no symbols");
    }
    if (!callback) {
        return MakeError(ErrorCategory::kInvalidArgument,
                         "StreamQuotes: no callback");
    }
    std::vector<std::string> native_symbols;
    for (const auto& s : symbols) {
        auto native = ToBinanceUSSymbol(s);
        if (!native) {
            return std::unexpected(native.error());
        }
        native_symbols.push_back(*native);
    }
    auto stream = std::make_unique<BinanceUSQuoteStream>(
        mOptions.ws_url, std::move(native_symbols), std::move(callback));
    stream->Start();
    return std::unique_ptr<Subscription>(std::move(stream));
}

Result<std::unique_ptr<Subscription>> BinanceUSDataSource::StreamBars(
    const std::vector<std::string>& symbols, types::Timeframe timeframe,
    BarCallback callback) {
    if (symbols.empty()) {
        return MakeError(ErrorCategory::kInvalidArgument,
                         "StreamBars: no symbols");
    }
    if (!callback) {
        return MakeError(ErrorCategory::kInvalidArgument,
                         "StreamBars: no callback");
    }
    auto interval = BinanceUSInterval(timeframe);
    if (!interval) {
        return std::unexpected(interval.error());
    }
    std::vector<std::string> native_symbols;
    for (const auto& s : symbols) {
        auto native = ToBinanceUSSymbol(s);
        if (!native) {
            return std::unexpected(native.error());
        }
        native_symbols.push_back(*native);
    }
    auto stream = std::make_unique<BinanceUSBarStream>(
        mOptions.ws_url, std::move(native_symbols), timeframe,
        std::string(*interval), std::move(callback));
    stream->Start();
    return std::unique_ptr<Subscription>(std::move(stream));
}

Result<types::SymbolInfo> BinanceUSDataSource::GetSymbolInfo(
    const types::Symbol& symbol) {
    {
        std::lock_guard lock(mCacheMutex);
        if (const auto it = mSymbolCache.find(symbol);
            it != mSymbolCache.end()) {
            return it->second;
        }
    }
    auto native = ToBinanceUSSymbol(symbol);
    if (!native) {
        return std::unexpected(native.error());
    }
    auto result = Call("exchangeInfo", {{"symbol", *native}});
    if (!result) {
        return std::unexpected(result.error());
    }
    const auto symbols = result->find("symbols");
    if (!result->is_object() || symbols == result->end() ||
        !symbols->is_array() || symbols->empty()) {
        return MakeError(ErrorCategory::kNotFound,
                         "binanceus: no exchange info for " + symbol);
    }
    const json& meta = symbols->front();
    const auto filters = meta.find("filters");
    if (filters == meta.end() || !filters->is_array()) {
        return MakeError(ErrorCategory::kParse,
                         "binanceus exchangeInfo: missing filters");
    }

    auto filter_value = [&](const char* type,
                            const char* field) -> Result<types::Decimal> {
        for (const auto& f : *filters) {
            if (f.is_object() && f.value("filterType", "") == type) {
                const auto it = f.find(field);
                if (it == f.end()) {
                    break;
                }
                return util::JsonDecimal(*it);
            }
        }
        return MakeError(ErrorCategory::kParse,
                         std::string("binanceus exchangeInfo: missing ") +
                             type + "." + field);
    };
    auto tick = filter_value("PRICE_FILTER", "tickSize");
    auto step = filter_value("LOT_SIZE", "stepSize");
    auto min_qty = filter_value("LOT_SIZE", "minQty");
    if (!tick) {
        return std::unexpected(tick.error());
    }
    if (!step) {
        return std::unexpected(step.error());
    }
    if (!min_qty) {
        return std::unexpected(min_qty.error());
    }

    types::SymbolInfo info;
    info.symbol = symbol;
    info.tick_size = *tick;
    info.lot_size = *step;
    info.min_order_size = *min_qty;
    {
        std::lock_guard lock(mCacheMutex);
        mSymbolCache[symbol] = info;
    }
    return info;
}

}  // namespace kestrel::data
