#include "kestrel/data/schwab_streamer.hpp"

#include <ixwebsocket/IXWebSocket.h>
#include <spdlog/spdlog.h>

#include <algorithm>

#include "kestrel/data/symbols.hpp"
#include "kestrel/util/json.hpp"

namespace kestrel::data {

using nlohmann::json;
using types::ErrorCategory;
using types::MakeError;

namespace {

// Streamer response and notify codes (Streamer API, Response Codes).
constexpr int kSuccess = 0;
constexpr int kLoginDenied = 3;
constexpr int kCloseConnection = 12;  // another connection took over
constexpr int kStopStreaming = 30;

// Live, every success is code 0; the document also lists 26 to 29 as a SUBS,
// UNSUBS, ADD or VIEW that succeeded.
bool Succeeded(int code) { return code == 0 || (code >= 26 && code <= 29); }

// A price field: 0 is no price.
void ReadPrice(const json& item, const char* field,
               std::optional<types::Decimal>& out) {
    const auto it = item.find(field);
    if (it == item.end() || it->is_null()) {
        return;
    }
    if (auto d = util::JsonDecimal(*it); !d) {
        spdlog::warn("schwab streamer: bad field {}: {}", field,
                     d.error().message);
    } else if (!d->IsZero()) {
        out = *d;
    }
}

void ReadSize(const json& item, const char* field,
              std::optional<types::Decimal>& out) {
    const auto it = item.find(field);
    if (it == item.end() || it->is_null()) {
        return;
    }
    if (auto d = util::JsonDecimal(*it)) {
        out = *d;
    } else {
        spdlog::warn("schwab streamer: bad field {}: {}", field,
                     d.error().message);
    }
}

std::optional<types::Timestamp> ReadTime(const json& item, const char* field) {
    const auto it = item.find(field);
    if (it == item.end() || !it->is_number_integer() ||
        it->get<std::int64_t>() <= 0) {
        return std::nullopt;
    }
    return types::FromEpochMillis(it->get<std::int64_t>());
}

std::string Join(const std::set<std::string>& keys) {
    std::string out;
    for (const auto& key : keys) {
        out += (out.empty() ? "" : ",") + key;
    }
    return out;
}

// The code and message of a response or notify item's content.
std::pair<int, std::string> Outcome(const json& item) {
    const auto content = item.find("content");
    if (content == item.end() || !content->is_object()) {
        return {-1, ""};
    }
    const auto code = content->find("code");
    return {code != content->end() && code->is_number_integer()
                ? code->get<int>()
                : -1,
            content->value("msg", "")};
}

class IxStreamerSocket : public SchwabStreamerSocket {
   public:
    ~IxStreamerSocket() override { Stop(); }

