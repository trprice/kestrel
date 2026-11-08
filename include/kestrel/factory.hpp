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
#include "kestrel/trading/trade_log.hpp"
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
// Schwab needs them in paper mode too, with a token file from `kestrel_app
// authorize`, since its market data is behind OAuth.
types::Result<Components> CreateComponents(
    const config::Configuration& configuration);

// Just the provider's DataSource, for work that needs no venue (backtests).
// Unlike CreateComponents it starts no streams. Schwab's needs its credentials
// and tokens, as above.
types::Result<std::shared_ptr<data::DataSource>> CreateDataSource(
    const config::Configuration& configuration);

// Names accepted by CreateStrategy.
std::vector<std::string> StrategyNames();

// Builds the named strategy from the configuration's [strategy] parameters.
// kNotFound for an unknown name; kInvalidArgument for a missing, malformed or
// unknown parameter, or a symbol that is not in [general] symbols. Each
// strategy's parameters are listed in docs/configuration.md.
types::Result<std::unique_ptr<strategy::Strategy>> CreateStrategy(
    const std::string& name, const config::Configuration& configuration);

// The risk limits for the named strategy: [risk] with [risk.<strategy>]
// overrides laid over it (see Configuration::GetRiskLimits). Absent when no
// limit applies. kInvalidArgument if a [risk.<name>] section names a strategy
// that does not exist, since a misspelled section would otherwise be ignored
// and its limits silently not applied.
types::Result<std::optional<trading::RiskLimits>> ResolveRiskLimits(
    const std::string& strategy, const config::Configuration& configuration);

// The [risk] limits that apply to an order placed by hand (`kestrel_app
// order`): the symbol allowlist, the order and position notional, open
// orders and the price age. Loss, drawdown and order-count limits are left
// out: a process that places one order would count them from its own start,
// so they could never trip. Absent when none of these is set.
std::optional<trading::RiskLimits> ManualOrderLimits(
    const std::optional<trading::RiskLimits>& limits);

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

// Where a run's fills and P&L are recorded: given the configuration, the
// strategy's name and the venue's, returns the log the run writes to. The
// default, OpenTradeLog, connects to the [database], applies the migrations
// and starts a row in `runs`; it fails with kNotFound without a [database].
// Tests pass one that returns a fake.
using TradeLogFactory =
    std::function<types::Result<std::shared_ptr<trading::TradeLog>>(
        const config::Configuration&, const std::string& strategy,
        const std::string& venue)>;
types::Result<std::shared_ptr<trading::TradeLog>> OpenTradeLog(
    const config::Configuration& configuration, const std::string& strategy,
    const std::string& venue);

// Where a strategy that keeps state across restarts (Requirements::state)
// keeps it, for the provider named (the data source's name, so a paper run's
// state is apart from a live one's by the mode, not by the paper venue). The
// default, OpenStateStore, connects to the [database], applies the migrations
// and scopes the keys by venue and mode; it fails with kNotFound without a
// [database]. Tests pass a fake.
using StateStoreFactory =
    std::function<types::Result<std::shared_ptr<strategy::StateStore>>(
        const config::Configuration&, const std::string& venue)>;
types::Result<std::shared_ptr<strategy::StateStore>> OpenStateStore(
    const config::Configuration& configuration, const std::string& venue);

// Builds everything `kestrel_app run` needs, ready for Runner::Run(): checks
// the configuration may trade (ValidateForRun), resolves the strategy's risk
// limits, builds the strategy, the provider connection (CreateComponents) and
// a Runner with the risk gate when limits apply, and starts the gate's
// daily-loss count. Any failure is returned, with a prefix saying which step
// failed, and nothing is left running. With `[recording] enabled` the runner
// records its fills and P&L through `make_log` (a failure to set that up is
// an error: a run that was asked to be recorded does not start unrecorded).
// A strategy that keeps state gets a store from `make_state`; without a
// [database] a live run is refused (its loss caps would restart from zero
// on each restart), and a paper run warns and keeps none.
// The runner has no risk gate (see
// Runner::RiskGate) when no limits apply, which is allowed only in paper mode.
types::Result<std::unique_ptr<strategy::Runner>> CreateRunner(
    const std::string& strategy_name,
    const config::Configuration& configuration,
    const ComponentsFactory& make_components = CreateComponents,
    const TradeLogFactory& make_log = OpenTradeLog,
    const StateStoreFactory& make_state = OpenStateStore);

}  // namespace kestrel
