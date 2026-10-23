#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "kestrel/config/configuration.hpp"
#include "kestrel/data/data_source.hpp"
#include "kestrel/strategy/runner.hpp"
#include "kestrel/strategy/strategy.hpp"
#include "kestrel/trading/risk_gate.hpp"
#include "kestrel/trading/trading_venue.hpp"

namespace kestrel {

// The two halves of a provider connection. Market data and order execution are
// separate objects: only the venue differs between paper and live mode.
struct Components {
    std::shared_ptr<data::DataSource> data_source;
    std::unique_ptr<trading::TradingVenue> venue;
};

// Builds the provider's DataSource and a TradingVenue chosen by
// configuration.GetMode(): a PaperTradingVenue (already streaming quotes for
// the configured symbols) in paper mode, the provider's live venue in live
// mode. Callers use only the abstract interfaces.
//
// Live mode fails with kAuth if the provider's credentials are not configured.
types::Result<Components> CreateComponents(
    const config::Configuration& configuration);

// Just the provider's DataSource, for work that needs no venue (backtests).
// Unlike CreateComponents it starts no streams.
types::Result<std::shared_ptr<data::DataSource>> CreateDataSource(
    const config::Configuration& configuration);

// Names accepted by CreateStrategy.
std::vector<std::string> StrategyNames();

// Builds the named strategy from the configuration's [strategy] parameters.
// kNotFound for an unknown name; kInvalidArgument for a missing, malformed or
// unknown parameter, or a symbol that is not in [general] symbols.
//
//   sma_crossover: symbol (required), quantity (required), timeframe (1h),
//                  fast_period (10), slow_period (30)
types::Result<std::unique_ptr<strategy::Strategy>> CreateStrategy(
    const std::string& name, const config::Configuration& configuration);

// The risk limits for the named strategy: [risk] with [risk.<strategy>]
// overrides laid over it (see Configuration::GetRiskLimits). Absent when no
// limit applies. kInvalidArgument if a [risk.<name>] section names a strategy
// that does not exist, since a misspelled section would otherwise be ignored
// and its limits silently not applied.
types::Result<std::optional<trading::RiskLimits>> ResolveRiskLimits(
    const std::string& strategy, const config::Configuration& configuration);

// Checks that the configuration may be used to trade (--run) with `limits`
// (from ResolveRiskLimits). Paper mode always passes. Live mode fails with
// kInvalidArgument unless `limits` is set, and with kNotImplemented if the
// provider's trading venue is still a stub; both are refused here, at
// startup, rather than at the first order.
types::Result<void> ValidateForRun(
    const config::Configuration& configuration,
    const std::optional<trading::RiskLimits>& limits);

// How CreateRunner gets its provider connection; CreateComponents by default.
// Tests pass one that returns fakes.
using ComponentsFactory =
    std::function<types::Result<Components>(const config::Configuration&)>;

// Builds everything `kestrel_app run` needs, ready for Runner::Run(): checks
// the configuration may trade (ValidateForRun), resolves the strategy's risk
// limits, builds the strategy, the provider connection (CreateComponents) and
// a Runner with the risk gate when limits apply, and starts the gate's
// daily-loss count. Any failure is returned, with a prefix saying which step
// failed, and nothing is left running. The runner has no risk gate (see
// Runner::RiskGate) when no limits apply, which is allowed only in paper mode.
types::Result<std::unique_ptr<strategy::Runner>> CreateRunner(
    const std::string& strategy_name,
    const config::Configuration& configuration,
    const ComponentsFactory& make_components = CreateComponents);

}  // namespace kestrel
