#include "kestrel/factory.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <charconv>
#include <set>
#include <stdexcept>

#include "kestrel/config/schwab_token_store.hpp"
#include "kestrel/data/binanceus_data_source.hpp"
#include "kestrel/data/kraken_data_source.hpp"
#include "kestrel/data/schwab_data_source.hpp"
#include "kestrel/strategy/market_maker.hpp"
#include "kestrel/strategy/sma_crossover.hpp"
#include "kestrel/trading/binanceus_trading_venue.hpp"
#include "kestrel/trading/kraken_trading_venue.hpp"
#include "kestrel/trading/paper_trading_venue.hpp"
#include "kestrel/trading/schwab_trading_venue.hpp"

namespace kestrel {

using types::ErrorCategory;
using types::MakeError;
using types::Result;

namespace {

using VenuePtr = std::unique_ptr<trading::TradingVenue>;

Result<VenuePtr> MakePaper(const config::Configuration& cfg,
                           std::shared_ptr<data::DataSource> ds) {
    auto venue = std::make_unique<trading::PaperTradingVenue>(
        std::move(ds), cfg.GetPaperOptions());
    if (auto started = venue->Start(cfg.GetSymbols()); !started) {
        return std::unexpected(started.error());
    }
    return VenuePtr(std::move(venue));
}

types::Error MissingCredentials(std::string_view provider,
                                std::string_view hint) {
    return MakeError(ErrorCategory::kAuth,
                     "live mode on " + std::string(provider) +
                         " needs credentials; set " + std::string(hint) +
                         " or add them to the secrets file")
        .error();
}

Result<Components> BuildKraken(const config::KrakenConfiguration& cfg) {
    data::KrakenDataSource::Options options;
    options.rest_base_url = cfg.RestUrl();
    options.ws_url = cfg.WsUrl();
    auto ds = std::make_shared<data::KrakenDataSource>(
        std::make_shared<net::CprHttpClient>(), options);
    if (cfg.GetMode() == config::Mode::kPaper) {
        auto venue = MakePaper(cfg, ds);
        if (!venue) {
            return std::unexpected(venue.error());
        }
        return Components{ds, std::move(*venue)};
    }
    if (!cfg.Credentials()) {
        return std::unexpected(MissingCredentials(
            "kraken", "KESTREL_KRAKEN_API_KEY and KESTREL_KRAKEN_API_SECRET"));
    }
    return Components{ds, std::make_unique<trading::KrakenTradingVenue>(
                              *cfg.Credentials(), ds)};
}

Result<Components> BuildBinanceUS(const config::BinanceUSConfiguration& cfg) {
    auto ds = std::make_shared<data::BinanceUSDataSource>();
    if (cfg.GetMode() == config::Mode::kPaper) {
        auto venue = MakePaper(cfg, ds);
        if (!venue) {
            return std::unexpected(venue.error());
        }
        return Components{ds, std::move(*venue)};
    }
    if (!cfg.Credentials()) {
        return std::unexpected(MissingCredentials(
            "binanceus",
            "KESTREL_BINANCEUS_API_KEY and KESTREL_BINANCEUS_API_SECRET"));
    }
    return Components{ds, std::make_unique<trading::BinanceUSTradingVenue>(
                              *cfg.Credentials(), ds)};
}

Result<Components> BuildSchwab(const config::SchwabConfiguration& cfg) {
    auto ds = std::make_shared<data::SchwabDataSource>();
    if (cfg.GetMode() == config::Mode::kPaper) {
        auto venue = MakePaper(cfg, ds);
        if (!venue) {
            return std::unexpected(venue.error());
        }
        return Components{ds, std::move(*venue)};
    }
    if (!cfg.Credentials()) {
        return std::unexpected(MissingCredentials(
            "schwab", "KESTREL_SCHWAB_APP_KEY and KESTREL_SCHWAB_APP_SECRET"));
    }
    // Surface an expired refresh token now, loudly, instead of on the first
    // order.
    const config::SchwabTokenStore store(cfg.TokenFile());
    auto tokens = store.Load();
    if (!tokens) {
        config::SchwabTokenStore::LogReauthorizationInstructions(
            cfg.Credentials()->app_key, cfg.RedirectUri());
        return MakeError(ErrorCategory::kAuth,
                         "no usable Schwab tokens: " + tokens.error().message);
    }
    if (config::SchwabTokenStore::Check(*tokens, types::Now()) ==
        config::SchwabTokenHealth::kReauthorizationRequired) {
        config::SchwabTokenStore::LogReauthorizationInstructions(
            cfg.Credentials()->app_key, cfg.RedirectUri());
        return MakeError(
            ErrorCategory::kAuth,
            "Schwab refresh token expired; re-authorization required");
    }
    return Components{ds, std::make_unique<trading::SchwabTradingVenue>(
                              *cfg.Credentials(), ds)};
}

using StrategyPtr = std::unique_ptr<strategy::Strategy>;

// Reads the [strategy] parameters, tracking which were used so that a
// misspelled one is an error rather than silently ignored.
class ParamReader {
   public:
    ParamReader(const config::StrategyParams& params,
                const config::Configuration& cfg)
        : mParams(params), mCfg(cfg) {}

