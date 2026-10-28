#pragma once

#include <CLI/CLI.hpp>
#include <memory>

#include "kestrel/config/configuration.hpp"

namespace kestrel::app {

// One thing kestrel_app can be told to do. A command is built from its parsed
// arguments (see the Add...Command functions) and run once the configuration
// is loaded.
class Command {
   public:
    virtual ~Command() = default;

    // Returns the process exit code: 0 on success, 1 on failure, 2 for a
    // usage error that only shows once the command runs. Errors are logged
    // here, not thrown.
    virtual int Run(const config::Configuration& config) = 0;
};

// Filled in by whichever subcommand the user named.
using CommandSlot = std::unique_ptr<Command>;

// Each of these adds its subcommand, with that subcommand's own arguments, to
// `app`. When the subcommand is chosen, parsing leaves its Command in `slot`.
void AddInitDbCommand(CLI::App& app, CommandSlot& slot);
void AddBarsCommand(CLI::App& app, CommandSlot& slot);
void AddAccountCommand(CLI::App& app, CommandSlot& slot);
void AddStreamCommand(CLI::App& app, CommandSlot& slot);
void AddStreamBarsCommand(CLI::App& app, CommandSlot& slot);
void AddRunCommand(CLI::App& app, CommandSlot& slot);
void AddBacktestCommand(CLI::App& app, CommandSlot& slot);

}  // namespace kestrel::app
