#include <spdlog/spdlog.h>

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
        auto runner = CreateRunner(mName, config);
        if (!runner) {
            spdlog::error("run: {}", runner.error().ToString());
            alerts.Stopped("could not start");
            return 1;
        }
        spdlog::info("running {} mode={}", mName,
                     config::ToString(config.GetMode()));
        alerts.Started("running " + mName);
        if (!(*runner)->RiskGate()) {
            spdlog::warn("no [risk] limits: the strategy trades unchecked");
        }
        StopSignalWatcher watcher([&runner] { (*runner)->Stop(); });
        if (auto ran = (*runner)->Run(); !ran) {
            spdlog::error("run: {}", ran.error().ToString());
            alerts.Stopped("stopped on an error");
            return 1;
        }
        spdlog::info("stopped");
        alerts.Stopped("stopped");
        net::LogLatency(net::HttpLatency().Summarize(false));
        return 0;
    }

   private:
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
