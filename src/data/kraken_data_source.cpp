#include "kestrel/data/kraken_data_source.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>

#include "kestrel/data/kraken_quote_stream.hpp"
#include "kestrel/data/symbols.hpp"
#include "kestrel/util/json.hpp"

namespace kestrel::data {

using nlohmann::json;
using types::ErrorCategory;
using types::MakeError;
using types::Result;

namespace {

constexpr std::size_t kMaxBarsPerRequest = 720;

bool StartsWith(std::string_view s, std::string_view prefix) { return s.starts_with(prefix); }

}  // namespace

Result<int> KrakenInterval(types::Timeframe tf) {
  using types::Timeframe;
  switch (tf) {
    case Timeframe::kM1: return 1;
    case Timeframe::kM5: return 5;
    case Timeframe::kM15: return 15;
    case Timeframe::kM30: return 30;
    case Timeframe::kH1: return 60;
    case Timeframe::kH4: return 240;
    case Timeframe::kD1: return 1440;
    case Timeframe::kW1: return 10080;
    case Timeframe::kD15: return 21600;
    default:
      return MakeError(ErrorCategory::kUnsupported,
                       "Kraken does not offer " + std::string(types::ToString(tf)) + " bars");
  }
}

ErrorCategory ClassifyKrakenError(std::string_view code) {
  if (StartsWith(code, "EAPI:Rate limit") || StartsWith(code, "EGeneral:Too many requests") ||
      StartsWith(code, "EOrder:Rate limit") || StartsWith(code, "EGeneral:Temporary lockout"))
  {
    return ErrorCategory::kRateLimit;
  }
  if (StartsWith(code, "EService:") || StartsWith(code, "EGeneral:Internal error"))
  {
    return ErrorCategory::kServer;
  }
  if (StartsWith(code, "EAPI:Invalid key") || StartsWith(code, "EAPI:Invalid signature") ||
      StartsWith(code, "EAPI:Invalid nonce") || StartsWith(code, "EGeneral:Permission denied"))
  {
    return ErrorCategory::kAuth;
  }
  if (StartsWith(code, "EQuery:Unknown asset pair") || StartsWith(code, "EOrder:Unknown order"))
  {
    return ErrorCategory::kNotFound;
  }
  if (StartsWith(code, "EOrder:Insufficient funds"))
  {
    return ErrorCategory::kInsufficientFunds;
  }
  if (StartsWith(code, "EGeneral:Invalid arguments"))
  {
    return ErrorCategory::kInvalidArgument;
  }
  return ErrorCategory::kRejected;
}

KrakenDataSource::KrakenDataSource(std::shared_ptr<net::HttpClient> http, Options options)
    : mHttp(std::move(http)),
      mOptions(std::move(options)),
      mLimiter(mOptions.requests_per_second, mOptions.request_burst) {}

KrakenDataSource::KrakenDataSource(std::shared_ptr<net::HttpClient> http)
    : KrakenDataSource(std::move(http), Options{}) {}

KrakenDataSource::KrakenDataSource()
    : KrakenDataSource(std::make_shared<net::CprHttpClient>(), Options{}) {}

Result<json> KrakenDataSource::Call(const std::string& method, std::map<std::string, std::string> query) {
  net::HttpRequest request;
  request.url = mOptions.rest_base_url + "/0/public/" + method;
  request.query = std::move(query);

  return util::Retry(
      mOptions.retry,
      [&]() -> Result<json> {
        mLimiter.Acquire();
        auto response = mHttp->Send(request);
        if (!response)
        {
          if (response.error().category == ErrorCategory::kRateLimit)
          {
            mLimiter.Penalize();
          }
          spdlog::debug("kraken {}: {}", method, response.error().ToString());
          return std::unexpected(response.error());
        }
        auto body = util::ParseJson(response->body);
        if (!body)
        {
          return std::unexpected(body.error());
        }
        if (!body->is_object())
        {
          return MakeError(ErrorCategory::kParse, "kraken: response is not an object");
        }

        if (const auto errors = body->find("error"); errors != body->end() && errors->is_array())
        {
          for (const auto& e : *errors) {
            if (!e.is_string())
            {
              continue;
            }
            const auto code = e.get<std::string>();
            if (code.empty() || code[0] != 'E')
            {
              continue;  // 'W' entries are warnings
            }
            const auto category = ClassifyKrakenError(code);
            if (category == ErrorCategory::kRateLimit)
            {
              mLimiter.Penalize();
            }
            auto err = MakeError(category, "kraken " + method + ": " + code, response->status, code);
            return err;
          }
        }
        const auto result = body->find("result");
        if (result == body->end())
        {
          return MakeError(ErrorCategory::kParse, "kraken: response has no result");
        }
        return *result;
      },
      mOptions.sleeper);
}

Result<std::vector<types::Bar>> KrakenDataSource::GetBars(const types::Symbol& symbol,
                                                          types::Timeframe timeframe,
                                                          types::Timestamp start,
                                                          types::Timestamp end) {
  using namespace std::chrono;
  auto interval = KrakenInterval(timeframe);
  if (!interval)
  {
    return std::unexpected(interval.error());
  }
  auto pair = ToKrakenSymbol(symbol);
  if (!pair)
  {
    return std::unexpected(pair.error());
  }
  if (end < start)
  {
    return MakeError(ErrorCategory::kInvalidArgument, "GetBars: end precedes start");
  }

  const seconds bar_length = *types::FixedDuration(timeframe);
  const types::Timestamp now = mOptions.clock();

  std::vector<types::Bar> bars;
  types::Timestamp since = start;
  while (since < end) {
    auto result = Call("OHLC", {{"pair", *pair},
                                {"interval", std::to_string(*interval)},
                                {"since", std::to_string(types::ToEpochSeconds(since))}});
    if (!result)
    {
      return std::unexpected(result.error());
    }

    // The result holds one array of candles under the pair's key, plus "last".
    const json* candles = nullptr;
    for (const auto& [key, value] : result->items()) {
      if (key != "last" && value.is_array())
      {
        candles = &value;
      }
    }
    if (!candles)
    {
      return MakeError(ErrorCategory::kParse, "kraken OHLC: no candle array in response");
    }

    std::vector<types::Bar> page;
    for (const auto& row : *candles) {
      if (!row.is_array() || row.size() < 7 || !row[0].is_number_integer())
      {
        return MakeError(ErrorCategory::kParse, "kraken OHLC: malformed candle");
      }
      // [time, open, high, low, close, vwap, volume, count]
      types::Bar bar;
      bar.open_time = types::FromEpochSeconds(row[0].get<std::int64_t>());
      auto o = util::JsonDecimal(row[1]), h = util::JsonDecimal(row[2]), l = util::JsonDecimal(row[3]),
           c = util::JsonDecimal(row[4]), v = util::JsonDecimal(row[6]);
      for (const auto* d : {&o, &h, &l, &c, &v}) {
        if (!*d)
        {
          return std::unexpected(d->error());
        }
      }
      bar.open = *o;
      bar.high = *h;
      bar.low = *l;
      bar.close = *c;
      bar.volume = *v;
      page.push_back(bar);
    }
    if (page.empty())
    {
      break;
    }

    // A full window whose oldest bar is after `start` means the range reaches
    // past the 720 bars Kraken retains.
    if (bars.empty() && page.size() >= kMaxBarsPerRequest && page.front().open_time > start)
    {
      return MakeError(ErrorCategory::kUnsupported,
                       "kraken retains only the latest " + std::to_string(kMaxBarsPerRequest) +
                           " " + std::string(types::ToString(timeframe)) +
                           " bars; earliest available is " + types::FormatIso8601(page.front().open_time));
    }

    const types::Timestamp last_open = page.back().open_time;
    for (const auto& bar : page) {
      const bool forming = bar.open_time + bar_length > now;
      if (bar.open_time >= start && bar.open_time < end && !forming &&
          (bars.empty() || bar.open_time > bars.back().open_time))
      {
        bars.push_back(bar);
      }
    }

    // Done when the page reached the end of the range or made no progress.
    if (last_open + bar_length >= end || last_open + bar_length > now)
    {
      break;
    }
    const types::Timestamp next = last_open + seconds(1);
    if (next <= since)
    {
      break;
    }
    since = next;
  }
  return bars;
}

Result<std::unique_ptr<Subscription>> KrakenDataSource::StreamQuotes(
    const std::vector<std::string>& symbols, QuoteCallback callback) {
  if (symbols.empty())
  {
    return MakeError(ErrorCategory::kInvalidArgument, "StreamQuotes: no symbols");
  }
  if (!callback)
  {
    return MakeError(ErrorCategory::kInvalidArgument, "StreamQuotes: no callback");
  }
  std::vector<std::string> ws_symbols;
  for (const auto& s : symbols) {
    auto native = ToKrakenSymbol(s);
    if (!native)
    {
      return std::unexpected(native.error());
    }
    ws_symbols.push_back(*native);
  }
  auto stream = std::make_unique<KrakenQuoteStream>(mOptions.ws_url, std::move(ws_symbols), std::move(callback));
  stream->Start();
  return std::unique_ptr<Subscription>(std::move(stream));
}

Result<types::SymbolInfo> KrakenDataSource::GetSymbolInfo(const types::Symbol& symbol) {
  {
    std::lock_guard lock(mCacheMutex);
    if (const auto it = mSymbolCache.find(symbol); it != mSymbolCache.end())
    {
      return it->second;
    }
  }
  auto pair = ToKrakenSymbol(symbol);
  if (!pair)
  {
    return std::unexpected(pair.error());
  }
  auto result = Call("AssetPairs", {{"pair", *pair}});
  if (!result)
  {
    return std::unexpected(result.error());
  }
  if (!result->is_object() || result->empty())
  {
    return MakeError(ErrorCategory::kNotFound, "kraken: no asset pair metadata for " + symbol);
  }
  const json& meta = result->begin().value();

  auto field = [&](const char* name) -> Result<types::Decimal> {
    const auto it = meta.find(name);
    if (it == meta.end())
    {
      return MakeError(ErrorCategory::kParse, std::string("kraken AssetPairs: missing ") + name);
    }
    return util::JsonDecimal(*it);
  };
  auto tick = field("tick_size");
  auto min_size = field("ordermin");
  auto lot_decimals = field("lot_decimals");
  if (!tick)
  {
    return std::unexpected(tick.error());
  }
  if (!min_size)
  {
    return std::unexpected(min_size.error());
  }
  if (!lot_decimals)
  {
    return std::unexpected(lot_decimals.error());
  }
  const auto decimals = lot_decimals->Mantissa() / 1;
  if (decimals < 0 || decimals > types::Decimal::kMaxScale)
  {
    return MakeError(ErrorCategory::kParse, "kraken AssetPairs: bad lot_decimals");
  }

  types::SymbolInfo info;
  info.symbol = symbol;
  info.tick_size = *tick;
  info.lot_size = types::Decimal(1, static_cast<int>(decimals));
  info.min_order_size = *min_size;
  {
    std::lock_guard lock(mCacheMutex);
    mSymbolCache[symbol] = info;
  }
  return info;
}

}  // namespace kestrel::data
