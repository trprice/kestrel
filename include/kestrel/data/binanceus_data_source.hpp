#pragma once

#include "kestrel/data/data_source.hpp"

namespace kestrel::data {

// Binance.US kline interval string ("1m", "4h", "1M", ...), or kUnsupported.
types::Result<std::string_view> BinanceUSInterval(types::Timeframe timeframe);

// STUB. Not yet functional; every call returns kNotImplemented.
//
// TODO(binanceus): implement against the Binance.US REST/WebSocket API.
//   Base URL: https://api.binance.us ; market data is public (no key needed).
//   GetBars:  GET /api/v3/klines?symbol=BTCUSD&interval=1h&startTime=<ms>
//             &endTime=<ms>&limit=1000. Rows are
//             [openTime, open, high, low, close, volume, closeTime, ...] with
//             prices as strings. Page by advancing startTime past the last
//             openTime (max 1000 rows per call); drop the forming kline.
//   Stream:   wss://stream.binance.us:9443/stream?streams=btcusd@bookTicker/
//             btcusd@trade (combined stream; lower-case symbols). bookTicker
//             gives b/B/a/A (bid, bid qty, ask, ask qty); trade gives p/q/T.
//             Connections are dropped after 24h, so reconnect and resubscribe.
//   SymbolInfo: GET /api/v3/exchangeInfo?symbol=BTCUSD ->
//             filters PRICE_FILTER.tickSize, LOT_SIZE.stepSize/minQty.
//   Limits:   request-weight budget of 1200/minute per IP (X-MBX-USED-WEIGHT
//             response header); HTTP 429 means back off, 418 means banned.
//   Signing:  only needed for trading endpoints; see BinanceUSTradingVenue.
class BinanceUSDataSource : public DataSource {
 public:
  std::string_view Name() const override { return "binanceus"; }
  types::Result<std::vector<types::Bar>> GetBars(const types::Symbol&, types::Timeframe,
                                                 types::Timestamp, types::Timestamp) override;
  types::Result<std::unique_ptr<Subscription>> StreamQuotes(const std::vector<std::string>&,
                                                            QuoteCallback) override;
  types::Result<types::SymbolInfo> GetSymbolInfo(const types::Symbol&) override;
};

}  // namespace kestrel::data
