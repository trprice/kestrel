#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "kestrel/config/credentials.hpp"
#include "kestrel/db/database.hpp"
#include "kestrel/trading/paper_trading_venue.hpp"
#include "kestrel/trading/risk_gate.hpp"
#include "kestrel/types/error.hpp"
#include "kestrel/types/symbol.hpp"

namespace kestrel::config {

enum class Mode { kPaper, kLive };
enum class Provider { kSchwab, kKraken, kBinanceUS };

std::string_view ToString(Mode);
std::string_view ToString(Provider);

// Where secrets may come from, injectable so tests don't touch the real
// environment or filesystem.
struct LoadOptions {
    // Returns the value of an environment variable, if set.
    std::function<std::optional<std::string>(const std::string&)> getenv;
    // Directory that relative paths in the config (secrets_file, token_file)
    // resolve against. Defaults to the config file's directory.
    std::filesystem::path base_dir;
};

// The [strategy] table: parameters for whichever strategy --run or --backtest
// names. Values are kept as text; the strategy registry (see factory.hpp)
// parses and validates them.
using StrategyParams = std::map<std::string, std::string>;

// The [recording] table: whether `run` stores its fills and P&L in the
// database (see db::TradeStore). Needs a [database].
struct RecordingOptions {
    bool enabled = false;
    // How often a P&L snapshot is written; zero writes only the last one.
    std::chrono::milliseconds pnl_interval{60'000};
};

// The [http] table: how the HTTP calls to the venue are watched.
struct HttpOptions {
    // A call taking this long or longer is logged as a warning.
    std::chrono::milliseconds slow_request{2000};
};

// Settings shared by every provider.
struct CommonSettings {
    Mode mode = Mode::kPaper;
    std::vector<types::Symbol> symbols;
    trading::PaperOptions paper;
    std::optional<db::ConnectionOptions> database;  // absent: no [database]
    std::optional<trading::RiskLimits> risk;        // absent: no [risk]
    // [risk.<strategy>]: limits that replace the [risk] ones key by key.
    std::map<std::string, trading::RiskLimits> strategy_risk;
    StrategyParams strategy;  // empty: no [strategy]
    RecordingOptions recording;
    HttpOptions http;
};

// Base class for per-provider configuration, loaded from a TOML file:
//
//   [general]
//   provider = "kraken"          # schwab | kraken | binanceus
//   mode     = "paper"           # paper | live
//   symbols  = ["BTC/USD"]
//
//   [database]                   # optional; PostgreSQL + TimescaleDB
//   host = "localhost"           # omit for the local socket
//   port = 5432
//   dbname = "kestrel"
//   user = "kestrel"
//   sslmode = "prefer"
//   # password: KESTREL_DATABASE_PASSWORD or [database] password in the
//   # secrets file (or libpq's own PGPASSWORD / ~/.pgpass when unset)
//
//   [paper]                      # optional; see PaperOptions
//   initial_balances = { USD = "10000" }
//   maker_fee_bps = 25
//   taker_fee_bps = 40
//   slippage_bps  = 5
//   limit_fill_model = "cross"   # or "conservative"
//
//   [risk]                       # optional; see RiskLimits. Required for live
//                                # runs. A [risk] table with no limits is
//                                # treated as absent.
//   max_order_notional    = "1000"
//   max_position_notional = "5000"
//   max_open_orders       = 5
//   max_orders_per_minute = 10
//   max_daily_loss        = "250"
//   allowed_symbols       = ["BTC/USD"]
//   max_price_age_ms      = 5000
//   max_orders_per_day    = 20
//   max_consecutive_losses = 3
//   max_drawdown          = "400"
//
//   [risk.sma_crossover]         # optional per-strategy overrides: each key
//   max_orders_per_day = 6       # given here replaces the [risk] value for
//                                # that strategy only (allowed_symbols
//                                # replaces the whole list)
//
//   [recording]                  # optional; `run` stores its fills and P&L
//   enabled = true               # in the [database] (default false)
//   pnl_interval_ms = 60000      # snapshot period; 0: only the last one
//
//   [http]                       # optional
//   slow_request_ms = 2000       # warn about calls at least this slow
//
//   [strategy]                   # optional; parameters, meaning depends on
//   symbol = "BTC/USD"           # the strategy named on the command line
//   ...
class Configuration {
   public:
    virtual ~Configuration() = default;

    virtual Provider GetProvider() const = 0;

