#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "kestrel/config/credentials.hpp"
#include "kestrel/trading/paper_trading_venue.hpp"
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

// Settings shared by every provider.
struct CommonSettings {
  Mode mode = Mode::kPaper;
  std::vector<types::Symbol> symbols;
  trading::PaperOptions paper;
};

// Base class for per-provider configuration, loaded from a TOML file:
//
//   [general]
//   provider = "kraken"          # schwab | kraken | binanceus
//   mode     = "paper"           # paper | live
//   symbols  = ["BTC/USD"]
//
//   [paper]                      # optional; see PaperOptions
//   initial_balances = { USD = "10000" }
//   maker_fee_bps = 25
//   taker_fee_bps = 40
//   slippage_bps  = 5
class Configuration {
 public:
  virtual ~Configuration() = default;

  virtual Provider GetProvider() const = 0;

  const std::vector<types::Symbol>& GetSymbols() const { return common_.symbols; }
  Mode GetMode() const { return common_.mode; }
  const trading::PaperOptions& GetPaperOptions() const { return common_.paper; }

 protected:
  explicit Configuration(CommonSettings common) : common_(std::move(common)) {}

 private:
  CommonSettings common_;
};

class KrakenConfiguration : public Configuration {
 public:
  static types::Result<KrakenConfiguration> Load(const std::filesystem::path& path);
  static types::Result<KrakenConfiguration> Parse(std::string_view toml_text, const LoadOptions& options);

  Provider GetProvider() const override { return Provider::kKraken; }
  const std::string& RestUrl() const { return rest_url_; }
  const std::string& WsUrl() const { return ws_url_; }
  // Env: KESTREL_KRAKEN_API_KEY / KESTREL_KRAKEN_API_SECRET, or [kraken] in
  // the secrets file. Empty when not configured; only live mode requires it.
  const std::optional<KrakenCredentials>& Credentials() const { return credentials_; }

 private:
  KrakenConfiguration(CommonSettings c, std::string rest, std::string ws, std::optional<KrakenCredentials> cr)
      : Configuration(std::move(c)), rest_url_(std::move(rest)), ws_url_(std::move(ws)),
        credentials_(std::move(cr)) {}
  friend class ConfigParser;
  std::string rest_url_;
  std::string ws_url_;
  std::optional<KrakenCredentials> credentials_;
};

class BinanceUSConfiguration : public Configuration {
 public:
  static types::Result<BinanceUSConfiguration> Load(const std::filesystem::path& path);
  static types::Result<BinanceUSConfiguration> Parse(std::string_view toml_text, const LoadOptions& options);

  Provider GetProvider() const override { return Provider::kBinanceUS; }
  const std::string& RestUrl() const { return rest_url_; }
  const std::string& WsUrl() const { return ws_url_; }
  // Env: KESTREL_BINANCEUS_API_KEY / KESTREL_BINANCEUS_API_SECRET, or
  // [binanceus] in the secrets file.
  const std::optional<BinanceUSCredentials>& Credentials() const { return credentials_; }

 private:
  BinanceUSConfiguration(CommonSettings c, std::string rest, std::string ws, std::optional<BinanceUSCredentials> cr)
      : Configuration(std::move(c)), rest_url_(std::move(rest)), ws_url_(std::move(ws)),
        credentials_(std::move(cr)) {}
  friend class ConfigParser;
  std::string rest_url_;
  std::string ws_url_;
  std::optional<BinanceUSCredentials> credentials_;
};

class SchwabConfiguration : public Configuration {
 public:
  static types::Result<SchwabConfiguration> Load(const std::filesystem::path& path);
  static types::Result<SchwabConfiguration> Parse(std::string_view toml_text, const LoadOptions& options);

  Provider GetProvider() const override { return Provider::kSchwab; }
  const std::string& RedirectUri() const { return redirect_uri_; }
  // Where OAuth tokens are persisted (see SchwabTokenStore). Not a secret
  // itself, but the file it names is, so keep it out of version control.
  const std::filesystem::path& TokenFile() const { return token_file_; }
  // Env: KESTREL_SCHWAB_APP_KEY / KESTREL_SCHWAB_APP_SECRET, or [schwab] in
  // the secrets file.
  const std::optional<SchwabCredentials>& Credentials() const { return credentials_; }

 private:
  SchwabConfiguration(CommonSettings c, std::string redirect, std::filesystem::path token_file,
                      std::optional<SchwabCredentials> cr)
      : Configuration(std::move(c)), redirect_uri_(std::move(redirect)),
        token_file_(std::move(token_file)), credentials_(std::move(cr)) {}
  friend class ConfigParser;
  std::string redirect_uri_;
  std::filesystem::path token_file_;
  std::optional<SchwabCredentials> credentials_;
};

// Reads [general] provider from the file and returns the matching subclass.
types::Result<std::unique_ptr<Configuration>> LoadConfiguration(const std::filesystem::path& path);

}  // namespace kestrel::config
