#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "kestrel/data/data_source.hpp"

namespace ix {
class WebSocket;
}

namespace kestrel::data {

// Turns Kraken v2 "ticker" messages into Quotes, keeping the latest known
// value of every field per symbol so partial updates yield complete quotes.
class KrakenTickerMerger {
 public:
  // `message` must come from util::ParseJson. Non-ticker messages (heartbeat,
  // status, subscribe acks) produce no quotes.
  std::vector<types::Quote> Apply(const nlohmann::json& message, types::Timestamp received);
  // Forget accumulated state (called on reconnect so stale fields can't leak
  // into the fresh snapshot).
  void Reset();

 private:
  std::mutex mMutex;
  std::unordered_map<types::Symbol, types::Quote> mState;
};

// WebSocket subscription to Kraken's v2 ticker channel. Reconnects with
// exponential backoff (IXWebSocket's built-in automatic reconnection) and
// resubscribes on every (re)connect. The callback runs on IXWebSocket's thread.
class KrakenQuoteStream : public Subscription {
 public:
  KrakenQuoteStream(std::string url, std::vector<std::string> ws_symbols, QuoteCallback callback);
  ~KrakenQuoteStream() override;

  void Start();
  void Stop() override;

 private:
  void OnMessage(const std::string& text);
  void Subscribe();

  std::string mUrl;
  std::vector<std::string> mSymbols;
  QuoteCallback mCallback;
  KrakenTickerMerger mMerger;
  std::unique_ptr<ix::WebSocket> mSocket;
};

}  // namespace kestrel::data
