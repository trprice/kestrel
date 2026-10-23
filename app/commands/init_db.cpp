#include <spdlog/spdlog.h>

#include <iostream>

#include "command.hpp"
#include "common.hpp"

namespace kestrel::app {

namespace {

class InitDbCommand : public Command {
   public:
    int Run(const config::Configuration& config) override {
        // Opening the database migrates it; that is the whole job.
        if (auto database = OpenDatabase(config); !database) {
            spdlog::error("database: {}", database.error().ToString());
            return 1;
        }
        std::cout << "database schema is up to date\n";
        return 0;
    }
};

}  // namespace

void AddInitDbCommand(CLI::App& app, CommandSlot& slot) {
    auto* sub = AddSubcommand(
        app, "init-db",
        "Create or update the database schema (tables and hypertables); see "
        "scripts/setup_database.sh");
    sub->callback([&slot] { slot = std::make_unique<InitDbCommand>(); });
}

}  // namespace kestrel::app
