#pragma once

// Pieces shared by the kestrel_app commands.

#include <CLI/CLI.hpp>
#include <memory>
#include <optional>
#include <string>

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

}  // namespace kestrel::app
