#include <spdlog/spdlog.h>

#include <mutex>

#include "alerts.hpp"
#include "command.hpp"
#include "common.hpp"
#include "kestrel/factory.hpp"
#include "kestrel/net/timed_http_client.hpp"
#include "kestrel/strategy/runner.hpp"

namespace kestrel::app {

namespace {

class RunCommand : public Command {
   public:
    explicit RunCommand(std::string strategy_name)
        : mName(std::move(strategy_name)) {}

    int Run(const config::Configuration& config) override {
        BlockStopSignals();
        IgnoreSigpipe();
        AlertSession alerts(config, mName);
        // From here, so a second signal still forces an exit while the
        // alerts post what is left (Close, below). The first stops the
        // runner once there is one.
        Running running;
        StopSignalWatcher watcher([&running] {
            std::lock_guard lock(running.mutex);
            if (running.runner) {
                running.runner->Stop();
            }
        });
        const int code = RunStrategy(config, alerts, watcher, running);
        alerts.Close();
        return code;
    }

   private:
    // The runner a stop signal stops, while there is one: the mutex keeps it
    // from going while the watcher stops it.
    struct Running {
        std::mutex mutex;
        strategy::Runner* runner = nullptr;
    };

    // Everything but the alerts' last posts; the runner is gone on return.
    int RunStrategy(const config::Configuration& config, AlertSession& alerts,
                    const StopSignalWatcher& watcher, Running& running) {
        CheckDatabaseDisk(config);
        auto runner = CreateRunner(mName, config);
        if (!runner) {
            spdlog::error("run: {}", runner.error().ToString());
            alerts.Stopped("could not start");
            return 1;
        }
        if (watcher.Stopping()) {
            alerts.Stopped("stopped early, on a signal");
            return 0;
        }
        spdlog::info("running {} mode={}", mName,
                     config::ToString(config.GetMode()));
        alerts.Started("running " + mName);
        if (!(*runner)->RiskGate()) {
            spdlog::warn("no [risk] limits: the strategy trades unchecked");
        }
        {
            std::lock_guard lock(running.mutex);
            running.runner = runner->get();
        }
        // A signal just before the runner was published above: Stop() before
        // Run() ends the run as soon as it has begun.
        if (watcher.Stopping()) {
            (*runner)->Stop();
        }
        auto ran = (*runner)->Run();
        {
            std::lock_guard lock(running.mutex);
            running.runner = nullptr;  // before the runner goes
        }
        if (!ran) {
            spdlog::error("run: {}", ran.error().ToString());
            alerts.Stopped("stopped on an error");
            return 1;
        }
        spdlog::info("stopped");
        alerts.Stopped("stopped");
        net::LogLatency(net::HttpLatency().Summarize(false));
        return 0;
    }

    std::string mName;
};

}  // namespace

void AddRunCommand(CLI::App& app, CommandSlot& slot) {
    auto* sub = AddSubcommand(
        app, "run",
        "Run the named strategy (ma_crossover, market_maker, breakout or "
        "momentum_rotation) on live data with the [strategy] parameters, "
        "trading through the paper or live venue and "
        "the [risk] limits until Ctrl-C or SIGTERM. Live mode requires [risk]");
    auto name = std::make_shared<std::string>();
    sub->add_option("strategy", *name, "Strategy name")->required();
    sub->callback(
        [&slot, name] { slot = std::make_unique<RunCommand>(*name); });
}

}  // namespace kestrel::app
