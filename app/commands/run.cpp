#include <pthread.h>
#include <signal.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <cstdlib>
#include <thread>

#include "command.hpp"
#include "common.hpp"
#include "kestrel/factory.hpp"
#include "kestrel/strategy/runner.hpp"

namespace kestrel::app {

namespace {

// Ctrl-C / SIGTERM stop the runner. The signals are blocked in every thread
// (call BlockStopSignals before any thread starts, so they inherit the mask)
// and a watch thread collects them with sigtimedwait, because
// Runner::Stop() takes a mutex and so cannot be called from a signal handler.
// A second signal gives up on a clean stop.
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

class StopWatcher {
   public:
    explicit StopWatcher(strategy::Runner& runner)
        : mThread([this, &runner] {
              const sigset_t set = StopSignals();
              bool stopping = false;
              while (!mDone) {
                  const timespec timeout{0, 200'000'000};
                  if (sigtimedwait(&set, nullptr, &timeout) < 0) {
                      continue;
                  }
                  if (stopping) {
                      spdlog::error("second signal: exiting immediately");
                      std::_Exit(130);
                  }
                  stopping = true;
                  spdlog::info("stopping (signal again to force exit)");
                  runner.Stop();
              }
          }) {}
    ~StopWatcher() {
        mDone = true;
        mThread.join();
    }

   private:
    std::atomic<bool> mDone{false};
    std::thread mThread;
};

class RunCommand : public Command {
   public:
    explicit RunCommand(std::string strategy_name)
        : mName(std::move(strategy_name)) {}

    int Run(const config::Configuration& config) override {
        BlockStopSignals();
        auto runner = CreateRunner(mName, config);
        if (!runner) {
            spdlog::error("run: {}", runner.error().ToString());
            return 1;
        }
        spdlog::info("running {} mode={}", mName,
                     config::ToString(config.GetMode()));
        if (!(*runner)->RiskGate()) {
            spdlog::warn("no [risk] limits: the strategy trades unchecked");
        }
        StopWatcher watcher(**runner);
        if (auto ran = (*runner)->Run(); !ran) {
            spdlog::error("run: {}", ran.error().ToString());
            return 1;
        }
        spdlog::info("stopped");
        return 0;
    }

   private:
    std::string mName;
};

}  // namespace

void AddRunCommand(CLI::App& app, CommandSlot& slot) {
    auto* sub = AddSubcommand(
        app, "run",
        "Run the named strategy (sma_crossover or market_maker) on live data with the "
        "[strategy] parameters, trading through the paper or live venue and "
        "the [risk] limits until Ctrl-C or SIGTERM. Live mode requires [risk]");
    auto name = std::make_shared<std::string>();
    sub->add_option("strategy", *name, "Strategy name")->required();
    sub->callback(
        [&slot, name] { slot = std::make_unique<RunCommand>(*name); });
}

}  // namespace kestrel::app
