#include "kestrel/net/alerts.hpp"

#include <gtest/gtest.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "kestrel/util/notice.hpp"

namespace {

using namespace kestrel;
using namespace std::chrono_literals;
using net::AlertBatch;

constexpr const char* kWebhook =
    "https://discord.com/api/webhooks/123456/abc-DEF_ghi";

// --- the pure parts ---------------------------------------------------------

// A notice is logged, and handed to the handler while one is set.
TEST(Alerts, NoticesGoToTheHandlerWhileOneIsSet) {
    std::vector<std::string> seen;
    struct Guard {
        ~Guard() { util::SetNoticeHandler({}); }
    } guard;
    util::SetNoticeHandler(
        [&seen](std::string_view text) { seen.emplace_back(text); });
    util::Notice("risk: trading halted: test");
    util::SetNoticeHandler({});
    util::Notice("not handed on");
    EXPECT_EQ(seen, std::vector<std::string>{"risk: trading halted: test"});
}

TEST(Alerts, RecognisesDiscordWebhookUrls) {
    EXPECT_TRUE(net::IsDiscordWebhookUrl(kWebhook));
    EXPECT_TRUE(net::IsDiscordWebhookUrl(
        "https://canary.discord.com/api/v10/webhooks/1/tok"));
    EXPECT_TRUE(
        net::IsDiscordWebhookUrl("https://ptb.discord.com/api/webhooks/1/t"));
    EXPECT_FALSE(net::IsDiscordWebhookUrl(
        "http://discord.com/api/webhooks/123456/abc"));  // not https
    EXPECT_FALSE(net::IsDiscordWebhookUrl(
        "https://discordapp.com/api/webhooks/123456/abc"));  // redirects
    EXPECT_FALSE(net::IsDiscordWebhookUrl(
        "https://discord.com.evil.example/api/webhooks/1/abc"));
    EXPECT_FALSE(net::IsDiscordWebhookUrl(
        "https://discord.com/api/webhooks/abc/def"));  // id not a number
    EXPECT_FALSE(net::IsDiscordWebhookUrl(
        "https://discord.com/api/webhooks/1/abc?wait=true"));
    EXPECT_FALSE(net::IsDiscordWebhookUrl(""));
    EXPECT_FALSE(net::IsDiscordWebhookUrl(
        "https://evil.discord.com/api/webhooks/1/abc"));
    EXPECT_FALSE(net::IsDiscordWebhookUrl(
        "https://canary.ptb.discord.com/api/webhooks/1/abc"));
    EXPECT_FALSE(
        net::IsDiscordWebhookUrl("https://discord.com/api/v/webhooks/1/abc"));
    EXPECT_FALSE(
        net::IsDiscordWebhookUrl("https://discord.com/api/webhooks/1/"));
    EXPECT_FALSE(
        net::IsDiscordWebhookUrl("https://discord.com/api/webhooks//abc"));
    EXPECT_FALSE(net::IsDiscordWebhookUrl(
        "https://discord.com/api/webhooks/1/abc/extra"));
}

TEST(Alerts, AmountsNeedTheWholeWordAndANumber) {
    EXPECT_EQ(net::RedactAlert("noequity 12", {}), "noequity 12");
    EXPECT_EQ(net::RedactAlert("the balance is low at 12", {}),
              "the balance is low at 12");
    EXPECT_EQ(net::RedactAlert("balance of 5. done", {}),
              "balance of <amount>. done");
    EXPECT_EQ(net::RedactAlert("equity 1 and equity 2", {}),
              "equity <amount> and equity <amount>");
    EXPECT_EQ(net::RedactAlert("ends in equity ", {}), "ends in equity ");
}

TEST(Alerts, RedactsSecretsHashesAndBalances) {
    const std::vector<std::string> secrets{"s3cr3t+key/=", kWebhook};
    EXPECT_EQ(net::RedactAlert("auth failed with s3cr3t+key/= twice", secrets),
              "auth failed with <secret> twice");
    EXPECT_EQ(net::RedactAlert(std::string("posting to ") + kWebhook, secrets),
              "posting to <secret>");
    const std::string hash(64, 'A');
    EXPECT_EQ(net::RedactAlert("GET accounts/" + hash + "/orders", {}),
              "GET accounts/<redacted>/orders");
    EXPECT_EQ(net::RedactAlert(
                  "the ZEC balance of 12.5 cannot say whether it filled", {}),
              "the ZEC balance of <amount> cannot say whether it filled");
    EXPECT_EQ(net::RedactAlert("but the BTC balance is 0.0021; settled", {}),
              "but the BTC balance is <amount>; settled");
    EXPECT_EQ(net::RedactAlert("and the BTC balance is still -3", {}),
              "and the BTC balance is still <amount>");
    EXPECT_EQ(net::RedactAlert("risk: equity 1,234.56 is not above zero", {}),
              "risk: equity <amount> is not above zero");
    EXPECT_EQ(net::RedactAlert("over 0.5 of equity 980.1", {}),
              "over 0.5 of equity <amount>");
}

TEST(Alerts, RedactionKeepsPricesQuantitiesAndOrderIds) {
    const std::string line =
        "armed: long 0.00035 on a stop at 81195.77 (order PAPER-O000003); "
        "risk: max_daily_loss 15 reached, P&L -15.20, HTTP 429";
    EXPECT_EQ(net::RedactAlert(line, {}), line);
}

TEST(Alerts, DedupKeyIgnoresDigits) {
    EXPECT_EQ(net::AlertDedupKey("10:11:09Z error: order kst12 failed in 3 s"),
              "#:#:#Z error: order kst# failed in # s");
    EXPECT_EQ(net::AlertDedupKey("10:11:09Z error: order kst12 failed"),
              net::AlertDedupKey("23:59:00Z error: order kst9981 failed"));
    EXPECT_NE(net::AlertDedupKey("error: BTC/USDT failed"),
              net::AlertDedupKey("error: ETH/USDT failed"));
}

TEST(Alerts, TruncatesOnACharacterBoundary) {
    const std::string text =
        "ab\xC3\xA9"
        "cd";  // "abécd": é is two bytes
    EXPECT_EQ(net::TruncateUtf8(text, 10), text);
    EXPECT_EQ(net::TruncateUtf8(text, 4), "ab\xC3\xA9");
    EXPECT_EQ(net::TruncateUtf8(text, 3), "ab");  // not half of é
    EXPECT_EQ(net::TruncateUtf8(text, 0), "");
}

TEST(Alerts, MessageIsAHeaderAndACodeBlock) {
    EXPECT_EQ(net::FormatAlertMessage("trend-paper (trend_ensemble, paper)",
                                      {"a", "b"}),
              "**trend-paper (trend\\_ensemble, paper)**\n```\na\nb\n```");
    EXPECT_EQ(net::FormatAlertMessage("x", {}), "**x**");
    // A ``` in a line cannot close the block.
    const std::string message = net::FormatAlertMessage("x", {"a ``` b"});
    EXPECT_EQ(message.find("```", 8), message.size() - 3);
}

TEST(Alerts, WebhookBodyTurnsMentionsOffAndSurvivesBadUtf8) {
    const auto body =
        nlohmann::json::parse(net::DiscordWebhookBody("@everyone hi"));
    EXPECT_EQ(body["content"], "@everyone hi");
    EXPECT_TRUE(body["allowed_mentions"]["parse"].is_array());
    EXPECT_TRUE(body["allowed_mentions"]["parse"].empty());
    // A cut in the middle of a character, as an error's body excerpt can
    // be, is replaced rather than thrown on.
    EXPECT_NO_THROW(net::DiscordWebhookBody("bad \xC3"));
}

// --- AlertBatch -------------------------------------------------------------

AlertBatch::Time At(int seconds) {
    return AlertBatch::Time{} + std::chrono::seconds(seconds);
}

TEST(AlertBatch, SendsTheFirstOfALineAndSummarisesItsRepeats) {
    AlertBatch batch(5min);
    batch.Add("10:00:00Z error: order kst1 failed", true, At(0));
    batch.Add("10:00:30Z error: order kst2 failed", true, At(30));
    batch.Add("10:01:00Z error: order kst3 failed", true, At(60));
    batch.Add("10:01:00Z error: something else", true, At(60));
    EXPECT_EQ(batch.Take(1000),
              (std::vector<std::string>{"10:00:00Z error: order kst1 failed",
                                        "10:01:00Z error: something else"}));
    EXPECT_TRUE(batch.Empty());
    ASSERT_TRUE(batch.NextExpiry());
    EXPECT_EQ(*batch.NextExpiry(), At(300));

    batch.Expire(At(299));
    EXPECT_TRUE(batch.Empty());
    batch.Expire(At(300));
    EXPECT_EQ(batch.Take(1000),
              (std::vector<std::string>{
                  "(2 more like this in 5 min; the last: 10:01:00Z error: "
                  "order kst3 failed)"}));
    EXPECT_FALSE(batch.NextExpiry());

    // The window has ended: the next one is sent again.
    batch.Add("10:06:00Z error: order kst4 failed", true, At(360));
    EXPECT_EQ(batch.Take(1000).size(), 1u);
}

TEST(AlertBatch, NoticesAreNotDeduplicated) {
    AlertBatch batch(5min);
    batch.Add("started", false, At(0));
    batch.Add("started", false, At(1));
    EXPECT_EQ(batch.Take(1000).size(), 2u);
}

TEST(AlertBatch, FlushSummarisesOpenWindows) {
    AlertBatch batch(5min);
    batch.Add("e 1", true, At(0));
    batch.Add("e 2", true, At(1));
    batch.Take(1000);
    batch.FlushSummaries();
    EXPECT_EQ(batch.Take(1000),
              (std::vector<std::string>{
                  "(1 more like this in 5 min; the last: e 2)"}));
    EXPECT_FALSE(batch.NextExpiry());
}

TEST(AlertBatch, TakesWhatFitsAndKeepsTheRest) {
    AlertBatch batch(5min);
    batch.Add("aaaa", false, At(0));
    batch.Add("bbbb", false, At(0));
    batch.Add("cccc", false, At(0));
    // "aaaa\nbbbb" is 9 bytes.
    EXPECT_EQ(batch.Take(9), (std::vector<std::string>{"aaaa", "bbbb"}));
    EXPECT_EQ(batch.Take(9), (std::vector<std::string>{"cccc"}));
    EXPECT_TRUE(batch.Empty());

    batch.Add(std::string(50, 'x'), false, At(0));
    const auto cut = batch.Take(10);
    ASSERT_EQ(cut.size(), 1u);
    EXPECT_EQ(cut[0], "xxxxxxx...");
    EXPECT_TRUE(batch.Empty());
}

TEST(AlertBatch, PutBackGoesFirst) {
    AlertBatch batch(5min);
    batch.Add("a", false, At(0));
    batch.Add("b", false, At(0));
    auto first = batch.Take(1);
    batch.Add("c", false, At(0));
    batch.PutBack(first);
    EXPECT_EQ(batch.Take(100), (std::vector<std::string>{"a", "b", "c"}));
}

TEST(AlertBatch, CountsWhatItDrops) {
    AlertBatch batch(5min, 2);
    batch.Add("a", false, At(0));
    batch.Add("b", false, At(0));
    batch.Add("c", false, At(0));
    batch.NoteDropped(4);
    EXPECT_EQ(batch.Take(1000),
              (std::vector<std::string>{"(5 lines dropped: too many at once)",
                                        "a", "b"}));
    EXPECT_TRUE(batch.Empty());
}

TEST(AlertBatch, KeepsAtMostMaxKeys) {
    AlertBatch batch(5min, 200, 2);
    batch.Add("a", true, At(0));
    batch.Add("a", true, At(1));  // a repeat, summarised when a is evicted
    batch.Add("b", true, At(2));
    batch.Add("c", true, At(3));  // evicts a, the oldest
    EXPECT_EQ(batch.Take(1000),
              (std::vector<std::string>{
                  "a", "b", "(1 more like this in 5 min; the last: a)", "c"}));
}

// --- Alerter ----------------------------------------------------------------

// Records each post; answers each with the next of `failures` (nullopt or
// none left: a 204). Can be held shut, to keep the worker inside a post.
class FakeWebhook : public net::HttpClient {
   public:
    types::Result<net::HttpResponse> Send(
        const net::HttpRequest& request) override {
        std::function<void()> after;
        {
            std::unique_lock lock(mMutex);
            ++mEntered;
            mChanged.notify_all();
            mChanged.wait(lock, [this] { return !mHeld; });
            requests.push_back(request);
            mChanged.notify_all();
            if (!failures.empty()) {
                auto failure = failures.front();
                failures.erase(failures.begin());
                if (failure) {
                    return std::unexpected(*failure);
                }
            }
            after = on_send;
        }
        if (after) {
            after();
        }
        return net::HttpResponse{204, "", {}};
    }