    void Start(const std::string& url, Handler handler) override {
        mSocket.setUrl(url);
        mSocket.setPingInterval(20);
        mSocket.enableAutomaticReconnection();
        mSocket.setMinWaitBetweenReconnectionRetries(1'000);
        mSocket.setMaxWaitBetweenReconnectionRetries(30'000);
        mSocket.setOnMessageCallback([handler = std::move(handler)](
                                         const ix::WebSocketMessagePtr& m) {
            switch (m->type) {
                case ix::WebSocketMessageType::Open:
                    handler(Event::kOpen, "");
                    break;
                case ix::WebSocketMessageType::Message:
                    handler(Event::kMessage, m->str);
                    break;
                case ix::WebSocketMessageType::Close:
                    handler(Event::kClose, std::to_string(m->closeInfo.code) +
                                               " " + m->closeInfo.reason);
                    break;
                case ix::WebSocketMessageType::Error:
                    spdlog::warn(
                        "schwab streamer: error: {} (retries {}, wait {} "
                        "ms)",
                        m->errorInfo.reason, m->errorInfo.retries,
                        m->errorInfo.wait_time);
                    break;
                default:
                    break;
            }
        });
        mSocket.start();
    }
    void Send(const std::string& text) override { mSocket.send(text); }
    void Close() override { mSocket.close(); }
    void StopReconnecting() override { mSocket.disableAutomaticReconnection(); }
    void Stop() override { mSocket.stop(); }

   private:
    ix::WebSocket mSocket;
};

}  // namespace

types::Result<SchwabStreamerInfo> ParseSchwabStreamerInfo(
    std::string_view body) {
    auto parsed = util::ParseJson(body);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    const json* info = nullptr;
    if (parsed->is_object()) {
        const auto it = parsed->find("streamerInfo");
        if (it != parsed->end() && it->is_array() && !it->empty()) {
            info = &it->front();
        } else if (it != parsed->end() && it->is_object()) {
            info = &*it;
        }
    }
    if (!info || !info->is_object()) {
        return MakeError(ErrorCategory::kParse,
                         "schwab userPreference: no streamerInfo");
    }
    SchwabStreamerInfo out;
    const std::pair<const char*, std::string*> fields[] = {
        {"streamerSocketUrl", &out.socket_url},
        {"schwabClientCustomerId", &out.customer_id},
        {"schwabClientCorrelId", &out.correl_id},
        {"schwabClientChannel", &out.channel},
        {"schwabClientFunctionId", &out.function_id}};
    for (const auto& [name, field] : fields) {
        const auto it = info->find(name);
        if (it == info->end() || !it->is_string() ||
            it->get<std::string>().empty()) {
            return MakeError(
                ErrorCategory::kParse,
                std::string("schwab userPreference: streamerInfo has no ") +
                    name);
        }
        *field = it->get<std::string>();
    }
    return out;
}

std::vector<SchwabKeyedQuote> SchwabLevelOneMerger::Apply(
    const json& content, types::Timestamp received) {
    std::vector<SchwabKeyedQuote> out;
    if (!content.is_array()) {
        return out;
    }
    for (const auto& item : content) {
        const auto key_it = item.is_object() ? item.find("key") : item.end();
        if (!item.is_object() || key_it == item.end() || !key_it->is_string()) {
            continue;
        }
        const std::string key = key_it->get<std::string>();
        if (const auto status = item.find("32");
            status != item.end() && status->is_string() &&
            status->get<std::string>() == "Unknown") {
            if (mUnknown.insert(key).second) {
                spdlog::warn(
                    "schwab streamer: Schwab does not know the symbol {}; no "
                    "quotes for it",
                    key);
            }
            continue;
        }
        if (mUnknown.contains(key)) {
            continue;
        }
        if (item.value("delayed", false) && mDelayed.insert(key).second) {
            spdlog::warn(
                "schwab streamer: quotes for {} are delayed or from a subset "
                "of exchanges (not the SIP)",
                key);
        }
        auto symbol = FromSchwabSymbol(key);
        if (!symbol) {
            spdlog::warn("schwab streamer: {}", symbol.error().message);
            continue;
        }
        types::Quote update;
        update.symbol = *symbol;
        update.venue = "schwab";
        update.received_time = received;
        ReadPrice(item, "1", update.bid);
        ReadPrice(item, "2", update.ask);
        ReadPrice(item, "3", update.last);
        ReadSize(item, "4", update.bid_size);
        ReadSize(item, "5", update.ask_size);
        ReadSize(item, "9", update.last_size);
        const auto quote_time = ReadTime(item, "34");
        const auto trade_time = ReadTime(item, "35");
        if (quote_time && trade_time) {
            update.exchange_time = std::max(*quote_time, *trade_time);
        } else {
            update.exchange_time = quote_time ? quote_time : trade_time;
        }
        auto& merged = mState[key];
        merged.MergeFrom(update);
        out.push_back({key, merged});
    }
    return out;
}

void SchwabLevelOneMerger::Reset() { mState.clear(); }

void SchwabLevelOneMerger::Forget(const std::string& key) { mState.erase(key); }

std::unique_ptr<SchwabStreamerSocket> MakeSchwabStreamerSocket() {
    return std::make_unique<IxStreamerSocket>();
}

// A consumer's handle: stopping it removes the consumer.
class SchwabStreamer::QuoteSubscription : public Subscription {
   public:
    QuoteSubscription(std::shared_ptr<SchwabStreamer> streamer,
                      std::uint64_t id)
        : mStreamer(std::move(streamer)), mId(id) {}
    ~QuoteSubscription() override { Stop(); }

