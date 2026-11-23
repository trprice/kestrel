#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "kestrel/config/configuration.hpp"
#include "kestrel/net/alerts.hpp"

namespace kestrel::app {

// An Alerter for the configured Discord webhook, its messages headed with
// the [alerts] name and what is running (`strategy`, if any), and every
// credential in the configuration masked. Null when no webhook is
// configured; [alerts] enabled is the caller's to check.
std::unique_ptr<net::Alerter> MakeAlerter(const config::Configuration& config,
                                          std::string_view strategy);

// Alerts for one `run`: error lines from the log, and notices when it starts
// and stops ([alerts] lifecycle). Make it after BlockStopSignals, so the
// worker thread inherits the blocked signals, and before anything else
// starts a thread, since spdlog's list of sinks is not safe to change while
// another thread logs. Does nothing when alerts are off or not configured.
class AlertSession {
   public:
    AlertSession(const config::Configuration& config,
                 std::string_view strategy);
    // Posts "stopped" (or that the run ended unexpectedly, if Stopped was
    // never called) and what is still queued.
    ~AlertSession();
    AlertSession(const AlertSession&) = delete;
    AlertSession& operator=(const AlertSession&) = delete;

    void Started(std::string_view what);
    void Stopped(std::string_view how);

   private:
    std::unique_ptr<net::Alerter> mAlerter;
    bool mLifecycle = false;
    bool mStopped = false;
};

}  // namespace kestrel::app
