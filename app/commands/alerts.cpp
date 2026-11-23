#include "alerts.hpp"

#include <spdlog/spdlog.h>

#include <iostream>

#include "command.hpp"
#include "common.hpp"
#include "kestrel/net/http_client.hpp"
#include "kestrel/util/notice.hpp"

namespace kestrel::app {

namespace {

// Every credential the configuration holds, so none can reach Discord even
// if a log line quotes one.
std::vector<std::string> Credentials(const config::Configuration& config) {
    std::vector<std::string> secrets;
    if (const auto& database = config.GetDatabase()) {
        secrets.push_back(database->password);
    }
    if (const auto* kraken =
            dynamic_cast<const config::KrakenConfiguration*>(&config);
        kraken && kraken->Credentials()) {
        secrets.push_back(kraken->Credentials()->api_key);
        secrets.push_back(kraken->Credentials()->api_secret);
    }
    if (const auto* binance =
            dynamic_cast<const config::BinanceUSConfiguration*>(&config);
        binance && binance->Credentials()) {
        secrets.push_back(binance->Credentials()->api_key);
        secrets.push_back(binance->Credentials()->api_secret);
    }
    if (const auto* schwab =
            dynamic_cast<const config::SchwabConfiguration*>(&config);
        schwab && schwab->Credentials()) {
        secrets.push_back(schwab->Credentials()->app_key);
        secrets.push_back(schwab->Credentials()->app_secret);
    }
    if (const auto* schwab =
            dynamic_cast<const config::SchwabConfiguration*>(&config);
        schwab && schwab->AccountNumber().size() >= 6) {
        // Kestrel masks it in its own lines, but a Schwab error body quoted
        // in a log line can carry it. Shorter would mask ordinary numbers.
        secrets.push_back(schwab->AccountNumber());
    }
    if (const auto* databento =
            dynamic_cast<const config::DatabentoConfiguration*>(&config);
        databento && databento->Credentials()) {
        secrets.push_back(databento->Credentials()->api_key);
    }
    return secrets;
}

class AlertCommand : public Command {
   public:
    explicit AlertCommand(std::string message) : mMessage(std::move(message)) {}

    int Run(const config::Configuration& config) override {
        auto alerter = MakeAlerter(config, "");
        if (!alerter) {
            spdlog::error(
                "alert: no Discord webhook: set discord_webhook_url in the "
                "secrets file's [alerts], or "
                "KESTREL_ALERTS_DISCORD_WEBHOOK_URL");
            return 1;
        }
        if (!config.GetAlerts().enabled) {
            spdlog::warn(
                "alert: [alerts] enabled = false, so commands send nothing; "
                "sending this test anyway");
        }
        if (auto sent = alerter->SendNow(mMessage); !sent) {
            spdlog::error("alert: {}", sent.error().ToString());
            return 1;
        }
        std::cout << "sent\n";
        return 0;
    }

   private:
    std::string mMessage;
};

}  // namespace

std::unique_ptr<net::Alerter> MakeAlerter(const config::Configuration& config,
                                          std::string_view strategy) {
    const auto& alerts = config.GetAlerts();
    if (!alerts.credentials) {
        return nullptr;
    }
    std::string what;
    if (!strategy.empty()) {
        what = std::string(strategy) + ", ";
    }
    what += std::string(config::ToString(config.GetMode())) + ", " +
            std::string(config::ToString(config.GetProvider()));
    net::AlerterOptions options;
    options.header =
        alerts.name.empty() ? what : alerts.name + " (" + what + ")";
    options.secrets = Credentials(config);
    return std::make_unique<net::Alerter>(
        std::make_shared<net::CprHttpClient>(std::chrono::seconds(3)),
        alerts.credentials->discord_webhook_url, std::move(options));
}

AlertSession::AlertSession(const config::Configuration& config,
                           std::string_view strategy) {
    if (!config.GetAlerts().enabled) {
        return;
    }
    mAlerter = MakeAlerter(config, strategy);
    if (!mAlerter) {
        return;
    }
    mLifecycle = config.GetAlerts().lifecycle;
    auto sink = mAlerter->Sink();
    sink->set_level(spdlog::level::err);
    // Never removed: other threads may still log while the run stops, and
    // the sink drops what it gets once the Alerter has closed.
    spdlog::default_logger()->sinks().push_back(std::move(sink));
    util::SetNoticeHandler(
        [alerter = mAlerter](std::string_view text) { alerter->Notify(text); });
    spdlog::info("alerts: error lines and notices go to Discord");
}

AlertSession::~AlertSession() { Close(); }

void AlertSession::Close() {
    if (!mAlerter) {
        return;
    }
    util::SetNoticeHandler({});
    if (!mStopped && mLifecycle) {
        mAlerter->Notify("stopped unexpectedly (see the log)");
    }
    mAlerter->Close();
    mAlerter.reset();
}

void AlertSession::Started(std::string_view what) {
    if (mAlerter && mLifecycle) {
        mAlerter->Notify("started: " + std::string(what));
    }
}

void AlertSession::Stopped(std::string_view how) {
    mStopped = true;
    if (mAlerter && mLifecycle) {
        mAlerter->Notify(how);
    }
}

void AddAlertCommand(CLI::App& app, CommandSlot& slot) {
    auto* sub = AddSubcommand(
        app, "alert",
        "Post a test message to the Discord webhook in the secrets file "
        "([alerts] discord_webhook_url), to check it before a run");
    auto message = std::make_shared<std::string>("test alert from kestrel_app");
    sub->add_option("message", *message, "What to post");
    sub->callback(
        [&slot, message] { slot = std::make_unique<AlertCommand>(*message); });
}

}  // namespace kestrel::app