    void Hold() {
        std::lock_guard lock(mMutex);
        mHeld = true;
    }
    void Release() {
        {
            std::lock_guard lock(mMutex);
            mHeld = false;
        }
        mChanged.notify_all();
    }
    // Until `count` posts have begun (held or not).
    bool WaitForEntered(std::size_t count) {
        std::unique_lock lock(mMutex);
        return mChanged.wait_for(lock, 5s, [&] { return mEntered >= count; });
    }
    bool WaitForPosts(std::size_t count) {
        std::unique_lock lock(mMutex);
        return mChanged.wait_for(lock, 5s,
                                 [&] { return requests.size() >= count; });
    }
    std::size_t Count() {
        std::lock_guard lock(mMutex);
        return requests.size();
    }
    std::string Content(std::size_t i) {
        std::lock_guard lock(mMutex);
        return nlohmann::json::parse(requests.at(i).body)["content"];
    }

    std::vector<std::optional<types::Error>> failures;
    std::vector<net::HttpRequest> requests;
    std::function<void()> on_send;

   private:
    std::mutex mMutex;
    std::condition_variable mChanged;
    bool mHeld = false;
    std::size_t mEntered = 0;
};

types::Error Fail(
    types::ErrorCategory category,
    std::optional<std::chrono::milliseconds> retry_after = std::nullopt) {
    types::Error error;
    error.category = category;
    error.message = "refused";
    error.retry_after = retry_after;
    return error;
}

net::AlerterOptions Fast() {
    net::AlerterOptions options;
    options.header = "test (paper)";
    options.min_gap = 0ms;
    return options;
}

std::shared_ptr<spdlog::logger> LoggerFor(net::Alerter& alerter) {
    auto sink = alerter.Sink();
    sink->set_level(spdlog::level::err);
    auto logger = std::make_shared<spdlog::logger>("alerts-test", sink);
    logger->set_level(spdlog::level::trace);
    return logger;
}

TEST(Alerter, PostsErrorLinesToTheWebhook) {
    auto http = std::make_shared<FakeWebhook>();
    net::Alerter alerter(http, kWebhook, Fast());
    auto logger = LoggerFor(alerter);
    logger->info("not sent");
    logger->error("order {} failed", "kst1");
    ASSERT_TRUE(http->WaitForPosts(1));
    alerter.Close();
    ASSERT_EQ(http->Count(), 1u);
    const auto& request = http->requests[0];
    EXPECT_EQ(request.method, net::HttpMethod::kPost);
    EXPECT_EQ(request.url, kWebhook);
    EXPECT_EQ(request.label, "discord webhook");
    EXPECT_EQ(request.headers.at("Content-Type"), "application/json");
    const std::string content = http->Content(0);
    EXPECT_EQ(content.rfind("**test (paper)**\n```\n", 0), 0u) << content;
    EXPECT_NE(content.find("Z error: order kst1 failed"), std::string::npos);
    EXPECT_EQ(content.find("not sent"), std::string::npos);
}

TEST(Alerter, BatchesABurstAndSummarisesRepeatsOnClose) {
    auto http = std::make_shared<FakeWebhook>();
    http->Hold();
    auto options = Fast();
    options.min_gap = 1h;  // one post, then nothing until Close
    net::Alerter alerter(http, kWebhook, options);
    auto logger = LoggerFor(alerter);
    logger->error("first");
    ASSERT_TRUE(http->WaitForEntered(1));  // and held there
    logger->error("halted: max_drawdown_close");
    logger->error("cancel 1 failed");
    logger->error("cancel 2 failed");
    http->Release();
    ASSERT_TRUE(http->WaitForPosts(1));
    alerter.Close();
    std::string all;
    for (std::size_t i = 0; i < http->Count(); ++i) {
        all += http->Content(i) + "\n";
    }
    EXPECT_NE(all.find("first"), std::string::npos) << all;
    EXPECT_NE(all.find("halted: max_drawdown_close"), std::string::npos);
    EXPECT_NE(all.find("cancel 1 failed"), std::string::npos);
    EXPECT_EQ(all.find("cancel 2 failed\n"), std::string::npos)
        << "the repeat is summarised, not sent: " << all;
    EXPECT_NE(all.find("(1 more like this in 5 min; the last: "),
              std::string::npos)
        << all;
    EXPECT_LE(http->Count(), 2u);
}

TEST(Alerter, NoticesGoOutAsTheyAre) {
    auto http = std::make_shared<FakeWebhook>();
    net::Alerter alerter(http, kWebhook, Fast());
    alerter.Notify("started: running breakout");
    alerter.Close();
    ASSERT_EQ(http->Count(), 1u);
    EXPECT_NE(http->Content(0).find("\nstarted: running breakout\n"),
              std::string::npos);
}

TEST(Alerter, MasksTheWebhookAndSecretsInWhatItPosts) {
    auto http = std::make_shared<FakeWebhook>();
    auto options = Fast();
    options.secrets = {"hunter2hunter2", ""};
    net::Alerter alerter(http, kWebhook, options);
    auto logger = LoggerFor(alerter);
    logger->error("posting to {} with key hunter2hunter2", kWebhook);
    alerter.Close();
    ASSERT_EQ(http->Count(), 1u);
    const std::string content = http->Content(0);
    EXPECT_EQ(content.find("abc-DEF_ghi"), std::string::npos) << content;
    EXPECT_EQ(content.find("hunter2"), std::string::npos) << content;
    EXPECT_NE(content.find("posting to <secret> with key <secret>"),
              std::string::npos);
}

// What the default logger gets (the worker's own warnings), to wait on.
class WarningSink final : public spdlog::sinks::base_sink<std::mutex> {
   public:
    bool WaitFor(std::string_view text) {
        std::unique_lock lock(mMutex);
        return mChanged.wait_for(lock, 5s, [&] {
            return std::ranges::any_of(mLines, [&](const std::string& line) {
                return line.find(text) != std::string::npos;
            });
        });
    }

   protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        {
            std::lock_guard lock(mMutex);
            mLines.emplace_back(msg.payload.data(), msg.payload.size());
        }
        mChanged.notify_all();
    }
    void flush_() override {}

   private:
    std::mutex mMutex;
    std::condition_variable mChanged;
    std::vector<std::string> mLines;
};

TEST(Alerter, StopsForGoodWhenDiscordRefusesTheWebhook) {
    auto warnings = std::make_shared<WarningSink>();
    auto& sinks = spdlog::default_logger()->sinks();
    sinks.push_back(warnings);
    auto http = std::make_shared<FakeWebhook>();
    http->failures.push_back(Fail(types::ErrorCategory::kNotFound));
    net::Alerter alerter(http, kWebhook, Fast());
    auto logger = LoggerFor(alerter);
    logger->error("one");
    // Logged once the worker has stopped taking lines.
    const bool stopped = warnings->WaitFor("no more alerts this run");
    logger->error("two");
    alerter.Close();
    std::erase(sinks, warnings);
    ASSERT_TRUE(stopped);
    EXPECT_EQ(http->Count(), 1u);
}

TEST(Alerter, ClosingDuringABackoffDoesNotWaitItOut) {
    auto http = std::make_shared<FakeWebhook>();
    http->failures.push_back(Fail(types::ErrorCategory::kServer));
    auto options = Fast();
    options.close_timeout = 3s;  // the retry, 5 s off, would miss it
    net::Alerter alerter(http, kWebhook, options);
    alerter.Notify("first");
    ASSERT_TRUE(http->WaitForPosts(1));
    const auto start = std::chrono::steady_clock::now();
    alerter.Close();
    EXPECT_LT(std::chrono::steady_clock::now() - start, 1s);
    EXPECT_EQ(http->Count(), 1u);
}

