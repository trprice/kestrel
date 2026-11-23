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

// Alerts for one command (`run`, or `stream`/`bars` with `--store`): error
// lines from the log, util::Notice's notices (a risk-gate halt, say), and
// notices when it starts and stops ([alerts] lifecycle). Make it after
// BlockStopSignals, so the worker thread inherits the blocked signals, and
// before anything else starts a thread, since spdlog's list of sinks is not
// safe to change while another thread logs.
// Does nothing when alerts are off or not configured.
class AlertSession {
   public:
    AlertSession(const config::Configuration& config,
                 std::string_view strategy);
    // Close()s.
    ~AlertSession();
    AlertSession(const AlertSession&) = delete;
    AlertSession& operator=(const AlertSession&) = delete;

    void Started(std::string_view what);
    void Stopped(std::string_view how);
    // Posts that the command ended unexpectedly, if Stopped was never
    // called, and what is still queued (for up to 10 s); later lines and
    // notices are not sent. Call it while the StopSignalWatcher is still
    // there, so a second Ctrl-C can cut the wait short. Once is enough.
    void Close();

   private:
    // Shared with the notice handler.
    std::shared_ptr<net::Alerter> mAlerter;
    bool mLifecycle = false;
    bool mStopped = false;
};

}  // namespace kestrel::app
