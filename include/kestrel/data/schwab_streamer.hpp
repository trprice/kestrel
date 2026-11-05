#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <thread>
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

// The CHART_EQUITY fields asked for. Live (Nov 2026) they are numbered one
// higher than the Streamer document says: 1 is the minute's sequence, 2 to 5
// open, high, low and close, 6 the volume, 7 the minute's start (epoch ms)
// and 8 the day.
inline constexpr std::string_view kSchwabChartFields = "0,1,2,3,4,5,6,7";

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

// A closed 1-minute bar for one Schwab key.
struct SchwabKeyedBar {
    std::string key;
    types::Bar bar;
};

// Reads the bars in the `content` array of one CHART_EQUITY data item, from
// util::ParseJson. An item without a key, a price, the volume or the time is
// logged and skipped.
std::vector<SchwabKeyedBar> ParseSchwabChartBars(const nlohmann::json& content);

// Whether a 1-minute bar opening at `open_time` is in the US equity regular
// session: 09:30 to 16:00 New York time, Monday to Friday. Holidays and early
// closes are not known (on a holiday there are no bars anyway).
bool InUsRegularSession(types::Timestamp open_time);

// Builds the bars of one timeframe from one key's closed 1-minute bars, as
// CHART_EQUITY streams only those. Bars open at multiples of the timeframe
// from the epoch (so 30-minute bars at :00 and :30).
//
// A bar is given only if every minute of it could have been seen: its start
// is not before the first minute added since Reset (the stream's start, or a
// reconnect), as minutes before that were missed. A minute with no trades
// has no bar, so a bar closes with its last minute, or failing that with the
// first minute of a later one. Minutes not after the last one added, even
// across Reset, are ignored: Schwab sends the last closed minute again on
// every subscribe.
class SchwabBarAggregator {
   public:
    explicit SchwabBarAggregator(std::chrono::minutes step);
    // The bars that closed with `minute`, oldest first.
    std::vector<types::Bar> Add(const types::Bar& minute);
    // Starts again after minutes may have been missed.
    void Reset();

   private:
    std::chrono::minutes mStep;
    std::optional<types::Timestamp> mSince;  // the first minute since Reset
    std::optional<types::Timestamp> mLast;   // the last minute added
    std::optional<types::Bar> mPending;      // the bar being built
};

// The streamer's WebSocket, as SchwabStreamer sees it, so tests can stand in
// for it. Events arrive on one background thread.
class SchwabStreamerSocket {
   public:
    enum class Event { kOpen, kMessage, kClose };
    using Handler = std::function<void(Event, const std::string& text)>;

    virtual ~SchwabStreamerSocket() = default;
    SchwabStreamerSocket() = default;
    // An interface: implementations are held by pointer, never copied.
    SchwabStreamerSocket(const SchwabStreamerSocket&) = delete;
    SchwabStreamerSocket& operator=(const SchwabStreamerSocket&) = delete;
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
// LOGIN disconnects the first (notify code 12). It streams quotes
// (LEVELONE_EQUITIES) and bars (CHART_EQUITY) on that one connection.
// Thread-safe. Hold it in a
// shared_ptr: subscriptions keep it alive.
//
// The first subscription fetches the streamer info (GET userPreference) and
// connects; on every (re)connect the streamer logs in with a current access
// token and, once the login succeeds, subscribes to every symbol wanted on
// each service.
// Later subscriptions ADD their new symbols, and a stopped one UNSUBS the
// symbols nobody else wants. The connection closes when the last
// subscription stops, and the next one connects again.
//
// A denied login renews the access token and reconnects; a second denial in
// a row, or a token only re-authorizing can fix, stops the streamer. After a
// stop the existing subscriptions get no more data, which is logged as an
// error, and new ones are refused.
//
// Code 12 means another connection for this user (another Kestrel process,
// or thinkorswim) has taken over, and reconnecting takes it back. With
// `takeover_reconnect` zero, the default, that stops the streamer too: the
// user is presumed to want the other one. Otherwise the streamer waits that
// long, without logging in, then connects afresh; if the other one takes
// over again, so does the next wait.
class SchwabStreamer : public std::enable_shared_from_this<SchwabStreamer> {
   public:
    using SocketFactory =
        std::function<std::unique_ptr<SchwabStreamerSocket>()>;
    using Clock = std::function<types::Timestamp()>;