TEST(Alerter, RetriesAfterARateLimit) {
    auto http = std::make_shared<FakeWebhook>();
    http->failures.push_back(Fail(types::ErrorCategory::kRateLimit, 50ms));
    net::Alerter alerter(http, kWebhook, Fast());
    auto logger = LoggerFor(alerter);
    logger->error("once");
    ASSERT_TRUE(http->WaitForPosts(2));
    alerter.Close();
    ASSERT_EQ(http->Count(), 2u);
    EXPECT_EQ(http->Content(0), http->Content(1));
}

TEST(Alerter, GivesUpOnCloseWhenTheRetryIsTooFarOff) {
    auto http = std::make_shared<FakeWebhook>();
    http->failures.push_back(Fail(types::ErrorCategory::kServer));
    auto options = Fast();
    options.close_timeout = 100ms;
    net::Alerter alerter(http, kWebhook, options);
    alerter.Notify("bye");
    const auto start = std::chrono::steady_clock::now();
    alerter.Close();  // the retry is 5 s away, past the deadline
    EXPECT_LT(std::chrono::steady_clock::now() - start, 2s);
    EXPECT_EQ(http->Count(), 1u);
}

TEST(Alerter, DropsWhatOverflowsItsQueueAndSaysSo) {
    auto http = std::make_shared<FakeWebhook>();
    http->Hold();
    auto options = Fast();
    options.queue_capacity = 3;
    net::Alerter alerter(http, kWebhook, options);
    auto logger = LoggerFor(alerter);
    logger->error("line 0");
    ASSERT_TRUE(http->WaitForEntered(1));  // and held there
    for (int i = 1; i <= 10; ++i) {
        logger->error("distinct {}", std::string(i, 'x'));
    }
    http->Release();
    alerter.Close();
    std::string all;
    for (std::size_t i = 0; i < http->Count(); ++i) {
        all += http->Content(i);
    }
    EXPECT_NE(all.find("(7 lines dropped: too many at once)"),
              std::string::npos)
        << all;
}

