#include "kestrel/data/kraken_quote_stream.hpp"

#include <ixwebsocket/IXWebSocket.h>
#include <spdlog/spdlog.h>

#include "kestrel/data/symbols.hpp"
#include "kestrel/util/json.hpp"

namespace kestrel::data {

using nlohmann::json;

namespace {

void ReadDecimal(const json& item, const char* key, std::optional<types::Decimal>& out) {
  const auto it = item.find(key);
  if (it == item.end() || it->is_null()) return;
  if (auto d = util::JsonDecimal(*it)) {
    out = *d;
  } else {
    spdlog::warn("kraken ticker: bad '{}' value: {}", key, d.error().message);
  }
}

}  // namespace

std::vector<types::Quote> KrakenTickerMerger::Apply(const json& message, types::Timestamp received) {
  std::vector<types::Quote> out;
  if (!message.is_object() || message.value("channel", "") != "ticker") return out;
  const auto data = message.find("data");
  if (data == message.end() || !data->is_array()) return out;

  std::lock_guard lock(mutex_);
  for (const auto& item : *data) {
    if (!item.is_object() || !item.contains("symbol") || !item["symbol"].is_string()) continue;
    auto symbol = FromKrakenSymbol(item["symbol"].get<std::string>());
    if (!symbol) {
      spdlog::warn("kraken ticker: {}", symbol.error().message);
      continue;
    }
    types::Quote update;
    update.symbol = *symbol;
    update.venue = "kraken";
    update.received_time = received;
    ReadDecimal(item, "bid", update.bid);
    ReadDecimal(item, "bid_qty", update.bid_size);
    ReadDecimal(item, "ask", update.ask);
    ReadDecimal(item, "ask_qty", update.ask_size);
    ReadDecimal(item, "last", update.last);
    // The v2 ticker carries no last-trade size, so last_size stays unset.
    if (item.contains("timestamp") && item["timestamp"].is_string()) {
      if (auto t = types::ParseIso8601(item["timestamp"].get<std::string>())) update.exchange_time = *t;
    }
    auto& merged = state_[update.symbol];
    merged.MergeFrom(update);
    out.push_back(merged);
  }
  return out;
}

void KrakenTickerMerger::Reset() {
  std::lock_guard lock(mutex_);
  state_.clear();
}

KrakenQuoteStream::KrakenQuoteStream(std::string url, std::vector<std::string> ws_symbols,
                                     QuoteCallback callback)
    : url_(std::move(url)),
      symbols_(std::move(ws_symbols)),
      callback_(std::move(callback)),
      socket_(std::make_unique<ix::WebSocket>()) {}

KrakenQuoteStream::~KrakenQuoteStream() { Stop(); }

void KrakenQuoteStream::Start() {
  socket_->setUrl(url_);
  socket_->setPingInterval(20);
  socket_->enableAutomaticReconnection();
  socket_->setMinWaitBetweenReconnectionRetries(500);
  socket_->setMaxWaitBetweenReconnectionRetries(30'000);  // backoff doubles up to this
  socket_->setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
    switch (msg->type) {
      case ix::WebSocketMessageType::Open:
        spdlog::info("kraken ws: connected to {}", url_);
        merger_.Reset();
        Subscribe();  // subscriptions don't survive a reconnect
        break;
      case ix::WebSocketMessageType::Message:
        OnMessage(msg->str);
        break;
      case ix::WebSocketMessageType::Close:
        spdlog::warn("kraken ws: closed ({} {})", msg->closeInfo.code, msg->closeInfo.reason);
        break;
      case ix::WebSocketMessageType::Error:
        spdlog::warn("kraken ws: error: {} (retries {}, wait {} ms)", msg->errorInfo.reason,
                     msg->errorInfo.retries, msg->errorInfo.wait_time);
        break;
      default:
        break;
    }
  });
  socket_->start();
}

void KrakenQuoteStream::Stop() {
  if (socket_) socket_->stop();
}

void KrakenQuoteStream::Subscribe() {
  json request = {{"method", "subscribe"}, {"params", {{"channel", "ticker"}, {"symbol", symbols_}}}};
  socket_->send(request.dump());
}

void KrakenQuoteStream::OnMessage(const std::string& text) {
  auto parsed = util::ParseJson(text);
  if (!parsed) {
    spdlog::warn("kraken ws: unparseable message: {}", parsed.error().message);
    return;
  }
  const json& msg = *parsed;
  if (msg.is_object() && msg.value("method", "") == "subscribe") {
    if (!msg.value("success", false)) {
      spdlog::error("kraken ws: subscription rejected: {}", msg.value("error", "unknown error"));
    }
    return;
  }
  for (const auto& quote : merger_.Apply(msg, types::Now())) {
    try {
      callback_(quote);
    } catch (const std::exception& e) {
      spdlog::error("kraken ws: quote callback threw: {}", e.what());
    }
  }
}

}  // namespace kestrel::data
