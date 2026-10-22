#include "kestrel/data/binanceus_bar_stream.hpp"

#include <ixwebsocket/IXWebSocket.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>

#include "kestrel/data/symbols.hpp"
#include "kestrel/util/json.hpp"

namespace kestrel::data {

using nlohmann::json;

std::optional<std::pair<types::Symbol, types::Bar>> ParseBinanceUSClosedKline(
    const json& message) {
    if (!message.is_object()) {
        return std::nullopt;
    }
    const auto data = message.find("data");
    if (data == message.end() || !data->is_object()) {
        return std::nullopt;
    }
    const auto k = data->find("k");
    const auto s = data->find("s");
    if (k == data->end() || !k->is_object() || s == data->end() ||
        !s->is_string()) {
        return std::nullopt;
    }
    const auto closed = k->find("x");
    if (closed == k->end() || !closed->is_boolean() || !closed->get<bool>()) {
        return std::nullopt;
    }
    const auto t = k->find("t");
    if (t == k->end() || !t->is_number_integer()) {
        return std::nullopt;
    }
    auto symbol = FromBinanceUSSymbol(s->get<std::string>());
    if (!symbol) {
        spdlog::warn("binanceus ws: {}", symbol.error().message);
        return std::nullopt;
    }

    types::Bar bar;
    bar.open_time = types::FromEpochMillis(t->get<std::int64_t>());
    const std::pair<const char*, types::Decimal*> fields[] = {
        {"o", &bar.open},
        {"h", &bar.high},
        {"l", &bar.low},
        {"c", &bar.close},
        {"v", &bar.volume}};
    for (const auto& [key, out] : fields) {
        const auto it = k->find(key);
        if (it == k->end()) {
            spdlog::warn("binanceus ws: kline missing '{}'", key);
            return std::nullopt;
        }
        auto d = util::JsonDecimal(*it);
        if (!d) {
            spdlog::warn("binanceus ws: bad kline '{}': {}", key,
                         d.error().message);
            return std::nullopt;
        }
        *out = *d;
    }
    return std::make_pair(std::move(*symbol), bar);
}

std::string BinanceUSBarStream::BuildUrl(
    const std::string& ws_base_url,
    const std::vector<std::string>& native_symbols,
    const std::string& interval) {
    std::string url = ws_base_url + "/stream?streams=";
    bool first = true;
    for (auto symbol : native_symbols) {
        std::ranges::transform(symbol, symbol.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        url += (first ? "" : "/") + symbol + "@kline_" + interval;
        first = false;
    }
    return url;
}

BinanceUSBarStream::BinanceUSBarStream(std::string ws_base_url,
                                       std::vector<std::string> native_symbols,
                                       types::Timeframe timeframe,
                                       std::string interval,
                                       BarCallback callback)
    : mUrl(BuildUrl(ws_base_url, native_symbols, interval)),
      mTimeframe(timeframe),
      mCallback(std::move(callback)),
      mSocket(std::make_unique<ix::WebSocket>()) {}

BinanceUSBarStream::~BinanceUSBarStream() { Stop(); }

void BinanceUSBarStream::Start() {
    mSocket->setUrl(mUrl);
    mSocket->setPingInterval(20);
    mSocket->enableAutomaticReconnection();
    mSocket->setMinWaitBetweenReconnectionRetries(500);
    mSocket->setMaxWaitBetweenReconnectionRetries(30'000);
    mSocket->setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
        switch (msg->type) {
            case ix::WebSocketMessageType::Open:
                spdlog::info("binanceus ws: connected to {}", mUrl);
                break;
            case ix::WebSocketMessageType::Message:
                OnMessage(msg->str);
                break;
            case ix::WebSocketMessageType::Close:
                spdlog::warn("binanceus ws: closed ({} {})",
                             msg->closeInfo.code, msg->closeInfo.reason);
                break;
            case ix::WebSocketMessageType::Error:
                spdlog::warn("binanceus ws: error: {} (retries {}, wait {} ms)",
                             msg->errorInfo.reason, msg->errorInfo.retries,
                             msg->errorInfo.wait_time);
                break;
            default:
                break;
        }
    });
    mSocket->start();
}

void BinanceUSBarStream::Stop() {
    if (mSocket) {
        mSocket->stop();
    }
}

void BinanceUSBarStream::OnMessage(const std::string& text) {
    auto parsed = util::ParseJson(text);
    if (!parsed) {
        spdlog::warn("binanceus ws: unparseable message: {}",
                     parsed.error().message);
        return;
    }
    const auto bar = ParseBinanceUSClosedKline(*parsed);
    if (!bar) {
        return;
    }
    try {
        mCallback(bar->first, mTimeframe, bar->second);
    } catch (const std::exception& e) {
        spdlog::error("binanceus ws: bar callback threw: {}", e.what());
    }
}

}  // namespace kestrel::data