TEST(Alerter, WhatItsOwnThreadLogsIsNotPostedAgain) {
    auto http = std::make_shared<FakeWebhook>();
    net::Alerter alerter(http, kWebhook, Fast());
    auto logger = LoggerFor(alerter);
    // A post that logs an error, as a failing HTTP client might.
    http->on_send = [logger] { logger->error("from the worker"); };
    logger->error("from the strategy");
    ASSERT_TRUE(http->WaitForPosts(1));
    alerter.Close();
    EXPECT_EQ(http->Count(), 1u);
    // The fake holds the logger, which holds the sink, which holds the fake.
    http->on_send = nullptr;
}

TEST(Alerter, TheSinkOutlivesTheAlerter) {
    auto http = std::make_shared<FakeWebhook>();
    std::shared_ptr<spdlog::logger> logger;
    {
        net::Alerter alerter(http, kWebhook, Fast());
        logger = LoggerFor(alerter);
    }
    EXPECT_NO_THROW(logger->error("after close"));
    EXPECT_EQ(http->Count(), 0u);
}

TEST(Alerter, SendNowPostsOnTheCallingThread) {
    auto http = std::make_shared<FakeWebhook>();
    net::Alerter alerter(http, kWebhook, Fast());
    ASSERT_TRUE(alerter.SendNow("hello"));
    EXPECT_EQ(http->Count(), 1u);
    EXPECT_EQ(http->Content(0), "**test (paper)**\n```\nhello\n```");
    http->failures.push_back(Fail(types::ErrorCategory::kAuth));
    EXPECT_FALSE(alerter.SendNow("again"));
}

