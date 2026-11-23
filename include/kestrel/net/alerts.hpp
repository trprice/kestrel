#pragma once

#include <spdlog/common.h>
#include <spdlog/sinks/sink.h>

#include <chrono>
#include <cstddef>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "kestrel/net/http_client.hpp"
#include "kestrel/types/error.hpp"

namespace kestrel::net {

// Alerts: error lines from the log posted to a Discord webhook, so a problem
// in an unattended run reaches a phone. `run` makes one Alerter, adds its
// Sink() to spdlog's default logger at error level, and posts a notice when
// it starts and stops (see app/commands/alerts.cpp).
//
// Nothing here may block or throw into the thread that logs: the sink only
// queues (a bounded queue that counts what it drops), and one worker thread
// redacts, deduplicates, batches and posts.

// Whether `url` is a Discord webhook: https://discord.com/api[/vN]/webhooks/
// <id>/<token>, or the canary. or ptb. host. Redirects are refused (see
// CprHttpClient), so an old discordapp.com URL would fail every post.
bool IsDiscordWebhookUrl(std::string_view url);

// What may not leave the machine, masked in every message: each of
// `secrets` as given (an API key, the webhook), any run of 32 or more
// letters and digits (a Schwab account hash, a token), and the amount after
// "balance of", "balance is", "balance is still" and "equity". Prices,
// quantities, P&L and order IDs are kept. The amounts are matched by
// wording: a new log line that prints a balance differently is not masked.
std::string RedactAlert(std::string_view text,
                        const std::vector<std::string>& secrets);

// What makes two lines "the same" for deduplication: the text with each run
// of digits replaced by '#', so lines that differ only in a time, an order ID
// or a count are one line.
std::string AlertDedupKey(std::string_view text);

// The longest prefix of `text` of at most `max_bytes` that does not cut a
// UTF-8 character in two.
std::string_view TruncateUtf8(std::string_view text, std::size_t max_bytes);

// The Discord message: `header` in bold, then `lines` in a code block (so
// max_drawdown_close is not italicised). Backtick runs in the lines are
// broken up so they cannot end the block early.
std::string FormatAlertMessage(std::string_view header,
                               const std::vector<std::string>& lines);

// The JSON body of a webhook post of `content`, with mentions turned off
// (an "@everyone" in a log line pings no one). Invalid UTF-8 is replaced
// rather than thrown on.
std::string DiscordWebhookBody(std::string_view content);

// The worker's lines between posts: deduplicated, then taken a message at a
// time. Only the worker uses it; exposed for tests.
class AlertBatch {
   public:
    using Time = std::chrono::steady_clock::time_point;

    explicit AlertBatch(std::chrono::milliseconds dedup_window,
                        std::size_t max_lines = 200,
                        std::size_t max_keys = 256);

    // Queues `line`. With `dedup`, a line like one sent within the window is
    // only counted, and summarised when the window ends.
    void Add(std::string line, bool dedup, Time now);
    // Lines lost before they got here (the sink's queue was full).
    void NoteDropped(std::size_t count);
    // Queues a summary for each window that has ended with repeats in it.
    void Expire(Time now);
    // Queues every summary now, ended or not (on shutdown).
    void FlushSummaries();
    // When the next window ends, if one is open.
    std::optional<Time> NextExpiry() const;

    bool Empty() const;
    // Lines queued, and lines noted as dropped, not yet taken.
    std::size_t Size() const;
    // As many queued lines as fit in `max_bytes` (joined by newlines), the
    // oldest first; a line longer than that is cut. A note of lines dropped
    // comes first.
    std::vector<std::string> Take(std::size_t max_bytes);
    // Puts back what Take returned, after a post that failed, to go first.
    void PutBack(std::vector<std::string> lines);

   private:
    struct Seen {
        Time first;
        std::size_t repeats = 0;
        std::string last;
    };
    void Push(std::string line);
    void Summarise(const Seen& seen);

    std::chrono::milliseconds mWindow;
    std::size_t mMaxLines;
    std::size_t mMaxKeys;
    std::deque<std::string> mLines;
    std::map<std::string, Seen> mSeen;
    std::size_t mDropped = 0;
};

struct AlerterOptions {
    // Bold first line of every message: which process this is, e.g.
    // "trend-paper (trend_ensemble, paper, binanceus)".
    std::string header;
    // Masked wherever they appear (see RedactAlert); the webhook URL is
    // always added.
    std::vector<std::string> secrets;
    // Between posts, so a burst arrives as one message and stays under
    // Discord's limit of 5 posts in 2 s per webhook.
    std::chrono::milliseconds min_gap{2000};
    std::chrono::milliseconds dedup_window{std::chrono::minutes(5)};
    std::size_t queue_capacity = 64;
    // Discord allows 2000 characters; bytes are counted, which is stricter.
    std::size_t max_message_bytes = 1900;
    // How long Close() keeps posting what is left.
    std::chrono::milliseconds close_timeout{std::chrono::seconds(10)};
};

class Alerter {
   public:
    Alerter(std::shared_ptr<HttpClient> http, std::string webhook_url,
            AlerterOptions options);
    // Close()s.
    ~Alerter();
    Alerter(const Alerter&) = delete;
    Alerter& operator=(const Alerter&) = delete;

    // An spdlog sink that queues each line it gets for posting; set its level
    // (error, say) and add it to a logger. It outlives the Alerter safely: once
    // closed, it drops what it gets. Lines logged on the worker thread itself
    // are dropped, so a failed post's warning cannot loop.
    std::shared_ptr<spdlog::sinks::sink> Sink();

    // Queues a notice to post as it is (not deduplicated), e.g. "started".
    void Notify(std::string_view text) noexcept;

    // Posts what is queued, with the summaries of repeats, for up to
    // close_timeout (less if a retry would come after it), and stops the
    // worker, logging how many lines went unsent. Afterwards nothing is
    // queued.
    void Close();

    // Posts `text` now, on the calling thread, redacted, under the header;
    // for `kestrel_app alert`. Independent of the worker, but not to be
    // called alongside Close(), which releases the HTTP client: Close(),
    // the destructor and SendNow() belong to the thread that owns the
    // Alerter.
    types::Result<void> SendNow(std::string_view text);

    struct Core;

   private:
    std::shared_ptr<Core> mCore;
    std::thread mWorker;
};

}  // namespace kestrel::net
