#include "kestrel/factory.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <charconv>
#include <set>
#include <stdexcept>

#include "kestrel/data/binanceus_data_source.hpp"
#include "kestrel/data/kraken_data_source.hpp"
#include "kestrel/data/schwab_auth.hpp"
#include "kestrel/data/schwab_data_source.hpp"
#include "kestrel/db/strategy_state_store.hpp"
#include "kestrel/db/trade_store.hpp"
#include "kestrel/net/timed_http_client.hpp"
#include "kestrel/strategy/breakout.hpp"
#include "kestrel/strategy/ma_crossover.hpp"
#include "kestrel/strategy/market_maker.hpp"
#include "kestrel/strategy/momentum_rotation.hpp"
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
    // A paper run can last for days: keep its history bounded.
    trading::PaperOptions options = cfg.GetPaperOptions();
    options.max_fills = 100'000;
    options.max_closed_orders = 100'000;
    auto venue =
        std::make_unique<trading::PaperTradingVenue>(std::move(ds), options);
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
        net::MakeTimedHttpClient(cfg.GetHttp().slow_request), options);
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
    auto http = net::MakeTimedHttpClient(cfg.GetHttp().slow_request);
    auto ds = std::make_shared<data::BinanceUSDataSource>(http);
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
                              *cfg.Credentials(), ds, cfg.GetSymbols(), http)};
}

// Schwab's market data needs OAuth tokens even in paper mode, so the data
// source fails to build without the app's credentials and a token file from
// `kestrel_app authorize`. An expired refresh token fails here, loudly, rather
// than at the first request.
Result<std::shared_ptr<data::SchwabDataSource>> MakeSchwabDataSource(
    const config::SchwabConfiguration& cfg) {
    if (!cfg.Credentials()) {
        return MakeError(
            ErrorCategory::kAuth,
            "Schwab needs the app's key and secret, even for market data in "
            "paper mode; set KESTREL_SCHWAB_APP_KEY and "
            "KESTREL_SCHWAB_APP_SECRET or add them to the secrets file");
    }
    auto http = net::MakeTimedHttpClient(cfg.GetHttp().slow_request);
    auto auth = std::make_shared<data::SchwabAuth>(
        *cfg.Credentials(), config::SchwabTokenStore(cfg.TokenFile()), http,
        cfg.RedirectUri());
    if (auto loaded = auth->Load(); !loaded) {
        return std::unexpected(loaded.error());
    }
    data::SchwabDataSource::Options options;
    options.extended_hours = cfg.ExtendedHours();
    options.takeover_reconnect = cfg.TakeoverReconnect();
    return std::make_shared<data::SchwabDataSource>(
        std::make_shared<data::SchwabApi>(auth, http,
                                          data::SchwabApi::Options{}),
        std::move(options));
}

