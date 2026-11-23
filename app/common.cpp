#include "common.hpp"

#include <pthread.h>
#include <signal.h>
#include <spdlog/spdlog.h>

#include <cstdlib>
#include <sstream>

#include "kestrel/types/timeframe.hpp"

namespace kestrel::app {

CLI::App* AddSubcommand(CLI::App& app, const std::string& name,
                        const std::string& description) {
    auto* sub = app.add_subcommand(name, description);
    sub->fallthrough();
    return sub;
}

CLI::Validator TimeframeValidator(bool require_fixed) {
    return CLI::Validator(
        [require_fixed](std::string& text) -> std::string {
            auto tf = types::ParseTimeframe(text);
            if (!tf) {
                return tf.error().ToString();
            }
            if (require_fixed && !types::FixedDuration(*tf)) {
                return text + " has no fixed length; pick another timeframe";
            }
            return {};
        },
        "TIMEFRAME", "timeframe");
}

CLI::Validator DateValidator() {
    return CLI::Validator(
        [](std::string& text) -> std::string {
            auto when = types::ParseTimestamp(text);
            return when ? std::string() : when.error().ToString();
        },
        "DATE", "date");
}

std::string Opt(const std::optional<types::Decimal>& d) {
    return d ? d->ToString() : "-";
}

std::string FormatBar(const types::Bar& b) {
    std::ostringstream out;
    out << types::FormatIso8601(b.open_time) << " O=" << b.open
        << " H=" << b.high << " L=" << b.low << " C=" << b.close
        << " V=" << b.volume;
    return out.str();
}

types::Result<std::shared_ptr<db::Database>> OpenDatabase(
    const config::Configuration& config) {
    const auto& options = config.GetDatabase();
    if (!options) {
        return types::MakeError(types::ErrorCategory::kNotFound,
                                "the configuration has no [database] section");
    }
    auto database = db::Database::Connect(*options);
    if (!database) {
        return std::unexpected(database.error());
    }
    std::shared_ptr<db::Database> shared = std::move(*database);
    if (auto migrated = shared->Migrate(); !migrated) {
        return std::unexpected(migrated.error());
    }
    return shared;
}

sigset_t StopSignals() {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    return set;
}

void BlockStopSignals() {
    const sigset_t set = StopSignals();
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
}

void IgnoreSigpipe() { std::signal(SIGPIPE, SIG_IGN); }

StopSignalWatcher::StopSignalWatcher(std::function<void()> on_stop)
    : mThread([this, on_stop = std::move(on_stop)] {
          const sigset_t set = StopSignals();
          bool stopping = false;
          while (!mDone) {
              const timespec timeout{0, 200'000'000};
              const int sig = sigtimedwait(&set, nullptr, &timeout);
              if (sig < 0) {
                  continue;  // timed out, or interrupted
              }
              if (stopping) {
                  spdlog::error("second signal: exiting immediately");
                  std::_Exit(128 + sig);
              }
              stopping = true;
              spdlog::info("stopping on a signal (signal again to force exit)");
              {
                  std::lock_guard lock(mMutex);
                  mStopping = true;
              }
              mSignalled.notify_all();
              if (on_stop) {
                  on_stop();
              }
          }
      }) {}

StopSignalWatcher::~StopSignalWatcher() {
    mDone = true;
    mThread.join();
}

bool StopSignalWatcher::Stopping() const {
    std::lock_guard lock(mMutex);
    return mStopping;
}

bool StopSignalWatcher::Wait(std::chrono::steady_clock::time_point deadline) {
    std::unique_lock lock(mMutex);
    return mSignalled.wait_until(lock, deadline, [this] { return mStopping; });
}

}  // namespace kestrel::app