TEST(Alerts, TokensAreFoundInALineOfAnyLength) {
    const std::string kept(31, 'a');
    EXPECT_EQ(net::RedactAlert("x " + kept + " y", {}), "x " + kept + " y");
    EXPECT_EQ(net::RedactAlert(std::string(32, 'a') + "-" + kept, {}),
              "<redacted>-" + kept);
    // A dumped body: long enough to overflow a recursive regex.
    const auto out = net::RedactAlert("body " + std::string(1'000'000, 'Z') +
                                          " end " + std::string(10'000, '.'),
                                      {});
    EXPECT_EQ(out.rfind("body <redacted> end ", 0), 0u);
    EXPECT_LE(out.size(), 4096u);
}

TEST(Alerter, MasksTheWebhookTokenOnItsOwn) {
    auto http = std::make_shared<FakeWebhook>();
    net::Alerter alerter(http, kWebhook, Fast());
    alerter.Notify("token abc-DEF_ghi, path 123456/abc-DEF_ghi");
    alerter.Close();
    ASSERT_EQ(http->Count(), 1u);
    const auto content = http->Content(0);
    EXPECT_EQ(content.find("abc-DEF_ghi"), std::string::npos) << content;
    EXPECT_NE(content.find("token <secret>, path <secret>"), std::string::npos)
        << content;
}

TEST(Alerts, LongerSecretsAreMaskedFirst) {
    // "abc12" is inside "abc12xyz9": masked first, it would leave "xyz9".
    EXPECT_EQ(net::RedactAlert("key abc12xyz9 here", {"abc12", "abc12xyz9"}),
              "key <secret> here");
}

TEST(Alerts, AmountsAreMaskedInAnyCaseAndWithADollarSign) {
    EXPECT_EQ(net::RedactAlert("order failed: Balance is $500.00", {}),
              "order failed: Balance is <amount>");
    EXPECT_EQ(net::RedactAlert("cash balance of $1,234.56 is too low", {}),
              "cash balance of <amount> is too low");
    EXPECT_EQ(net::RedactAlert("Equity -$12.5", {}), "Equity <amount>");
}

TEST(Alerts, TheHeaderStaysOnOneLine) {
    EXPECT_EQ(net::FormatAlertMessage("a\nb\r\tc", {}), "**a b  c**");
}

TEST(AlertBatch, NoKeysAtAllIsTakenAsOne) {
    net::AlertBatch batch(5min, 200, 0);
    const auto now = net::AlertBatch::Time{};
    batch.Add("a", true, now);
    batch.Add("b", true, now);
    const auto lines = batch.Take(1000);
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[0], "a");
    EXPECT_EQ(lines[1], "b");
}

