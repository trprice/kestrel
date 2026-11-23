#include "kestrel/net/alerts.hpp"

#include <spdlog/sinks/base_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <condition_variable>
#include <format>
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <regex>
#include <utility>

namespace kestrel::net {

namespace {

using Clock = std::chrono::steady_clock;

// Set on the worker thread: what it logs itself is not queued again.
thread_local bool tOnAlertWorker = false;

void ReplaceAll(std::string& s, std::string_view from, std::string_view to) {
    if (from.empty()) {
        return;
    }
    for (std::size_t at = s.find(from); at != std::string::npos;
         at = s.find(from, at + to.size())) {
        s.replace(at, from.size(), to);
    }
}

// Two backticks in a row (the start of a ``` that would end the code block)
// get a zero-width space between them. Applying it twice changes nothing.
std::string EscapeLine(std::string line) {
    static constexpr std::string_view kZeroWidthSpace = "\xE2\x80\x8B";
    for (std::size_t at = line.find("``"); at != std::string::npos;
         at = line.find("``", at + 1 + kZeroWidthSpace.size())) {
        line.insert(at + 1, kZeroWidthSpace);
    }
    return line;
}

// Markdown in the bold header (trend_ensemble's underscore) shown as is.
std::string EscapeMarkdown(std::string_view text) {
    std::string out;
    for (const char c : text) {
        if (c == '\n' || c == '\r' || c == '\t') {
            out += ' ';  // the header is one line, or the bold breaks
            continue;
        }
        if (std::string_view("\\*_~`|>").find(c) != std::string_view::npos) {
            out += '\\';
        }
        out += c;
    }
    return out;
}

std::string WindowText(std::chrono::milliseconds window) {
    const auto seconds =
        std::chrono::duration_cast<std::chrono::seconds>(window).count();
    if (seconds >= 60 && seconds % 60 == 0) {
        return std::to_string(seconds / 60) + " min";
    }
    return std::to_string(seconds) + " s";
}

}  // namespace

bool IsDiscordWebhookUrl(std::string_view url) {
    static const std::regex kWebhook(
        R"(^https://(discord\.com|canary\.discord\.com|ptb\.discord\.com))"
        R"(/api(/v[0-9]+)?/webhooks/[0-9]+/[A-Za-z0-9_-]+$)");
    return std::regex_match(url.begin(), url.end(), kWebhook);
}

std::string RedactAlert(std::string_view text,
                        const std::vector<std::string>& secrets) {
    std::string out(text);
    // Longest first, so a secret inside another cannot mask part of it and
    // leave the rest showing.
    std::vector<std::string_view> ordered(secrets.begin(), secrets.end());
    std::ranges::stable_sort(ordered, std::greater{}, &std::string_view::size);
    for (const auto secret : ordered) {
        if (!secret.empty()) {
            ReplaceAll(out, secret, "<secret>");
        }
    }
    // Runs of 32 or more letters and digits, scanned by hand: std::regex
    // recurses per character it matches, and a long line could overflow
    // the worker's stack.
    std::string masked;
    masked.reserve(out.size());
    for (std::size_t i = 0; i < out.size();) {
        std::size_t end = i;
        // ASCII only, whatever the locale.
        const auto alnum = [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                   (c >= 'a' && c <= 'z');
        };
        while (end < out.size() && alnum(out[end])) {
            ++end;
        }
        if (end - i >= 32) {
            masked += "<redacted>";
        } else {
            masked.append(out, i, end - i);
        }
        if (end == i) {
            masked += out[i];
            ++end;
        }
        i = end;
    }
    // No message holds more than this; the cut comes after the masking
    // above, so it cannot leave part of a secret or token showing.
    constexpr std::size_t kMaxLineBytes = 4096;
    out = std::string(TruncateUtf8(masked, kMaxLineBytes));
    static const std::regex kAmount(
        R"(\b(balance of|balance is still|balance is|equity) )"
        R"(-?\$?-?[0-9][0-9,]*(\.[0-9]+)?)",
        std::regex::ECMAScript | std::regex::icase);
    return std::regex_replace(out, kAmount, "$1 <amount>");
}

std::string AlertDedupKey(std::string_view text) {
    std::string key;
    key.reserve(text.size());
    for (const char c : text) {
        const bool digit = c >= '0' && c <= '9';
        if (!digit) {
            key += c;
        } else if (key.empty() || key.back() != '#') {
            key += '#';
        }
    }
    return key;
}

std::string_view TruncateUtf8(std::string_view text, std::size_t max_bytes) {
    if (text.size() <= max_bytes) {
        return text;
    }
    std::size_t end = max_bytes;
    // Back up to the first byte of the character the cut falls in.
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) {
        --end;
    }
    return text.substr(0, end);
}