Result<Components> BuildSchwab(const config::SchwabConfiguration& cfg) {
    auto ds = MakeSchwabDataSource(cfg);
    if (!ds) {
        return std::unexpected(ds.error());
    }
    if (cfg.GetMode() == config::Mode::kPaper) {
        auto venue = MakePaper(cfg, *ds);
        if (!venue) {
            return std::unexpected(venue.error());
        }
        return Components{*ds, std::move(*venue)};
    }
    return Components{*ds, std::make_unique<trading::SchwabTradingVenue>(
                               *cfg.Credentials(), *ds)};
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

    // The [general] symbols, for a strategy that trades them all.
    const std::vector<types::Symbol>& Universe() const {
        return mCfg.GetSymbols();
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

    // An optional word, one of `allowed`.
    Result<std::string> Word(const std::string& key,
                             const std::string& fallback,
                             std::initializer_list<std::string_view> allowed) {
        auto it = mParams.find(key);
        if (it == mParams.end()) {
            return fallback;
        }
        mUsed.insert(key);
        if (std::find(allowed.begin(), allowed.end(), it->second) ==
            allowed.end()) {
            std::string list;
            for (const auto word : allowed) {
                list +=
                    (list.empty() ? "\"" : ", \"") + std::string(word) + "\"";
            }
            return MakeError(ErrorCategory::kInvalidArgument,
                             "[strategy] " + key + " must be one of " + list);
        }
        return it->second;
    }

    // An optional whole number, zero allowed.
    Result<std::size_t> Whole(const std::string& key, std::size_t fallback) {
        auto it = mParams.find(key);
        if (it == mParams.end()) {
            return fallback;
        }
        mUsed.insert(key);
        std::size_t value = 0;
        const auto& text = it->second;
        const auto [end, ec] =
            std::from_chars(text.data(), text.data() + text.size(), value);
        if (ec != std::errc() || end != text.data() + text.size()) {
            return Bad(key, "a whole number");
        }
        return value;
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

Result<StrategyPtr> BuildMaCrossover(ParamReader& p) {
    strategy::MaCrossoverParams params;
    auto symbol = p.Symbol();
    auto quantity = p.Quantity("quantity");
    auto timeframe = p.Timeframe("timeframe", params.timeframe);
    auto fast = p.Count("fast_period", params.fast_period);
    auto slow = p.Count("slow_period", params.slow_period);
    std::string average = "sma";
    auto average_word = p.Word("average", average, {"sma", "ema"});
    if (!symbol) return std::unexpected(symbol.error());
    if (!quantity) return std::unexpected(quantity.error());
    if (!timeframe) return std::unexpected(timeframe.error());
    if (!fast) return std::unexpected(fast.error());
    if (!slow) return std::unexpected(slow.error());
    if (!average_word) return std::unexpected(average_word.error());
    if (auto ok = p.Finish(); !ok) {
        return std::unexpected(ok.error());
    }
    params.symbol = *symbol;
    params.quantity = *quantity;
    params.timeframe = *timeframe;
    params.fast_period = *fast;
    params.slow_period = *slow;
    params.average = *average_word == "ema"
                         ? strategy::MaCrossoverParams::Average::kEma
                         : strategy::MaCrossoverParams::Average::kSma;
    try {
        return StrategyPtr(
            std::make_unique<strategy::MaCrossover>(std::move(params)));
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
    Take(p.Flag("reset_state", r.reset_state), r.reset_state, error);
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
    auto vol_multiplier = p.Number("vol_multiplier", params.vol_multiplier);
    auto vol_window = p.Millis("vol_window_ms", params.vol_window);
    auto max_half_spread =
        p.Number("max_half_spread_bps", params.max_half_spread_bps);
    auto microprice = p.Number("microprice_weight", params.microprice_weight);
    auto flow_pull =
        p.Number("flow_pull_imbalance", params.flow_pull_imbalance);
    auto flow_window = p.Millis("flow_window_ms", params.flow_window);
    auto skew = p.Number("skew_bps", params.skew_bps);
    auto skew_power = p.Number("skew_power", params.skew_power);
    auto skew_age = p.Millis("skew_age_ms", params.skew_age);
    auto requote = p.Number("requote_bps", params.requote_bps);
    auto requote_toward =
        p.Number("requote_toward_bps", requote ? *requote : params.requote_bps);
    auto max_age = p.Millis("max_quote_age_ms", params.max_quote_age);
    auto post_only = p.Flag("post_only", params.post_only);
    auto keep_at_touch = p.Flag("keep_at_touch", params.keep_at_touch);
    auto quiet_pull = p.Millis("quiet_pull_ms", params.quiet_pull);
    auto levels = p.Count("levels", params.levels);
    auto level_step = p.Number("level_step_bps", params.level_step_bps);
    std::string touch_word = "compute";
    auto touch = p.Word("touch", touch_word, {"compute", "join", "improve"});
    auto protection = ReadProtection(p, params.protection);
    if (!symbol) return std::unexpected(symbol.error());
    if (!quantity) return std::unexpected(quantity.error());
    if (!half_spread) return std::unexpected(half_spread.error());
    if (!max_inventory) return std::unexpected(max_inventory.error());
    if (!vol_multiplier) return std::unexpected(vol_multiplier.error());
    if (!vol_window) return std::unexpected(vol_window.error());
    if (!max_half_spread) return std::unexpected(max_half_spread.error());
    if (!microprice) return std::unexpected(microprice.error());
    if (!flow_pull) return std::unexpected(flow_pull.error());
    if (!flow_window) return std::unexpected(flow_window.error());
    if (!skew) return std::unexpected(skew.error());
    if (!skew_power) return std::unexpected(skew_power.error());
    if (!skew_age) return std::unexpected(skew_age.error());
    if (!requote) return std::unexpected(requote.error());
    if (!requote_toward) return std::unexpected(requote_toward.error());
    if (!max_age) return std::unexpected(max_age.error());
    if (!post_only) return std::unexpected(post_only.error());
    if (!keep_at_touch) return std::unexpected(keep_at_touch.error());
    if (!quiet_pull) return std::unexpected(quiet_pull.error());
    if (!levels) return std::unexpected(levels.error());
    if (!level_step) return std::unexpected(level_step.error());
    if (!touch) return std::unexpected(touch.error());
    if (!protection) return std::unexpected(protection.error());
    if (auto ok = p.Finish(); !ok) {
        return std::unexpected(ok.error());
    }
    params.symbol = *symbol;
    params.quantity = *quantity;
    params.half_spread_bps = *half_spread;
    params.max_inventory = *max_inventory;
    params.vol_multiplier = *vol_multiplier;
    params.vol_window = *vol_window;
    params.max_half_spread_bps = *max_half_spread;
    params.microprice_weight = *microprice;
    params.flow_pull_imbalance = *flow_pull;
    params.flow_window = *flow_window;
    params.skew_bps = *skew;
    params.skew_power = *skew_power;
    params.skew_age = *skew_age;
    params.requote_bps = *requote;
    params.requote_toward_bps = *requote_toward;
    params.max_quote_age = *max_age;
    params.post_only = *post_only;
    params.keep_at_touch = *keep_at_touch;
    params.quiet_pull = *quiet_pull;
    params.levels = *levels;
    params.level_step_bps = *level_step;
    using Touch = strategy::MarketMakerParams::Touch;
    params.touch = *touch == "join"      ? Touch::kJoin
                   : *touch == "improve" ? Touch::kImprove
                                         : Touch::kCompute;
    try {
        return StrategyPtr(
            std::make_unique<strategy::MarketMaker>(std::move(params)));
    } catch (const std::invalid_argument& e) {
        return MakeError(ErrorCategory::kInvalidArgument,
                         std::string("[strategy] ") + e.what());
    }
}

Result<StrategyPtr> BuildBreakout(ParamReader& p) {
    using Params = strategy::BreakoutParams;
    Params params;
    std::optional<types::Error> error;
    Take(p.Symbol(), params.symbol, error);
    Take(p.Quantity("quantity"), params.quantity, error);
    Take(p.Timeframe("timeframe", params.timeframe), params.timeframe, error);
    std::string setup = "nr";
    Take(p.Word("setup", setup,
                {"nr", "inside", "range", "rectangle", "pipe",
                 "failed_breakdown", "double_bottom", "triple_bottom",
                 "opening_range", "inverse_head_shoulders",
                 "ascending_triangle", "symmetrical_triangle", "cup_handle"}),
         setup, error);
    params.setup =
        setup == "inside"             ? Params::Setup::kInsideBar
        : setup == "range"            ? Params::Setup::kRange
        : setup == "rectangle"        ? Params::Setup::kRectangle
        : setup == "pipe"             ? Params::Setup::kPipeBottom
        : setup == "failed_breakdown" ? Params::Setup::kFailedBreakdown
        : setup == "double_bottom" || setup == "triple_bottom"
            ? Params::Setup::kMultipleBottom
        : setup == "opening_range" ? Params::Setup::kOpeningRange
        : setup == "inverse_head_shoulders"
            ? Params::Setup::kInverseHeadShoulders
        : setup == "ascending_triangle" || setup == "symmetrical_triangle"
            ? Params::Setup::kTriangle
        : setup == "cup_handle" ? Params::Setup::kCupHandle
                                : Params::Setup::kNarrowRange;
    params.bottoms = setup == "triple_bottom" ? 3 : 2;
    params.triangle = setup == "symmetrical_triangle"
                          ? strategy::TriangleKind::kSymmetrical
                          : strategy::TriangleKind::kAscending;
    Take(p.Count("nr_bars", params.nr_bars), params.nr_bars, error);
    Take(p.Count("range_bars", params.range_bars), params.range_bars, error);
    Take(p.Number("touch_zone", params.touch_zone), params.touch_zone, error);
    Take(p.Count("min_touches", params.min_touches), params.min_touches, error);
    Take(p.Flag("shortfall", params.shortfall), params.shortfall, error);
    Take(p.Count("decline_bars", params.decline_bars), params.decline_bars,
         error);
    Take(p.Count("reclaim_bars", params.reclaim_bars), params.reclaim_bars,
         error);
    Take(p.Count("swing_bars", params.swing_bars), params.swing_bars, error);
    Take(p.Count("opening_bars", params.opening_bars), params.opening_bars,
         error);
    std::string confirm = "none";
    Take(p.Word("confirm", confirm, {"none", "closes", "percent"}), confirm,
         error);
    params.confirm = confirm == "closes"    ? Params::Confirm::kCloses
                     : confirm == "percent" ? Params::Confirm::kPercent
                                            : Params::Confirm::kNone;
    Take(p.Count("confirm_closes", params.confirm_closes),
         params.confirm_closes, error);
    Take(p.Number("confirm_percent", params.confirm_percent),
         params.confirm_percent, error);
    std::string entry = "stop";
    Take(p.Word("entry", entry, {"stop", "throwback"}), entry, error);
    params.entry =
        entry == "throwback" ? Params::Entry::kThrowback : Params::Entry::kStop;
    Take(p.Count("throwback_bars", params.throwback_bars),
         params.throwback_bars, error);
    Take(p.Number("stop_buffer_bps", params.stop_buffer_bps),
         params.stop_buffer_bps, error);
    Take(p.Number("atr_stop", params.atr_stop), params.atr_stop, error);
    Take(p.Count("atr_bars", params.atr_bars), params.atr_bars, error);
    Take(p.Number("target_multiple", params.target_multiple),
         params.target_multiple, error);
    Take(p.Flag("resting_target", params.resting_target), params.resting_target,
         error);
    std::string direction = "long";
    Take(p.Word("direction", direction, {"long", "short"}), direction, error);
    params.direction = direction == "short" ? Params::Direction::kShort
                                            : Params::Direction::kLong;
    Take(p.Flag("reverse", params.reverse), params.reverse, error);
    Take(p.Count("arm_bars", params.arm_bars), params.arm_bars, error);
    Take(p.Whole("max_hold_bars", params.max_hold_bars), params.max_hold_bars,
         error);
    Take(p.Whole("trend_sma", params.trend_sma), params.trend_sma, error);
    Take(p.Flag("flatten_at_close", params.flatten_at_close),
         params.flatten_at_close, error);
    Take(p.Number("volume_ratio", params.volume_ratio), params.volume_ratio,
         error);
    Take(p.Count("volume_bars", params.volume_bars), params.volume_bars, error);
    Take(p.Number("breakeven_r", params.breakeven_r), params.breakeven_r,
         error);
    Take(p.Whole("trail_bars", params.trail_bars), params.trail_bars, error);
    if (error) {
        return std::unexpected(*error);
    }
    if (auto ok = p.Finish(); !ok) {
        return std::unexpected(ok.error());
    }
    try {
        return StrategyPtr(
            std::make_unique<strategy::Breakout>(std::move(params)));
    } catch (const std::invalid_argument& e) {
        return MakeError(ErrorCategory::kInvalidArgument,
                         std::string("[strategy] ") + e.what());
    }
}

Result<StrategyPtr> BuildMomentumRotation(ParamReader& p) {
    strategy::MomentumRotationParams params;
    params.symbols = p.Universe();
    std::optional<types::Error> error;
    Take(p.Timeframe("timeframe", params.timeframe), params.timeframe, error);
    Take(p.Count("lookback_bars", params.lookback_bars), params.lookback_bars,
         error);
    Take(p.Count("rebalance_bars", params.rebalance_bars),
         params.rebalance_bars, error);
    Take(p.Count("top_n", params.top_n), params.top_n, error);
    Take(p.Flag("positive_only", params.positive_only), params.positive_only,
         error);
    Take(p.Number("invest_fraction", params.invest_fraction),
         params.invest_fraction, error);
    if (error) {
        return std::unexpected(*error);
    }
    if (auto ok = p.Finish(); !ok) {
        return std::unexpected(ok.error());
    }
    try {
        return StrategyPtr(
            std::make_unique<strategy::MomentumRotation>(std::move(params)));
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
    {"ma_crossover", BuildMaCrossover},
    {"market_maker", BuildMarketMaker},
    {"breakout", BuildBreakout},
    {"momentum_rotation", BuildMomentumRotation},
};

// Whether the provider's TradingVenue is a real implementation. Update this
// as each stub is completed.
bool HasLiveVenue(config::Provider provider) {
    return provider == config::Provider::kBinanceUS;
}

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
                net::MakeTimedHttpClient(cfg.GetHttp().slow_request), options);
        }
        case config::Provider::kBinanceUS:
            return std::make_shared<data::BinanceUSDataSource>(
                net::MakeTimedHttpClient(configuration.GetHttp().slow_request));
        case config::Provider::kSchwab: {
            auto ds = MakeSchwabDataSource(
                dynamic_cast<const config::SchwabConfiguration&>(
                    configuration));
            if (!ds) {
                return std::unexpected(ds.error());
            }
            return std::shared_ptr<data::DataSource>(std::move(*ds));
        }
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

std::optional<trading::RiskLimits> ManualOrderLimits(
    const std::optional<trading::RiskLimits>& limits) {
    if (!limits) {
        return std::nullopt;
    }
    trading::RiskLimits manual;
    manual.allowed_symbols = limits->allowed_symbols;
    manual.max_order_notional = limits->max_order_notional;
    manual.max_position_notional = limits->max_position_notional;
    manual.max_open_orders = limits->max_open_orders;
    manual.max_price_age = limits->max_price_age;
    if (manual.allowed_symbols.empty() && !manual.max_order_notional &&
        !manual.max_position_notional && !manual.max_open_orders) {
        return std::nullopt;
    }
    return manual;
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

Result<std::shared_ptr<trading::TradeLog>> OpenTradeLog(
    const config::Configuration& cfg, const std::string& strategy,
    const std::string& venue) {
    const auto& options = cfg.GetDatabase();
    if (!options) {
        return MakeError(ErrorCategory::kNotFound,
                         "[recording] needs a [database] section");
    }
    auto connected = db::Database::Connect(*options);
    if (!connected) {
        return std::unexpected(connected.error());
    }
    std::shared_ptr<db::Database> database = std::move(*connected);
    if (auto migrated = database->Migrate(); !migrated) {
        return std::unexpected(migrated.error());
    }
    auto store = db::TradeStore::StartRun(
        database, db::RunInfo{strategy, venue,
                              std::string(config::ToString(cfg.GetMode())),
                              types::Now()});
    if (!store) {
        return std::unexpected(store.error());
    }
    spdlog::info("recording run {} to the database", (*store)->RunId());
    return std::shared_ptr<trading::TradeLog>(std::move(*store));
}

Result<std::shared_ptr<strategy::StateStore>> OpenStateStore(
    const config::Configuration& cfg, const std::string& venue) {
    const auto& options = cfg.GetDatabase();
    if (!options) {
        return MakeError(ErrorCategory::kNotFound,
                         "keeping strategy state needs a [database] section");
    }
    auto connected = db::Database::Connect(*options);
    if (!connected) {
        return std::unexpected(connected.error());
    }
    std::shared_ptr<db::Database> database = std::move(*connected);
    if (auto migrated = database->Migrate(); !migrated) {
        return std::unexpected(migrated.error());
    }
    return std::shared_ptr<strategy::StateStore>(
        std::make_shared<db::StrategyStateStore>(
            database,
            venue + "/" + std::string(config::ToString(cfg.GetMode())) + "/"));
}

Result<std::unique_ptr<strategy::Runner>> CreateRunner(
    const std::string& strategy_name, const config::Configuration& cfg,
    const ComponentsFactory& make_components, const TradeLogFactory& make_log,
    const StateStoreFactory& make_state) {
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
    if (options.risk && cfg.GetMode() == config::Mode::kPaper) {
        options.risk->shorts = cfg.GetPaperOptions().allow_short;
    }
    options.cancel_entries_on_stop = true;
    if (cfg.GetRecording().enabled) {
        auto log = make_log(cfg, strategy_name,
                            std::string(components->venue->Name()));
        if (!log) {
            return WithContext(log.error(), "recording");
        }
        options.trade_log = *log;
        options.pnl_interval = cfg.GetRecording().pnl_interval;
        if (cfg.GetRecording().latency) {
            options.latency_interval = cfg.GetRecording().latency_interval;
        }
    }
    if ((*strategy)->Needs().state) {
        // Scoped by the provider (the paper venue's own name is "paper").
        auto store =
            make_state(cfg, std::string(components->data_source->Name()));
        if (store) {
            options.state_store = *store;
        } else if (cfg.GetMode() == config::Mode::kLive ||
                   store.error().category != ErrorCategory::kNotFound) {
            // Live, its loss caps must survive a restart; and a [database]
            // that is configured but cannot be reached is an error either
            // way, like recording.
            return WithContext(store.error(), "strategy state");
        } else {
            spdlog::warn("strategy state: {}; {} keeps none across restarts",
                         store.error().ToString(), strategy_name);
        }
    }
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
