#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "kestrel/data/data_source.hpp"
#include "kestrel/data/schwab_auth.hpp"

namespace kestrel::data {

// Where and as whom to connect to Schwab's streamer: `streamerInfo` from
// GET /trader/v1/userPreference.
struct SchwabStreamerInfo {
    std::string socket_url;
    std::string customer_id;
    std::string correl_id;
    std::string channel;
    std::string function_id;
};

// Reads `streamerInfo` from a userPreference body. kParse if it is missing.
types::Result<SchwabStreamerInfo> ParseSchwabStreamerInfo(
    std::string_view body);

// The LEVELONE_EQUITIES fields asked for: bid, ask, last, bid size, ask size,
// last size, security status, quote time, trade time.
inline constexpr std::string_view kSchwabLevelOneFields =
    "0,1,2,3,4,5,9,32,34,35";

// A quote for one Schwab key (the symbol as Schwab writes it, e.g. "BRK/B").
struct SchwabKeyedQuote {
    std::string key;
    types::Quote quote;
};

// Turns LEVELONE_EQUITIES data into Quotes, keeping the latest known value of
// every field per key, as Schwab sends only the fields that changed.
//
// Bid and ask sizes are in shares, like the last size: the Streamer document
// says lots of 100, but live they match REST /quotes, which gives shares. A
// price of 0 is no price (Schwab's value before any quote or trade), so it is
// not kept. A key with security status "Unknown" is not a symbol Schwab
// knows: it is logged once and gives no quotes. Delayed (non-SIP) data is
// logged once per key. The exchange time is the later of the quote and trade
// times in the update. Schwab does not say which side took liquidity.
class SchwabLevelOneMerger {
   public:
    // `content` is the `content` array of one LEVELONE_EQUITIES data item,
    // from util::ParseJson.
    std::vector<SchwabKeyedQuote> Apply(const nlohmann::json& content,
                                        types::Timestamp received);
    // Forgets every key (on reconnect, so stale fields cannot leak into the
    // fresh snapshot), or one key that is no longer subscribed.
    void Reset();
    void Forget(const std::string& key);

   private:
    std::unordered_map<std::string, types::Quote> mState;
    std::set<std::string> mUnknown;  // keys logged as unknown
    std::set<std::string> mDelayed;  // keys logged as delayed
};

// The streamer's WebSocket, as SchwabStreamer sees it, so tests can stand in
// for it. Events arrive on one background thread.
class SchwabStreamerSocket {
   public:
    enum class Event { kOpen, kMessage, kClose };
    using Handler = std::function<void(Event, const std::string& text)>;

    virtual ~SchwabStreamerSocket() = default;
    // Connects to `url`, and after any close reconnects with backoff until
    // StopReconnecting() or Stop().
    virtual void Start(const std::string& url, Handler handler) = 0;
    virtual void Send(const std::string& text) = 0;
    // Closes the current connection (a reconnect follows). Callable from the
    // handler.
    virtual void Close() = 0;
    // Stops reconnecting once the current connection closes. Callable from
    // the handler.
    virtual void StopReconnecting() = 0;
    // Closes for good and joins the thread: never from the handler.
    virtual void Stop() = 0;
};

// The real socket, on IXWebSocket.
std::unique_ptr<SchwabStreamerSocket> MakeSchwabStreamerSocket();

// One connection to Schwab's streamer, shared by every subscription in the
// process: Schwab allows one streamer connection per user, and a second
// LOGIN disconnects the first (notify code 12). Thread-safe. Hold it in a
// shared_ptr: subscriptions keep it alive.
//
// The first subscription fetches the streamer info (GET userPreference) and
// connects; on every (re)connect the streamer logs in with a current access
// token and, once the login succeeds, subscribes to every symbol wanted.
// Later subscriptions ADD their new symbols, and a stopped one UNSUBS the
// symbols nobody else wants. The connection closes when the last
// subscription stops, and the next one connects again.
//
// A denied login renews the access token and reconnects; a second denial in
// a row, or a token only re-authorizing can fix, stops the streamer. So does
// code 12: another connection for this user (another Kestrel process, or a
// Schwab app) has taken over, and reconnecting would only take it back and
// set the two fighting. After a stop the existing subscriptions get no more
// quotes, which is logged as an error, and new ones are refused.
class SchwabStreamer : public std::enable_shared_from_this<SchwabStreamer> {
   public:
    using SocketFactory =
        std::function<std::unique_ptr<SchwabStreamerSocket>()>;
    using Clock = std::function<types::Timestamp()>;

    // `trader_url` is the Trader API's base, e.g.
    // https://api.schwabapi.com/trader/v1.
    SchwabStreamer(std::shared_ptr<SchwabApi> api, std::string trader_url,
                   SocketFactory make_socket, Clock clock = types::Now);
    ~SchwabStreamer();
    SchwabStreamer(const SchwabStreamer&) = delete;
    SchwabStreamer& operator=(const SchwabStreamer&) = delete;

    // Streams LEVELONE_EQUITIES quotes for `keys` (Schwab symbols) to
    // `callback`, on the socket's thread. kRejected once the streamer has
    // stopped; the userPreference request's error if it fails.
    types::Result<std::unique_ptr<Subscription>> SubscribeQuotes(
        const std::vector<std::string>& keys, QuoteCallback callback);

   private:
    class QuoteSubscription;
    struct Consumer {
        std::set<std::string> keys;
        QuoteCallback callback;
    };

    types::Result<SchwabStreamerInfo> FetchInfo();
    void Remove(std::uint64_t id);
    void OnEvent(std::uint64_t generation, SchwabStreamerSocket::Event event,
                 const std::string& text);
    void OnOpen(std::uint64_t generation);
    void OnResponse(std::uint64_t generation, const nlohmann::json& item);
    void OnNotify(std::uint64_t generation, const nlohmann::json& item);
    void OnData(std::uint64_t generation, const nlohmann::json& item);
    // Whether events from socket `generation` still count. Requires mMutex.
    bool Current(std::uint64_t generation) const;
    // Sends one request on mSocket. Requires mMutex.
    void Send(std::string_view service, std::string_view command,
              nlohmann::json parameters);
    // Sends a LEVELONE_EQUITIES command for `keys`. Requires mMutex.
    void SendKeys(std::string_view command, const std::set<std::string>& keys);
    // Stops for good, with `reason` logged. Requires mMutex.
    void Halt(const std::string& reason);

    const std::shared_ptr<SchwabApi> mApi;
    const std::string mTraderUrl;
    const SocketFactory mMakeSocket;
    const Clock mClock;

    // Held while quotes are merged and callbacks run, so a subscription that
    // has stopped is never called again. Never taken while holding mMutex.
    std::mutex mDispatch;
    SchwabLevelOneMerger mMerger;  // guarded by mDispatch

    std::mutex mMutex;  // guards the members below
    std::map<std::uint64_t, std::shared_ptr<Consumer>> mConsumers;
    std::map<std::string, int> mKeys;  // key -> consumers that want it
    std::uint64_t mNextConsumer = 1;
    std::unique_ptr<SchwabStreamerSocket> mSocket;
    std::uint64_t mGeneration = 0;  // of mSocket; older events are ignored
    std::optional<SchwabStreamerInfo> mInfo;  // of mSocket's connection
    std::string mLoginToken;  // the access token of the last LOGIN
    bool mLoggedIn = false;
    int mDenials = 0;                    // denied logins since the last success
    std::optional<std::string> mHalted;  // why the streamer stopped
    std::uint64_t mNextRequest = 1;
};

}  // namespace kestrel::data