    // `trader_url` is the Trader API's base, e.g.
    // https://api.schwabapi.com/trader/v1.
    SchwabStreamer(std::shared_ptr<SchwabApi> api, std::string trader_url,
                   SocketFactory make_socket,
                   std::chrono::milliseconds takeover_reconnect = {},
                   Clock clock = types::Now);
    ~SchwabStreamer();
    SchwabStreamer(const SchwabStreamer&) = delete;
    SchwabStreamer& operator=(const SchwabStreamer&) = delete;

    // Streams LEVELONE_EQUITIES quotes for `keys` (Schwab symbols) to
    // `callback`, on the socket's thread. kRejected once the streamer has
    // stopped; the userPreference request's error if it fails.
    types::Result<std::unique_ptr<Subscription>> SubscribeQuotes(
        const std::vector<std::string>& keys, QuoteCallback callback);
    // Streams `timeframe` bars for `keys`, built from CHART_EQUITY's 1-minute
    // bars by SchwabBarAggregator, to `callback` on the socket's thread.
    // `timeframe` must be a whole number of minutes (kInvalidArgument
    // otherwise); the rest is as SubscribeQuotes.
    types::Result<std::unique_ptr<Subscription>> SubscribeBars(
        const std::vector<std::string>& keys, types::Timeframe timeframe,
        BarCallback callback);

   private:
    class ConsumerSubscription;
    enum class Service : std::uint8_t { kLevelOne, kChart };
    // The service as Schwab names it.
    static std::string_view ServiceName(Service service);
    struct Consumer {
        Service service = Service::kLevelOne;
        std::set<std::string> keys;
        QuoteCallback on_quote;  // Service::kLevelOne
        BarCallback on_bar;      // Service::kChart
        types::Timeframe timeframe{};
        std::chrono::minutes step{1};
        // Per key; guarded by mDispatch.
        std::map<std::string, SchwabBarAggregator> aggregators;
    };

    types::Result<SchwabStreamerInfo> FetchInfo();
    types::Result<std::unique_ptr<Subscription>> Add(
        std::shared_ptr<Consumer> consumer);
    void OnQuotes(std::uint64_t generation, const nlohmann::json& content);
    void OnBars(std::uint64_t generation, const nlohmann::json& content);
    // The consumers of `service`. Requires mMutex.
    std::vector<std::shared_ptr<Consumer>> ConsumersOf(Service service) const;
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
    // Sends a `service` command for `keys`. Requires mMutex.
    void SendKeys(Service service, std::string_view command,
                  const std::set<std::string>& keys);
    // Stops for good, with `reason` logged. Requires mMutex.
    void Halt(const std::string& reason);
    // Makes mSocket a new socket and starts it. Requires mMutex.
    void StartSocket();
    // The thread that reconnects after a takeover, once mResumeAt comes.
    void ResumeLoop();

    const std::shared_ptr<SchwabApi> mApi;
    const std::string mTraderUrl;
    const SocketFactory mMakeSocket;
    const std::chrono::milliseconds mTakeoverReconnect;
    const Clock mClock;

    // Held while quotes are merged and callbacks run, so a subscription that
    // has stopped is never called again. Never taken while holding mMutex.
    std::mutex mDispatch;
    SchwabLevelOneMerger mMerger;  // guarded by mDispatch

    std::mutex mMutex;  // guards the members below
    std::map<std::uint64_t, std::shared_ptr<Consumer>> mConsumers;
    // Per service, key -> consumers that want it.
    std::map<Service, std::map<std::string, int>> mKeys;
    std::uint64_t mNextConsumer = 1;
    std::unique_ptr<SchwabStreamerSocket> mSocket;
    std::uint64_t mGeneration = 0;  // of mSocket; older events are ignored
    std::optional<SchwabStreamerInfo> mInfo;  // of mSocket's connection
    std::string mLoginToken;  // the access token of the last LOGIN
    bool mLoggedIn = false;
    int mDenials = 0;                    // denied logins since the last success
    std::optional<std::string> mHalted;  // why the streamer stopped
    std::uint64_t mNextRequest = 1;
    // When to reconnect after a takeover; until then nothing logs in.
    std::optional<std::chrono::steady_clock::time_point> mResumeAt;
    bool mClosing = false;                // the destructor has begun
    std::condition_variable mResumeWake;  // with mMutex

    std::thread mResumer;  // started at the first takeover
};

}  // namespace kestrel::data
