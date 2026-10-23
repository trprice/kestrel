#include "common.hpp"

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

}  // namespace kestrel::app