std::string FormatAlertMessage(std::string_view header,
                               const std::vector<std::string>& lines) {
    std::string out = "**" + EscapeMarkdown(header) + "**";
    if (lines.empty()) {
        return out;
    }
    out += "\n```\n";
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) {
            out += '\n';
        }
        out += EscapeLine(lines[i]);
    }
    out += "\n```";
    return out;
}

std::string DiscordWebhookBody(std::string_view content) {
    nlohmann::json body;
    body["content"] = std::string(content);
    body["allowed_mentions"] = {{"parse", nlohmann::json::array()}};
    return body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

// --- AlertBatch ------------------------------------------------------------

AlertBatch::AlertBatch(std::chrono::milliseconds dedup_window,
                       std::size_t max_lines, std::size_t max_keys)
    : mWindow(dedup_window),
      mMaxLines(max_lines),
      mMaxKeys(std::max<std::size_t>(max_keys, 1)) {}

void AlertBatch::Push(std::string line) {
    if (mLines.size() >= mMaxLines) {
        ++mDropped;
        return;
    }
    mLines.push_back(std::move(line));
}

void AlertBatch::Summarise(const Seen& seen) {
    if (seen.repeats == 0) {
        return;
    }
    Push(std::format("({} more like this in {}; the last: {})", seen.repeats,
                     WindowText(mWindow), seen.last));
}

void AlertBatch::Add(std::string line, bool dedup, Time now) {
    if (!dedup) {
        Push(std::move(line));
        return;
    }
    std::string key = AlertDedupKey(line);
    if (const auto it = mSeen.find(key); it != mSeen.end()) {
        if (now - it->second.first < mWindow) {
            ++it->second.repeats;
            it->second.last = std::move(line);
            return;
        }
        Summarise(it->second);
        mSeen.erase(it);
    } else if (mSeen.size() >= mMaxKeys) {
        const auto oldest = std::ranges::min_element(
            mSeen, {}, [](const auto& entry) { return entry.second.first; });
        Summarise(oldest->second);
        mSeen.erase(oldest);
    }
    mSeen.emplace(std::move(key), Seen{now, 0, {}});
    Push(std::move(line));
}

void AlertBatch::NoteDropped(std::size_t count) { mDropped += count; }

void AlertBatch::Expire(Time now) {
    for (auto it = mSeen.begin(); it != mSeen.end();) {
        if (now - it->second.first >= mWindow) {
            Summarise(it->second);
            it = mSeen.erase(it);
        } else {
            ++it;
        }
    }
}

void AlertBatch::FlushSummaries() {
    for (const auto& [key, seen] : mSeen) {
        Summarise(seen);
    }
    mSeen.clear();
}

std::optional<AlertBatch::Time> AlertBatch::NextExpiry() const {
    std::optional<Time> next;
    for (const auto& [key, seen] : mSeen) {
        if (seen.repeats > 0) {
            const Time end = seen.first + mWindow;
            next = next ? std::min(*next, end) : end;
        }
    }
    return next;
}

bool AlertBatch::Empty() const { return mLines.empty() && mDropped == 0; }

std::size_t AlertBatch::Size() const { return mLines.size() + mDropped; }

std::vector<std::string> AlertBatch::Take(std::size_t max_bytes) {
    if (mDropped > 0) {
        mLines.push_front(std::format("({} lines dropped: too many at once)",
                                      std::exchange(mDropped, 0)));
    }
    std::vector<std::string> taken;
    std::size_t used = 0;
    while (!mLines.empty()) {
        std::string line = EscapeLine(std::move(mLines.front()));
        const std::size_t need = line.size() + (taken.empty() ? 0 : 1);
        if (used + need <= max_bytes) {
            used += need;
            taken.push_back(std::move(line));
            mLines.pop_front();
            continue;
        }
        if (taken.empty()) {
            // One line longer than a message: send what fits of it.
            static constexpr std::string_view kCut = "...";
            const std::size_t room =
                max_bytes > kCut.size() ? max_bytes - kCut.size() : 0;
            taken.push_back(std::string(TruncateUtf8(line, room)) +
                            std::string(kCut));
            mLines.pop_front();
        } else {
            mLines.front() = std::move(line);
        }
        break;
    }
    return taken;
}

void AlertBatch::PutBack(std::vector<std::string> lines) {
    mLines.insert(mLines.begin(), std::make_move_iterator(lines.begin()),
                  std::make_move_iterator(lines.end()));
}

// --- Alerter ---------------------------------------------------------------

struct Alerter::Core {
    struct Item {
        std::string text;
        bool dedup = true;
    };

    std::shared_ptr<HttpClient> http;
    std::string url;
    AlerterOptions options;

    std::mutex mutex;  // guards intake, dropped and closed
    std::condition_variable wake;
    std::deque<Item> intake;
    std::size_t dropped = 0;
    bool closed = false;

    // Never blocks for long, never throws: called while logging.
    void Queue(std::string text, bool dedup) noexcept {
        try {
            {
                std::lock_guard lock(mutex);
                if (closed) {
                    return;
                }
                if (intake.size() >= options.queue_capacity) {
                    ++dropped;
                    return;
                }
                intake.push_back(Item{std::move(text), dedup});
            }
            wake.notify_one();
        } catch (...) {
        }
    }

    std::size_t Budget() const {
        const std::size_t overhead =
            FormatAlertMessage(options.header, {std::string()}).size();
        return options.max_message_bytes > overhead
                   ? options.max_message_bytes - overhead
                   : 0;
    }

    types::Result<void> Post(const std::vector<std::string>& lines) {
        HttpRequest request;
        request.method = HttpMethod::kPost;
        request.url = url;
        // Errors and logs name the request by this, never by the URL.
        request.label = "discord webhook";
        request.headers["Content-Type"] = "application/json";
        request.body =
            DiscordWebhookBody(FormatAlertMessage(options.header, lines));
        auto sent = http->Send(request);
        if (!sent) {
            return std::unexpected(sent.error());
        }
        return {};
    }

    // Between posts; shorter once closing, but still inside Discord's 5
    // posts in 2 s, so a backlog does not run into 429s.
    std::chrono::milliseconds Gap(bool closing) const {
        constexpr std::chrono::milliseconds kClosingGap{500};
        return closing ? std::min(options.min_gap, kClosingGap)
                       : options.min_gap;
    }

    // On the worker, so not posted itself.
    static void Unsent(std::size_t count) {
        if (count > 0) {
            spdlog::warn("alerts: stopping with {} line(s) not sent", count);
        }
    }

    void Run();
};

void Alerter::Core::Run() {
    tOnAlertWorker = true;
    AlertBatch batch(options.dedup_window);
    Clock::time_point next_post{};
    std::chrono::milliseconds backoff{0};
    // Whether next_post is only min_gap after a post, which closing skips,
    // rather than a retry's wait, which it keeps to.
    bool gap_only = true;
    bool failing = false;
    bool seen_close = false;
    Clock::time_point close_deadline{};
    for (;;) {
        if (seen_close && batch.Empty()) {
            return;  // all posted; nothing more can be queued
        }
        std::deque<Item> items;
        std::size_t lost = 0;
        {
            std::unique_lock lock(mutex);
            std::optional<Clock::time_point> until;
            if (!batch.Empty()) {
                until = next_post;
            }
            if (const auto expiry = batch.NextExpiry()) {
                until = until ? std::min(*until, *expiry) : *expiry;
            }
            if (seen_close) {
                until =
                    until ? std::min(*until, close_deadline) : close_deadline;
            }
            const auto ready = [&] {
                return !intake.empty() || (closed && !seen_close);
            };
            if (until) {
                wake.wait_until(lock, *until, ready);
            } else {
                wake.wait(lock, ready);
            }
            items.swap(intake);
            lost = std::exchange(dropped, 0);
            if (closed && !seen_close) {
                seen_close = true;
                close_deadline = Clock::now() + options.close_timeout;
                if (gap_only) {
                    next_post = {};
                }
            }
        }
        const auto now = Clock::now();
        batch.NoteDropped(lost);
        for (auto& item : items) {
            try {
                batch.Add(RedactAlert(item.text, options.secrets), item.dedup,
                          now);
            } catch (...) {
                // Nothing unredacted may go, and the worker may not die.
                batch.NoteDropped(1);
            }
        }
        batch.Expire(now);
        if (seen_close) {
            batch.FlushSummaries();
            if (batch.Empty()) {
                return;
            }
            // Out of time, or waiting on a retry that would come too late.
            if (now >= close_deadline || next_post >= close_deadline) {
                Unsent(batch.Size());
                return;
            }
        }
        if (batch.Empty() || now < next_post) {
            continue;
        }
        auto lines = batch.Take(Budget());
        const auto posted = Post(lines);
        const auto after = Clock::now();
        if (posted) {
            if (failing) {
                failing = false;
                spdlog::info("alerts: posting to Discord again");
            }
            backoff = std::chrono::milliseconds::zero();
            gap_only = true;
            next_post = after + Gap(seen_close);
            continue;
        }
        const types::Error& error = posted.error();
        if (error.category == types::ErrorCategory::kAuth ||
            error.category == types::ErrorCategory::kNotFound) {
            // The webhook was deleted or its token is wrong: retrying cannot
            // help, and the rest of the run would only fail the same way.
            std::size_t unsent = lines.size() + batch.Size();
            {
                std::lock_guard lock(mutex);
                closed = true;
                unsent += intake.size() + dropped;
                intake.clear();
            }
            spdlog::warn(
                "alerts: Discord refused the webhook ({}); no more alerts this "
                "run ({} line(s) not sent)",
                error.ToString(), unsent);
            return;
        }
        if (error.category == types::ErrorCategory::kRejected &&
            error.http_status && *error.http_status >= 400 &&
            *error.http_status < 500) {
            // Discord refused this message itself (a 400: too long, say):
            // the same post would fail forever and hold back every line
            // behind it, so it is dropped and the next is tried.
            spdlog::warn(
                "alerts: Discord rejected a message ({}); {} line(s) dropped",
                error.ToString(), lines.size());
            gap_only = true;
            next_post = after + Gap(seen_close);
            continue;
        }
        batch.PutBack(std::move(lines));
        std::chrono::milliseconds wait;
        if (error.category == types::ErrorCategory::kRateLimit) {
            wait = error.retry_after.value_or(std::chrono::seconds(5));
        } else {
            backoff = backoff == std::chrono::milliseconds::zero()
                          ? std::chrono::milliseconds(std::chrono::seconds(5))
                          : std::min<std::chrono::milliseconds>(
                                backoff * 2, std::chrono::seconds(60));
            wait = backoff;
        }
        if (!failing) {
            failing = true;
            spdlog::warn("alerts: could not post to Discord ({}); retrying",
                         error.ToString());
        }
        gap_only = false;
        next_post = after + wait;
        if (seen_close && next_post >= close_deadline) {
            Unsent(batch.Size());
            return;
        }
    }
}

namespace {

class AlertSink final : public spdlog::sinks::base_sink<std::mutex> {
   public:
    explicit AlertSink(std::shared_ptr<Alerter::Core> core)
        : mCore(std::move(core)) {}

   protected:
    // Must not log (spdlog would come back here, onto a mutex it holds) and
    // must not throw.
    void sink_it_(const spdlog::details::log_msg& msg) override {
        if (tOnAlertWorker) {
            return;
        }
        try {
            const auto level = spdlog::level::to_string_view(msg.level);
            const auto when =
                std::chrono::floor<std::chrono::seconds>(msg.time);
            mCore->Queue(
                std::format(
                    "{:%H:%M:%S}Z {}: {}", when,
                    std::string_view(level.data(), level.size()),
                    std::string_view(msg.payload.data(), msg.payload.size())),
                true);
        } catch (...) {
        }
    }
    void flush_() override {}

   private:
    std::shared_ptr<Alerter::Core> mCore;
};

}  // namespace

Alerter::Alerter(std::shared_ptr<HttpClient> http, std::string webhook_url,
                 AlerterOptions options)
    : mCore(std::make_shared<Core>()) {
    std::erase_if(options.secrets,
                  [](const std::string& secret) { return secret.empty(); });
    options.secrets.push_back(webhook_url);
    // And its parts, should a line quote the token or <id>/<token> alone.
    if (const auto at = webhook_url.find("/webhooks/");
        at != std::string::npos) {
        const std::string path = webhook_url.substr(at + 10);
        options.secrets.push_back(path);
        if (const auto slash = path.find('/'); slash != std::string::npos) {
            options.secrets.push_back(path.substr(slash + 1));
        }
    }
    mCore->http = std::move(http);
    mCore->url = std::move(webhook_url);
    mCore->options = std::move(options);
    mWorker = std::thread([core = mCore] { core->Run(); });
}

Alerter::~Alerter() { Close(); }

std::shared_ptr<spdlog::sinks::sink> Alerter::Sink() {
    return std::make_shared<AlertSink>(mCore);
}

void Alerter::Notify(std::string_view text) noexcept {
    try {
        mCore->Queue(std::string(text), false);
    } catch (...) {
    }
}

void Alerter::Close() {
    {
        std::lock_guard lock(mCore->mutex);
        mCore->closed = true;
    }
    mCore->wake.notify_one();
    if (mWorker.joinable()) {
        mWorker.join();
        // The sink stays on the logger to the end of the process, holding
        // the Core: release the HTTP client (its curl handles) and the
        // secrets now, while main() runs, not in static destruction.
        mCore->http.reset();
        mCore->url.clear();
        mCore->options.secrets.clear();
    }
}

types::Result<void> Alerter::SendNow(std::string_view text) {
    if (!mCore->http) {
        return types::MakeError(types::ErrorCategory::kInvalidArgument,
                                "alerts: closed");
    }
    AlertBatch one(std::chrono::milliseconds::zero());
    one.Add(RedactAlert(text, mCore->options.secrets), false, Clock::now());
    return mCore->Post(one.Take(mCore->Budget()));
}

}  // namespace kestrel::net