    void Stop() override {
        if (mStreamer) {
            mStreamer->Remove(mId);
            mStreamer.reset();
        }
    }

   private:
    std::shared_ptr<SchwabStreamer> mStreamer;
    std::uint64_t mId;
};

SchwabStreamer::SchwabStreamer(std::shared_ptr<SchwabApi> api,
                               std::string trader_url,
                               SocketFactory make_socket,
                               std::chrono::milliseconds takeover_reconnect,
                               Clock clock)
    : mApi(std::move(api)),
      mTraderUrl(std::move(trader_url)),
      mMakeSocket(std::move(make_socket)),
      mTakeoverReconnect(takeover_reconnect),
      mClock(std::move(clock)) {}

SchwabStreamer::~SchwabStreamer() {
    std::unique_ptr<SchwabStreamerSocket> socket;
    {
        std::lock_guard lock(mMutex);
        mClosing = true;
        socket = std::move(mSocket);
    }
    mResumeWake.notify_all();
    if (mResumer.joinable()) {
        mResumer.join();
    }
    if (socket) {
        socket->Stop();
    }
}

void SchwabStreamer::StartSocket() {
    mSocket = mMakeSocket();
    mLoggedIn = false;
    mDenials = 0;
    const std::uint64_t generation = ++mGeneration;
    mSocket->Start(mInfo->socket_url,
                   [this, generation](SchwabStreamerSocket::Event event,
                                      const std::string& text) {
                       OnEvent(generation, event, text);
                   });
}

void SchwabStreamer::ResumeLoop() {
    std::unique_lock lock(mMutex);
    while (!mClosing) {
        if (!mResumeAt) {
            mResumeWake.wait(lock);
            continue;
        }
        if (std::chrono::steady_clock::now() < *mResumeAt) {
            mResumeWake.wait_until(lock, *mResumeAt);
            continue;
        }
        mResumeAt.reset();
        if (!mSocket || mHalted) {
            continue;  // nobody is subscribed any more
        }
        spdlog::info("schwab streamer: reconnecting after the takeover");
        auto old = std::move(mSocket);
        StartSocket();
        // The old socket's thread may be waiting for mMutex.
        lock.unlock();
        old->Stop();
        lock.lock();
    }
}

types::Result<SchwabStreamerInfo> SchwabStreamer::FetchInfo() {
    net::HttpRequest request;
    request.url = mTraderUrl + "/userPreference";
    auto response = mApi->Send(std::move(request));
    if (!response) {
        auto error = response.error();
        error.message = "schwab userPreference: " + error.message;
        return std::unexpected(std::move(error));
    }
    return ParseSchwabStreamerInfo(response->body);
}

types::Result<std::unique_ptr<Subscription>> SchwabStreamer::SubscribeQuotes(
    const std::vector<std::string>& keys, QuoteCallback callback) {
    std::lock_guard lock(mMutex);
    if (mHalted) {
        return MakeError(ErrorCategory::kRejected,
                         "the Schwab streamer has stopped: " + *mHalted);
    }
    if (!mSocket) {
        // The info is fetched for each new connection, while holding mMutex:
        // there is no connection whose events could be waiting for it.
        auto info = FetchInfo();
        if (!info) {
            return std::unexpected(info.error());
        }
        mInfo = std::move(*info);
    }
    auto consumer = std::make_shared<Consumer>();
    consumer->keys = {keys.begin(), keys.end()};
    consumer->callback = std::move(callback);
    std::set<std::string> added;
    for (const auto& key : consumer->keys) {
        if (mKeys[key]++ == 0) {
            added.insert(key);
        }
    }
    const std::uint64_t id = mNextConsumer++;
    mConsumers.emplace(id, std::move(consumer));

    if (!mSocket) {
        StartSocket();
    } else if (mLoggedIn && !added.empty()) {
        SendKeys("ADD", added);
    }
    // Before login, OnResponse subscribes to every key wanted.
    return std::make_unique<QuoteSubscription>(shared_from_this(), id);
}

void SchwabStreamer::Remove(std::uint64_t id) {
    std::set<std::string> removed;
    std::unique_ptr<SchwabStreamerSocket> socket;
    {
        std::lock_guard lock(mMutex);
        const auto it = mConsumers.find(id);
        if (it == mConsumers.end()) {
            return;
        }
        for (const auto& key : it->second->keys) {
            if (--mKeys[key] == 0) {
                mKeys.erase(key);
                removed.insert(key);
            }
        }
        mConsumers.erase(it);
        if (mConsumers.empty()) {
            socket = std::move(mSocket);
            mLoggedIn = false;
            mResumeAt.reset();
        } else if (mLoggedIn && !removed.empty()) {
            SendKeys("UNSUBS", removed);
        }
    }
    // Wait out a dispatch that may still be calling this consumer.
    {
        std::lock_guard dispatch(mDispatch);
        for (const auto& key : removed) {
            mMerger.Forget(key);
        }
    }
    if (socket) {
        socket->Stop();
    }
}

bool SchwabStreamer::Current(std::uint64_t generation) const {
    return mSocket && generation == mGeneration;
}

void SchwabStreamer::Send(std::string_view service, std::string_view command,
                          json parameters) {
    json request = {{"requestid", std::to_string(mNextRequest++)},
                    {"service", service},
                    {"command", command},
                    {"SchwabClientCustomerId", mInfo->customer_id},
                    {"SchwabClientCorrelId", mInfo->correl_id},
                    {"parameters", std::move(parameters)}};
    mSocket->Send(json{{"requests", json::array({std::move(request)})}}.dump());
}

void SchwabStreamer::SendKeys(std::string_view command,
                              const std::set<std::string>& keys) {
    json parameters = {{"keys", Join(keys)}};
    if (command != "UNSUBS") {
        parameters["fields"] = kSchwabLevelOneFields;
    }
    Send("LEVELONE_EQUITIES", command, std::move(parameters));
}

void SchwabStreamer::Halt(const std::string& reason) {
    mHalted = reason;
    mLoggedIn = false;
    spdlog::error("schwab streamer: stopped, so no more Schwab quotes: {}",
                  reason);
    mSocket->StopReconnecting();
    mSocket->Close();
}

void SchwabStreamer::OnEvent(std::uint64_t generation,
                             SchwabStreamerSocket::Event event,
                             const std::string& text) {
    switch (event) {
        case SchwabStreamerSocket::Event::kOpen:
            OnOpen(generation);
            return;
        case SchwabStreamerSocket::Event::kClose: {
            std::lock_guard lock(mMutex);
            if (Current(generation)) {
                mLoggedIn = false;
                if (!mHalted) {
                    spdlog::warn("schwab streamer: closed ({})", text);
                }
            }
            return;
        }
        case SchwabStreamerSocket::Event::kMessage:
            break;
    }
    auto parsed = util::ParseJson(text);
    if (!parsed || !parsed->is_object()) {
        spdlog::warn("schwab streamer: unparseable message");
        return;
    }
    const auto each = [&](const char* kind, auto&& handle) {
        const auto items = parsed->find(kind);
        if (items == parsed->end() || !items->is_array()) {
            return;
        }
        for (const auto& item : *items) {
            if (item.is_object()) {
                handle(item);
            }
        }
    };
    each("response", [&](const json& item) { OnResponse(generation, item); });
    each("notify", [&](const json& item) { OnNotify(generation, item); });
    each("data", [&](const json& item) { OnData(generation, item); });
}

void SchwabStreamer::OnOpen(std::uint64_t generation) {
    {
        std::lock_guard dispatch(mDispatch);
        mMerger.Reset();
    }
    {
        std::lock_guard lock(mMutex);
        if (!Current(generation) || mHalted) {
            return;
        }
        mLoggedIn = false;
        if (mResumeAt) {
            // A reconnect begun before the takeover was seen: logging in
            // now would take the connection back before the wait is over.
            mSocket->Close();
            return;
        }
    }
    // Not under mMutex: a renewal is a request to the token endpoint.
    auto token = mApi->Auth().AccessToken();
    std::lock_guard lock(mMutex);
    if (!Current(generation) || mHalted || mResumeAt) {
        return;
    }
    if (!token) {
        if (token.error().category == ErrorCategory::kAuth) {
            Halt(token.error().message);
        } else {
            spdlog::warn("schwab streamer: no access token to log in with: {}",
                         token.error().message);
            mSocket->Close();
        }
        return;
    }
    mLoginToken = *token;
    Send("ADMIN", "LOGIN",
         {{"Authorization", *token},
          {"SchwabClientChannel", mInfo->channel},
          {"SchwabClientFunctionId", mInfo->function_id}});
}

void SchwabStreamer::OnResponse(std::uint64_t generation, const json& item) {
    const std::string service = item.value("service", "");
    const std::string command = item.value("command", "");
    const auto [code, message] = Outcome(item);
    std::string denied_token;
    {
        std::lock_guard lock(mMutex);
        if (!Current(generation) || mHalted) {
            return;
        }
        if (service != "ADMIN" || command != "LOGIN") {
            if (!Succeeded(code)) {
                spdlog::error("schwab streamer: {} {} failed: code {}: {}",
                              service, command, code, message);
            }
            return;
        }
        if (code == kSuccess) {
            spdlog::info("schwab streamer: logged in ({})", message);
            mLoggedIn = true;
            mDenials = 0;
            std::set<std::string> keys;
            for (const auto& [key, count] : mKeys) {
                keys.insert(key);
            }
            if (!keys.empty()) {
                SendKeys("SUBS", keys);
            }
            return;
        }
        if (code != kLoginDenied || ++mDenials > 1) {
            Halt("login refused: code " + std::to_string(code) + ": " +
                 message);
            return;
        }
        spdlog::warn(
            "schwab streamer: login denied ({}); renewing the access token",
            message);
        denied_token = mLoginToken;
        mSocket->Close();
    }
    // Not under mMutex, like any call into SchwabAuth.
    mApi->Auth().Invalidate(denied_token);
}

void SchwabStreamer::OnNotify(std::uint64_t generation, const json& item) {
    if (item.contains("heartbeat")) {
        return;
    }
    const auto [code, message] = Outcome(item);
    std::lock_guard lock(mMutex);
    if (!Current(generation) || mHalted) {
        return;
    }
    if (code == kCloseConnection && mTakeoverReconnect.count() == 0) {
        Halt(
            "another streamer connection for this Schwab user took over (" +
            message +
            "); only one is allowed at a time. To stream here, stop the other "
            "one (another kestrel_app, or thinkorswim) and start again, or set "
            "[schwab] takeover_reconnect_ms to take it back after a wait");
    } else if (code == kCloseConnection) {
        spdlog::warn(
            "schwab streamer: another streamer connection for this Schwab user "
            "took over ({}); reconnecting in {} s, which takes it back",
            message, mTakeoverReconnect.count() / 1000.0);
        mLoggedIn = false;
        mSocket->StopReconnecting();
        mResumeAt = std::chrono::steady_clock::now() + mTakeoverReconnect;
        if (!mResumer.joinable()) {
            mResumer = std::thread([this] { ResumeLoop(); });
        }
        mResumeWake.notify_all();
    } else if (code == kStopStreaming) {
        spdlog::warn("schwab streamer: Schwab stopped streaming: {}", message);
    } else {
        spdlog::warn("schwab streamer: notice code {}: {}", code, message);
    }
}

void SchwabStreamer::OnData(std::uint64_t generation, const json& item) {
    if (item.value("service", "") != "LEVELONE_EQUITIES") {
        return;
    }
    const auto content = item.find("content");
    if (content == item.end()) {
        return;
    }
    std::lock_guard dispatch(mDispatch);
    const auto quotes = mMerger.Apply(*content, mClock());
    if (quotes.empty()) {
        return;
    }
    std::vector<std::shared_ptr<Consumer>> consumers;
    {
        std::lock_guard lock(mMutex);
        if (!Current(generation)) {
            return;
        }
        for (const auto& [id, consumer] : mConsumers) {
            consumers.push_back(consumer);
        }
    }
    for (const auto& keyed : quotes) {
        for (const auto& consumer : consumers) {
            if (!consumer->keys.contains(keyed.key)) {
                continue;
            }
            try {
                consumer->callback(keyed.quote);
            } catch (const std::exception& e) {
                spdlog::error("schwab streamer: quote callback threw: {}",
                              e.what());
            }
        }
    }
}

}  // namespace kestrel::data
