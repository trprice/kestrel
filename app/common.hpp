#pragma once

// Pieces shared by the kestrel_app commands.

#include <CLI/CLI.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "kestrel/config/configuration.hpp"
#include "kestrel/db/database.hpp"
#include "kestrel/types/decimal.hpp"
#include "kestrel/types/market.hpp"
#include "kestrel/types/time.hpp"

namespace kestrel::app {

// Adds a subcommand that also accepts the global options (--config, -v) after
// its own arguments.
CLI::App* AddSubcommand(CLI::App& app, const std::string& name,
                        const std::string& description);

// CLI11 validators, so a bad argument is a usage error before any command runs.
// Timeframe names are checked with ParseTimeframe; `require_fixed` also
// refuses calendar timeframes (1M), which have no fixed length.
CLI::Validator TimeframeValidator(bool require_fixed);
// "YYYY-MM-DD" or an ISO 8601 UTC time; see types::ParseTimestamp.
CLI::Validator DateValidator();

// "-" for a missing quote field.
std::string Opt(const std::optional<types::Decimal>& d);

// "<open time> O=.. H=.. L=.. C=.. V=.."; callers add their own prefix.
std::string FormatBar(const types::Bar& bar);

// Connects to the [database] and brings the schema up to date. kNotFound when
// the configuration has no [database] section.
types::Result<std::shared_ptr<db::Database>> OpenDatabase(
    const config::Configuration& config);

// Ctrl-C and SIGTERM, for a command to stop cleanly on. They are blocked
// in every thread (call BlockStopSignals before any thread starts, so each
// inherits the mask) and collected with sigtimedwait, since what a stop
// does (a mutex, a flush) cannot be done in a signal handler.
sigset_t StopSignals();
void BlockStopSignals();
// Ignore SIGPIPE so a command writing to a closed stdout/stderr pipe
// (such as `kestrel_app ... | mask > log` when the filter stops on
// SIGTERM) flushes and sends its stop alerts instead of exiting on a signal.
void IgnoreSigpipe();
// Collects the stop signals on a thread of its own, from construction on:
// the first asks for a stop (`on_stop` runs on the watcher thread, if given,
// so it must return quickly, and Wait returns), a second exits at once
// (128 + signal), so a command stuck starting up or shutting down can still
// be ended (without destructors, so alerts still queued are not sent). Once
// destroyed, the signals remain blocked while outer guards (such as
// ~AlertSession's bounded drain) finish. The signals must already be blocked
// (BlockStopSignals).
class StopSignalWatcher {
   public:
    explicit StopSignalWatcher(std::function<void()> on_stop = {});
    ~StopSignalWatcher();
    StopSignalWatcher(const StopSignalWatcher&) = delete;
    StopSignalWatcher& operator=(const StopSignalWatcher&) = delete;

    // Whether a stop signal has already arrived.
    bool Stopping() const;
    // Until `deadline` or a stop signal (also one that came earlier),
    // whichever is first; true on the signal.
    bool Wait(std::chrono::steady_clock::time_point deadline);

   private:
    mutable std::mutex mMutex;
    std::condition_variable mSignalled;
    bool mStopping = false;
    std::atomic<bool> mDone{false};
    std::thread mThread;
};

}  // namespace kestrel::app
