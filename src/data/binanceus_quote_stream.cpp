#include "kestrel/data/binanceus_quote_stream.hpp"

#include <ixwebsocket/IXWebSocket.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>

#include "kestrel/data/symbols.hpp"
#include "kestrel/util/json.hpp"

namespace kestrel::data {

using nlohmann::json;

namespace {

void ReadDecimal(const json& item, const char* key,
                 std::optional<types::Decimal>& out) {
    const auto it = item.find(key);
    if (it == item.end() || it->is_null()) {
        return;
    }
    if (auto d = util::JsonDecimal(*it)) {
        out = *d;
    } else {
        spdlog::warn("binanceus ws: bad '{}' value: {}", key,
                     d.error().message);
    }
}

}  // namespace

std::vector<types::Quote> BinanceUSStreamMerger::Apply(
    const json& message, types::Timestamp received) {
    std::vector<types::Quote> out;
    if (!message.is_object()) {
        return out;
    }
    const auto stream = message.find("stream");
    const auto data = message.find("data");
    if (stream == message.end() || !stream->is_string() ||
        data == message.end() || !data->is_object()) {
        return out;
    }
    const auto& name = stream->get_ref<const std::string&>();
    const bool is_trade = name.ends_with("@trade");
    const bool is_book = name.ends_with("@bookTicker");
    if (!is_trade && !is_book) {
        return out;
    }
    const auto sym = data->find("s");
    if (sym == data->end() || !sym->is_string()) {
        return out;
    }
    auto symbol = FromBinanceUSSymbol(sym->get<std::string>());
    if (!symbol) {
        spdlog::warn("binanceus ws: {}", symbol.error().message);
        return out;
    }

    types::Quote update;
    update.symbol = *symbol;
    update.venue = "binanceus";
    update.received_time = received;
    if (is_book) {
        ReadDecimal(*data, "b", update.bid);
        ReadDecimal(*data, "B", update.bid_size);
        ReadDecimal(*data, "a", update.ask);
        ReadDecimal(*data, "A", update.ask_size);
    } else {
        ReadDecimal(*data, "p", update.last);
        ReadDecimal(*data, "q", update.last_size);
        if (const auto t = data->find("T");
            t != data->end() && t->is_number_integer()) {
            update.exchange_time =
                types::FromEpochMillis(t->get<std::int64_t>());
        }
    }

    std::lock_guard lock(mMutex);
    auto& merged = mState[update.symbol];
    merged.MergeFrom(update);
    out.push_back(merged);
    return out;
}

void BinanceUSStreamMerger::Reset() {
    std::lock_guard lock(mMutex);
    mState.clear();
}

std::string BinanceUSQuoteStream::BuildUrl(
    const std::string& ws_base_url,
    const std::vector<std::string>& native_symbols) {
    std::string url = ws_base_url + "/stream?streams=";
    bool first = true;
    for (auto symbol : native_symbols) {
        std::ranges::transform(symbol, symbol.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        url += (first ? "" : "/") + symbol + "@bookTicker/" + symbol + "@trade";
        first = false;
    }
    return url;
}

BinanceUSQuoteStream::BinanceUSQuoteStream(
    std::string ws_base_url, std::vector<std::string> native_symbols,
    QuoteCallback callback)
    : mUrl(BuildUrl(ws_base_url, native_symbols)),
      mCallback(std::move(callback)),
      mSocket(std::make_unique<ix::WebSocket>()) {}

BinanceUSQuoteStream::~BinanceUSQuoteStream() { Stop(); }

void BinanceUSQuoteStream::Start() {
    mSocket->setUrl(mUrl);
    mSocket->setPingInterval(20);
    mSocket->enableAutomaticReconnection();
    mSocket->setMinWaitBetweenReconnectionRetries(500);
    mSocket->setMaxWaitBetweenReconnectionRetries(30'000);
    mSocket->setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
        switch (msg->type) {
            case ix::WebSocketMessageType::Open:
                spdlog::info("binanceus ws: connected to {}", mUrl);
                mMerger.Reset();
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

void BinanceUSQuoteStream::Stop() {
    if (mSocket) {
        mSocket->stop();
    }
}

void BinanceUSQuoteStream::OnMessage(const std::string& text) {
    auto parsed = util::ParseJson(text);
    if (!parsed) {
        spdlog::warn("binanceus ws: unparseable message: {}",
                     parsed.error().message);
        return;
    }
    for (const auto& quote : mMerger.Apply(*parsed, types::Now())) {
        try {
            mCallback(quote);
        } catch (const std::exception& e) {
            spdlog::error("binanceus ws: quote callback threw: {}", e.what());
        }
    }
}

}  // namespace kestrel::data