    Result<std::string> Required(const std::string& key) {
        auto it = mParams.find(key);
        if (it == mParams.end()) {
            return Missing(key);
        }
        mUsed.insert(key);
        return it->second;
    }

    Result<types::Symbol> Symbol() {
        auto text = Required("symbol");
        if (!text) {
            return std::unexpected(text.error());
        }
        auto symbol = types::NormalizeSymbol(*text);
        if (!symbol) {
            return std::unexpected(symbol.error());
        }
        const auto& allowed = mCfg.GetSymbols();
        if (std::find(allowed.begin(), allowed.end(), *symbol) ==
            allowed.end()) {
            return MakeError(ErrorCategory::kInvalidArgument,
                             "[strategy] symbol " + *symbol +
                                 " is not in [general] symbols");
        }
        return *symbol;
    }

    Result<types::Decimal> Quantity(const std::string& key) {
        auto text = Required(key);
        if (!text) {
            return std::unexpected(text.error());
        }
        auto d = types::Decimal::Parse(*text);
        if (!d) {
            return Bad(key, "a decimal number");
        }
        return *d;
    }

    // An optional decimal parameter.
    Result<types::Decimal> Number(const std::string& key,
                                  const types::Decimal& fallback) {
        auto it = mParams.find(key);
        if (it == mParams.end()) {
            return fallback;
        }
        mUsed.insert(key);
        auto d = types::Decimal::Parse(it->second);
        if (!d) {
            return Bad(key, "a decimal number");
        }
        return *d;
    }

    // An optional true / false parameter.
    Result<bool> Flag(const std::string& key, bool fallback) {
        auto it = mParams.find(key);
        if (it == mParams.end()) {
            return fallback;
        }
        mUsed.insert(key);
        if (it->second == "true") return true;
        if (it->second == "false") return false;
        return Bad(key, "true or false");
    }

    // An optional whole number of milliseconds; zero is allowed.
    Result<std::chrono::milliseconds> Millis(
        const std::string& key, std::chrono::milliseconds fallback) {
        auto it = mParams.find(key);
        if (it == mParams.end()) {
            return fallback;
        }
        mUsed.insert(key);
        std::int64_t value = 0;
        const auto& text = it->second;
        const auto [end, ec] =
            std::from_chars(text.data(), text.data() + text.size(), value);
        if (ec != std::errc() || end != text.data() + text.size() ||
            value < 0) {
            return Bad(key, "a whole number of milliseconds");
        }
        return std::chrono::milliseconds(value);
    }

    Result<types::Timeframe> Timeframe(const std::string& key,
                                       types::Timeframe fallback) {
        auto it = mParams.find(key);
        if (it == mParams.end()) {
            return fallback;
        }
        mUsed.insert(key);
        auto tf = types::ParseTimeframe(it->second);
        if (!tf) {
            return Bad(key, "a timeframe such as 1h");
        }
        return *tf;
    }

