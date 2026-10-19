#pragma once

#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "kestrel/data/data_source.hpp"

namespace ix {
class WebSocket;
}

namespace kestrel::data {

// Extracts a completed bar from a combined-stream kline message
// ({"stream":"btcusd@kline_1m","data":{"e":"kline","k":{...,"x":true}}}).
// Returns nullopt for other messages and for klines that are still forming.
std::optional<std::pair<types::Symbol, types::Bar>> ParseBinanceUSClosedKline(
    const nlohmann::json& message);

// WebSocket subscription to the combined kline streams of one interval. Like
// BinanceUSQuoteStream it reconnects with backoff and runs the callback on
// IXWebSocket's thread.
class BinanceUSBarStream : public Subscription {
   public:
    // `native_symbols` are upper-case Binance symbols such as "BTCUSD";
    // `interval` is the Binance interval string for `timeframe`.
    BinanceUSBarStream(std::string ws_base_url,
                       std::vector<std::string> native_symbols,
                       types::Timeframe timeframe, std::string interval,
                       BarCallback callback);
    ~BinanceUSBarStream() override;

    void Start();
    void Stop() override;

    // wss://host:port/stream?streams=btcusd@kline_1m/ethusd@kline_1m/...
    static std::string BuildUrl(const std::string& ws_base_url,
                                const std::vector<std::string>& native_symbols,
                                const std::string& interval);

   private:
    void OnMessage(const std::string& text);

    std::string mUrl;
    types::Timeframe mTimeframe;
    BarCallback mCallback;
    std::unique_ptr<ix::WebSocket> mSocket;
};

}  // namespace kestrel::data