    const std::vector<types::Symbol>& GetSymbols() const {
        return mCommon.symbols;
    }
    Mode GetMode() const { return mCommon.mode; }
    const std::optional<db::ConnectionOptions>& GetDatabase() const {
        return mCommon.database;
    }
    // The [risk] limits that apply to every strategy.
    const std::optional<trading::RiskLimits>& GetRiskLimits() const {
        return mCommon.risk;
    }
    // [risk.<strategy>] tables by strategy name.
    const std::map<std::string, trading::RiskLimits>& GetRiskOverrides() const {
        return mCommon.strategy_risk;
    }
    // The limits for one strategy: [risk] with that strategy's overrides laid
    // over it. Absent when neither sets a limit.
    std::optional<trading::RiskLimits> GetRiskLimits(
        const std::string& strategy) const;
    const trading::PaperOptions& GetPaperOptions() const {
        return mCommon.paper;
    }
    const StrategyParams& GetStrategyParams() const { return mCommon.strategy; }
    const RecordingOptions& GetRecording() const { return mCommon.recording; }
    const HttpOptions& GetHttp() const { return mCommon.http; }

   protected:
    explicit Configuration(CommonSettings common)
        : mCommon(std::move(common)) {}

   private:
    CommonSettings mCommon;
};

class KrakenConfiguration : public Configuration {
   public:
    static types::Result<KrakenConfiguration> Load(
        const std::filesystem::path& path);
    static types::Result<KrakenConfiguration> Parse(std::string_view toml_text,
                                                    const LoadOptions& options);

    Provider GetProvider() const override { return Provider::kKraken; }
    const std::string& RestUrl() const { return mRestUrl; }
    const std::string& WsUrl() const { return mWsUrl; }
    // Env: KESTREL_KRAKEN_API_KEY / KESTREL_KRAKEN_API_SECRET, or [kraken] in
    // the secrets file. Empty when not configured; only live mode requires it.
    const std::optional<KrakenCredentials>& Credentials() const {
        return mCredentials;
    }

   private:
    KrakenConfiguration(CommonSettings c, std::string rest, std::string ws,
                        std::optional<KrakenCredentials> cr)
        : Configuration(std::move(c)),
          mRestUrl(std::move(rest)),
          mWsUrl(std::move(ws)),
          mCredentials(std::move(cr)) {}
    friend class ConfigParser;
    std::string mRestUrl;
    std::string mWsUrl;
    std::optional<KrakenCredentials> mCredentials;
};

class BinanceUSConfiguration : public Configuration {
   public:
    static types::Result<BinanceUSConfiguration> Load(
        const std::filesystem::path& path);
    static types::Result<BinanceUSConfiguration> Parse(
        std::string_view toml_text, const LoadOptions& options);

    Provider GetProvider() const override { return Provider::kBinanceUS; }
    const std::string& RestUrl() const { return mRestUrl; }
    const std::string& WsUrl() const { return mWsUrl; }
    // Env: KESTREL_BINANCEUS_API_KEY / KESTREL_BINANCEUS_API_SECRET, or
    // [binanceus] in the secrets file.
    const std::optional<BinanceUSCredentials>& Credentials() const {
        return mCredentials;
    }

   private:
    BinanceUSConfiguration(CommonSettings c, std::string rest, std::string ws,
                           std::optional<BinanceUSCredentials> cr)
        : Configuration(std::move(c)),
          mRestUrl(std::move(rest)),
          mWsUrl(std::move(ws)),
          mCredentials(std::move(cr)) {}
    friend class ConfigParser;
    std::string mRestUrl;
    std::string mWsUrl;
    std::optional<BinanceUSCredentials> mCredentials;
};

class SchwabConfiguration : public Configuration {
   public:
    static types::Result<SchwabConfiguration> Load(
        const std::filesystem::path& path);
    static types::Result<SchwabConfiguration> Parse(std::string_view toml_text,
                                                    const LoadOptions& options);

    Provider GetProvider() const override { return Provider::kSchwab; }
    const std::string& RedirectUri() const { return mRedirectUri; }
    // Where OAuth tokens are persisted (see SchwabTokenStore). Not a secret
    // itself, but the file it names is, so keep it out of version control.
    const std::filesystem::path& TokenFile() const { return mTokenFile; }
    // Env: KESTREL_SCHWAB_APP_KEY / KESTREL_SCHWAB_APP_SECRET, or [schwab] in
    // the secrets file.
    const std::optional<SchwabCredentials>& Credentials() const {
        return mCredentials;
    }

   private:
    SchwabConfiguration(CommonSettings c, std::string redirect,
                        std::filesystem::path token_file,
                        std::optional<SchwabCredentials> cr)
        : Configuration(std::move(c)),
          mRedirectUri(std::move(redirect)),
          mTokenFile(std::move(token_file)),
          mCredentials(std::move(cr)) {}
    friend class ConfigParser;
    std::string mRedirectUri;
    std::filesystem::path mTokenFile;
    std::optional<SchwabCredentials> mCredentials;
};

// Reads [general] provider from the file and returns the matching subclass.
types::Result<std::unique_ptr<Configuration>> LoadConfiguration(
    const std::filesystem::path& path);

}  // namespace kestrel::config