    Result<std::size_t> Count(const std::string& key, std::size_t fallback) {
        auto it = mParams.find(key);
        if (it == mParams.end()) {
            return fallback;
        }
        mUsed.insert(key);
        std::size_t value = 0;
        const auto& text = it->second;
        const auto [end, ec] =
            std::from_chars(text.data(), text.data() + text.size(), value);
        if (ec != std::errc() || end != text.data() + text.size() ||
            value == 0) {
            return Bad(key, "a positive integer");
        }
        return value;
    }

    Result<void> Finish() const {
        for (const auto& [key, value] : mParams) {
            if (!mUsed.contains(key)) {
                return MakeError(
                    ErrorCategory::kInvalidArgument,
                    "[strategy] has an unknown parameter '" + key + "'");
            }
        }
        return {};
    }

   private:
    static std::unexpected<types::Error> Missing(const std::string& key) {
        return std::unexpected(MakeError(ErrorCategory::kInvalidArgument,
                                         "[strategy] needs '" + key + "'")
                                   .error());
    }
    static std::unexpected<types::Error> Bad(const std::string& key,
                                             const char* wanted) {
        return std::unexpected(
            MakeError(ErrorCategory::kInvalidArgument,
                      "[strategy] " + key + " must be " + wanted)
                .error());
    }