TEST(Alerter, DropsAMessageDiscordRejectsAndGoesOn) {
    auto http = std::make_shared<FakeWebhook>();
    auto rejected = Fail(types::ErrorCategory::kRejected);
    rejected.http_status = 400;
    http->failures.push_back(rejected);
    net::Alerter alerter(http, kWebhook, Fast());
    auto logger = LoggerFor(alerter);
    logger->error("first");
    ASSERT_TRUE(http->WaitForPosts(1));
    logger->error("second");
    ASSERT_TRUE(http->WaitForPosts(2));
    alerter.Close();
    ASSERT_EQ(http->Count(), 2u);
    EXPECT_NE(http->Content(1).find("second"), std::string::npos);
    EXPECT_EQ(http->Content(1).find("first"), std::string::npos);
}

TEST(Alerter, SendNowAfterCloseFails) {
    auto http = std::make_shared<FakeWebhook>();
    net::Alerter alerter(http, kWebhook, Fast());
    alerter.Close();
    EXPECT_FALSE(alerter.SendNow("late").has_value());
    EXPECT_EQ(http->Count(), 0u);
}

TEST(HttpError, RetryAfterKeepsItsFraction) {
    const auto after = [](std::string value) {
        return net::ErrorFromHttpStatus({429, "", {{"Retry-After", value}}})
            .retry_after;
    };
    EXPECT_EQ(after("1.45"), std::chrono::milliseconds(1450));
    EXPECT_EQ(after("0.0001"), std::chrono::milliseconds(1));
    EXPECT_EQ(after("60"), std::chrono::milliseconds(60'000));
    EXPECT_EQ(after("0"), std::nullopt);
    EXPECT_EQ(after("soon"), std::nullopt);
    EXPECT_EQ(after("1e9"), std::chrono::milliseconds(86'400'000));
}

}  // namespace
