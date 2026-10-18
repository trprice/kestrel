#pragma once

#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>

#include "kestrel/data/data_source.hpp"

namespace ix {
class WebSocket;
}

namespace kestrel::data {

// Turns Binance.US combined-stream messages ({"stream":..., "data":...}) into
// Quotes, keeping the latest known value of every field per symbol. Handles
// the bookTicker (bid/ask) and trade (last price/size) streams.
class BinanceUSStreamMerger {
   public:
    // `message` must come from util::ParseJson. Unrecognised messages produce
    // no quotes.
    std::vector<types::Quote> Apply(const nlohmann::json& message,
                                    types::Timestamp received);
    // Forget accumulated state (called on reconnect).
    void Reset();

   private:
    std::mutex mMutex;
    std::unordered_map<types::Symbol, types::Quote> mState;
};

// WebSocket subscription to the combined bookTicker + trade streams for a set
// of symbols. Reconnects with exponential backoff, which also covers
// Binance's forced disconnect after 24 hours; the streams are encoded in the
// URL, so nothing needs resubscribing. The callback runs on IXWebSocket's
// thread.
class BinanceUSQuoteStream : public Subscription {
   public:
    // `native_symbols` are upper-case Binance symbols such as "BTCUSD".
    BinanceUSQuoteStream(std::string ws_base_url,
                         std::vector<std::string> native_symbols,
                         QuoteCallback callback);
    ~BinanceUSQuoteStream() override;

    void Start();
    void Stop() override;

    // wss://host:port/stream?streams=btcusd@bookTicker/btcusd@trade/...
    static std::string BuildUrl(const std::string& ws_base_url,
                                const std::vector<std::string>& native_symbols);

   private:
    void OnMessage(const std::string& text);

    std::string mUrl;
    QuoteCallback mCallback;
    BinanceUSStreamMerger mMerger;
    std::unique_ptr<ix::WebSocket> mSocket;
};

}  // namespace kestrel::data