    const config::StrategyParams& mParams;
    const config::Configuration& mCfg;
    std::set<std::string> mUsed;
};

Result<StrategyPtr> BuildSmaCrossover(ParamReader& p) {
    strategy::SmaCrossoverParams params;
    auto symbol = p.Symbol();
    auto quantity = p.Quantity("quantity");
    auto timeframe = p.Timeframe("timeframe", params.timeframe);
    auto fast = p.Count("fast_period", params.fast_period);
    auto slow = p.Count("slow_period", params.slow_period);
    if (!symbol) return std::unexpected(symbol.error());
    if (!quantity) return std::unexpected(quantity.error());
    if (!timeframe) return std::unexpected(timeframe.error());
    if (!fast) return std::unexpected(fast.error());
    if (!slow) return std::unexpected(slow.error());
    if (auto ok = p.Finish(); !ok) {
        return std::unexpected(ok.error());
    }
    params.symbol = *symbol;
    params.quantity = *quantity;
    params.timeframe = *timeframe;
    params.fast_period = *fast;
    params.slow_period = *slow;
    try {
        return StrategyPtr(
            std::make_unique<strategy::SmaCrossover>(std::move(params)));
    } catch (const std::invalid_argument& e) {
        return MakeError(ErrorCategory::kInvalidArgument,
                         std::string("[strategy] ") + e.what());
    }
}

// Stores a read parameter, keeping the first error.
template <typename T>
void Take(Result<T> read, T& target, std::optional<types::Error>& error) {
    if (read) {
        target = *read;
    } else if (!error) {
        error = read.error();
    }
}

// The protection rules: each is off unless its flag is set. Every parameter
// is read (and so checked) whether or not its rule is on.
Result<void> ReadProtection(ParamReader& p,
                            strategy::MarketMakerProtection& r) {
    std::optional<types::Error> error;
    Take(p.Flag("stop_loss", r.stop_loss), r.stop_loss, error);
    Take(p.Number("stop_loss_bps", r.stop_loss_bps), r.stop_loss_bps, error);
    Take(p.Flag("loss_cap", r.loss_cap), r.loss_cap, error);
    Take(p.Number("loss_cap_amount", r.loss_cap_amount), r.loss_cap_amount,
         error);
    Take(p.Flag("drawdown_cap", r.drawdown_cap), r.drawdown_cap, error);
    Take(p.Number("drawdown_cap_amount", r.drawdown_cap_amount),
         r.drawdown_cap_amount, error);
    Take(p.Flag("spike_standdown", r.spike_standdown), r.spike_standdown,
         error);
    Take(p.Number("spike_3m_bps", r.spike_3m_bps), r.spike_3m_bps, error);
    Take(p.Number("spike_5m_bps", r.spike_5m_bps), r.spike_5m_bps, error);
    Take(p.Number("spike_15m_bps", r.spike_15m_bps), r.spike_15m_bps, error);
    Take(p.Millis("spike_pause_ms", r.spike_pause), r.spike_pause, error);
    Take(p.Flag("downtrend_filter", r.downtrend_filter), r.downtrend_filter,
         error);
    Take(p.Number("downtrend_bps", r.downtrend_bps), r.downtrend_bps, error);
    Take(p.Flag("momentum_filter", r.momentum_filter), r.momentum_filter,
         error);
    Take(p.Number("momentum_bps", r.momentum_bps), r.momentum_bps, error);
    Take(p.Flag("cooldown_after_stop", r.cooldown_after_stop),
         r.cooldown_after_stop, error);
    Take(p.Millis("cooldown_ms", r.cooldown), r.cooldown, error);
    if (error) {
        return std::unexpected(*error);
    }
    return {};
}

Result<StrategyPtr> BuildMarketMaker(ParamReader& p) {
    strategy::MarketMakerParams params;
    auto symbol = p.Symbol();
    auto quantity = p.Quantity("quantity");
    auto half_spread = p.Quantity("half_spread_bps");
    auto max_inventory = p.Quantity("max_inventory");
    auto skew = p.Number("skew_bps", params.skew_bps);
    auto requote = p.Number("requote_bps", params.requote_bps);
    auto max_age = p.Millis("max_quote_age_ms", params.max_quote_age);
    auto protection = ReadProtection(p, params.protection);
    if (!symbol) return std::unexpected(symbol.error());
    if (!quantity) return std::unexpected(quantity.error());
    if (!half_spread) return std::unexpected(half_spread.error());
    if (!max_inventory) return std::unexpected(max_inventory.error());
    if (!skew) return std::unexpected(skew.error());
    if (!requote) return std::unexpected(requote.error());
    if (!max_age) return std::unexpected(max_age.error());
    if (!protection) return std::unexpected(protection.error());
    if (auto ok = p.Finish(); !ok) {
        return std::unexpected(ok.error());
    }
    params.symbol = *symbol;
    params.quantity = *quantity;
    params.half_spread_bps = *half_spread;
    params.max_inventory = *max_inventory;
    params.skew_bps = *skew;
    params.requote_bps = *requote;
    params.max_quote_age = *max_age;
    try {
        return StrategyPtr(
            std::make_unique<strategy::MarketMaker>(std::move(params)));
    } catch (const std::invalid_argument& e) {
        return MakeError(ErrorCategory::kInvalidArgument,
                         std::string("[strategy] ") + e.what());
    }
}

struct StrategyEntry {
    const char* name;
    Result<StrategyPtr> (*build)(ParamReader&);
};

constexpr StrategyEntry kStrategies[] = {
    {"sma_crossover", BuildSmaCrossover},
    {"market_maker", BuildMarketMaker},
};

// Whether the provider's TradingVenue is a real implementation. Update this
// as each stub is completed.
bool HasLiveVenue(config::Provider) { return false; }

}  // namespace

Result<std::shared_ptr<data::DataSource>> CreateDataSource(
    const config::Configuration& configuration) {
    switch (configuration.GetProvider()) {
        case config::Provider::kKraken: {
            const auto& cfg =
                dynamic_cast<const config::KrakenConfiguration&>(configuration);
            data::KrakenDataSource::Options options;
            options.rest_base_url = cfg.RestUrl();
            options.ws_url = cfg.WsUrl();
            return std::make_shared<data::KrakenDataSource>(
                std::make_shared<net::CprHttpClient>(), options);
        }
        case config::Provider::kBinanceUS:
            return std::make_shared<data::BinanceUSDataSource>();
        case config::Provider::kSchwab:
            return std::make_shared<data::SchwabDataSource>();
    }
    return MakeError(ErrorCategory::kInternal, "unknown provider");
}

std::vector<std::string> StrategyNames() {
    std::vector<std::string> names;
    for (const auto& entry : kStrategies) {
        names.emplace_back(entry.name);
    }
    return names;
}

Result<StrategyPtr> CreateStrategy(const std::string& name,
                                   const config::Configuration& cfg) {
    for (const auto& entry : kStrategies) {
        if (name == entry.name) {
            ParamReader reader(cfg.GetStrategyParams(), cfg);
            return entry.build(reader);
        }
    }
    return MakeError(ErrorCategory::kNotFound,
                     "unknown strategy '" + name + "'");
}

Result<std::optional<trading::RiskLimits>> ResolveRiskLimits(
    const std::string& strategy, const config::Configuration& cfg) {
    const auto names = StrategyNames();
    for (const auto& [name, limits] : cfg.GetRiskOverrides()) {
        if (std::find(names.begin(), names.end(), name) == names.end()) {
            return MakeError(ErrorCategory::kInvalidArgument,
                             "[risk." + name + "] names an unknown strategy");
        }
    }
    return cfg.GetRiskLimits(strategy);
}

Result<void> ValidateForRun(const config::Configuration& cfg,
                            const std::optional<trading::RiskLimits>& limits) {
    if (cfg.GetMode() != config::Mode::kLive) {
        return {};
    }
    if (!limits) {
        return MakeError(ErrorCategory::kInvalidArgument,
                         "live mode needs a [risk] section with at least one "
                         "limit");
    }
    if (!HasLiveVenue(cfg.GetProvider())) {
        return MakeError(ErrorCategory::kNotImplemented,
                         "live trading on " +
                             std::string(config::ToString(cfg.GetProvider())) +
                             " is not implemented yet");
    }
    return {};
}

namespace {

std::unexpected<types::Error> WithContext(types::Error error,
                                          std::string_view context) {
    error.message = std::string(context) + ": " + error.message;
    return std::unexpected(std::move(error));
}

}  // namespace

Result<std::unique_ptr<strategy::Runner>> CreateRunner(
    const std::string& strategy_name, const config::Configuration& cfg,
    const ComponentsFactory& make_components) {
    auto limits = ResolveRiskLimits(strategy_name, cfg);
    if (!limits) {
        return WithContext(limits.error(), "risk");
    }
    if (auto ok = ValidateForRun(cfg, *limits); !ok) {
        return std::unexpected(ok.error());
    }
    auto strategy = CreateStrategy(strategy_name, cfg);
    if (!strategy) {
        return WithContext(strategy.error(), "strategy");
    }
    auto components = make_components(cfg);
    if (!components) {
        return WithContext(components.error(), "startup");
    }

    strategy::RunnerOptions options;
    options.risk = *limits;
    auto runner = std::make_unique<strategy::Runner>(
        components->data_source,
        std::shared_ptr<trading::TradingVenue>(std::move(components->venue)),
        std::move(*strategy), options);
    if (const auto gate = runner->RiskGate()) {
        // The loss count starts here. If it cannot, trading without it would
        // be trading without the limit.
        if (auto baseline = gate->ResetDailyBaseline(); !baseline) {
            return WithContext(baseline.error(),
                               "cannot start the daily loss count");
        }
    }
    return runner;
}

Result<Components> CreateComponents(
    const config::Configuration& configuration) {
    switch (configuration.GetProvider()) {
        case config::Provider::kKraken:
            return BuildKraken(dynamic_cast<const config::KrakenConfiguration&>(
                configuration));
        case config::Provider::kBinanceUS:
            return BuildBinanceUS(
                dynamic_cast<const config::BinanceUSConfiguration&>(
                    configuration));
        case config::Provider::kSchwab:
            return BuildSchwab(dynamic_cast<const config::SchwabConfiguration&>(
                configuration));
    }
    return MakeError(ErrorCategory::kInternal, "unknown provider");
}

}  // namespace kestrel
