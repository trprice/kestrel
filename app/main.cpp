// kestrel_app: command-line driver for the data, trading, backtest and
// strategy-running code. Each subcommand lives in app/commands/; this file only
// parses the command line, loads the configuration and runs the command that
// was chosen.

#include <spdlog/spdlog.h>

#include <CLI/CLI.hpp>

#include "commands/command.hpp"
#include "kestrel/config/configuration.hpp"

int main(int argc, char** argv) {
    using namespace kestrel;

    CLI::App app{
        "Exercises the data and trading interfaces of the configured "
        "provider, backtests strategies and runs them. Does only what the "
        "subcommand says."};
    app.set_version_flag("--version", "kestrel 0.1.0");
    app.require_subcommand(1);

    std::string config_path;
    app.add_option("-c,--config", config_path, "Path to the TOML configuration")
        ->required()
        ->check(CLI::ExistingFile);
    bool verbose = false;
    app.add_flag("-v,--verbose", verbose, "Enable debug logging");

    app::CommandSlot command;
    app::AddBarsCommand(app, command);
    app::AddAccountCommand(app, command);
    app::AddOrderCommand(app, command);
    app::AddCancelCommand(app, command);
    app::AddStreamCommand(app, command);
    app::AddStreamBarsCommand(app, command);
    app::AddInitDbCommand(app, command);
    app::AddRunCommand(app, command);
    app::AddBacktestCommand(app, command);

    CLI11_PARSE(app, argc, argv);

    if (verbose) {
        spdlog::set_level(spdlog::level::debug);
    }
    auto config = config::LoadConfiguration(config_path);
    if (!config) {
        spdlog::error("config: {}", config.error().ToString());
        return 1;
    }
    return command->Run(**config);
}
