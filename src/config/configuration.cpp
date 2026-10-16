#include "kestrel/config/configuration.hpp"

#include <spdlog/spdlog.h>
#include <sys/stat.h>

#include <charconv>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include <toml++/toml.hpp>

namespace kestrel::config {

using types::Decimal;
using types::ErrorCategory;
using types::MakeError;
using types::Result;

std::string_view ToString(Mode m) { return m == Mode::kPaper ? "paper" : "live"; }

std::string_view ToString(Provider p) {
  switch (p) {
    case Provider::kSchwab: return "schwab";
    case Provider::kKraken: return "kraken";
    case Provider::kBinanceUS: return "binanceus";
  }
  return "unknown";
}

namespace {

Result<std::string> ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path);
  if (!in) return MakeError(ErrorCategory::kNotFound, "cannot open " + path.string());
  std::stringstream buf;
  buf << in.rdbuf();
  return buf.str();
}

}  // namespace

class ConfigParser {
 public:
  static Result<toml::table> ParseToml(std::string_view text, std::string_view what) {
    try {
      return toml::parse(text);
    } catch (const toml::parse_error& e) {
      return MakeError(ErrorCategory::kParse, std::string("invalid TOML in ") + std::string(what) + ": " +
                                                  std::string(e.description()));
    }
  }

  // A numeric TOML value -> Decimal, without letting a double near prices.
  // Integers and strings are exact; floats (used only for rates like 12.5) go
  // through their shortest round-trip text.
  static Result<Decimal> NumberNode(const toml::node& node, const std::string& name) {
    if (const auto i = node.value<std::int64_t>(); i && node.is_integer()) return Decimal(*i, 0);
    if (node.is_string()) return Decimal::Parse(node.value<std::string>().value());
    if (const auto f = node.as_floating_point()) {
      char buf[64];
      const auto [end, ec] = std::to_chars(buf, buf + sizeof buf, f->get());
      if (ec == std::errc()) return Decimal::Parse(std::string_view(buf, end));
    }
    return MakeError(ErrorCategory::kInvalidArgument, name + " must be a number or a numeric string");
  }

  static Result<Decimal> Bps(const toml::table& t, const char* key, const Decimal& fallback) {
    const auto* node = t.get(key);
    if (!node) return fallback;
    auto bps = NumberNode(*node, key);
    if (!bps) return std::unexpected(bps.error());
    if (bps->IsNegative()) return MakeError(ErrorCategory::kInvalidArgument, std::string(key) + " cannot be negative");
    return *bps * Decimal(1, 4);
  }

  static Result<CommonSettings> Common(const toml::table& root, Provider expected) {
    CommonSettings c;
    const auto* general = root["general"].as_table();
    if (!general) return MakeError(ErrorCategory::kInvalidArgument, "config is missing the [general] table");

    if (const auto provider = general->get("provider")) {
      const auto name = provider->value<std::string>();
      if (!name || *name != ToString(expected)) {
        return MakeError(ErrorCategory::kInvalidArgument, "[general] provider is '" + name.value_or("?") +
                                                              "' but this loader expects '" +
                                                              std::string(ToString(expected)) + "'");
      }
    }
    if (const auto mode = general->get("mode")) {
      const auto m = mode->value<std::string>();
      if (m == "paper") c.mode = Mode::kPaper;
      else if (m == "live") c.mode = Mode::kLive;
      else return MakeError(ErrorCategory::kInvalidArgument, "[general] mode must be \"paper\" or \"live\"");
    }

    const auto* symbols = general->get_as<toml::array>("symbols");
    if (!symbols || symbols->empty()) {
      return MakeError(ErrorCategory::kInvalidArgument, "[general] symbols must list at least one symbol");
    }
    for (const auto& s : *symbols) {
      const auto text = s.value<std::string>();
      if (!text) return MakeError(ErrorCategory::kInvalidArgument, "[general] symbols must be strings");
      auto sym = types::NormalizeSymbol(*text);
      if (!sym) return std::unexpected(sym.error());
      c.symbols.push_back(*sym);
    }

    if (const auto* paper = root["paper"].as_table()) {
      auto& p = c.paper;
      if (const auto* balances = paper->get_as<toml::table>("initial_balances")) {
        p.initial_balances.clear();
        for (const auto& [asset, node] : *balances) {
          auto amount = NumberNode(node, "initial_balances." + std::string(asset.str()));
          if (!amount) return std::unexpected(amount.error());
          if (amount->IsNegative()) {
            return MakeError(ErrorCategory::kInvalidArgument, "initial balance cannot be negative");
          }
          p.initial_balances[std::string(asset.str())] = *amount;
        }
      }
      auto maker = Bps(*paper, "maker_fee_bps", p.maker_fee_rate);
      auto taker = Bps(*paper, "taker_fee_bps", p.taker_fee_rate);
      auto slip = Bps(*paper, "slippage_bps", p.slippage);
      if (!maker) return std::unexpected(maker.error());
      if (!taker) return std::unexpected(taker.error());
      if (!slip) return std::unexpected(slip.error());
      p.maker_fee_rate = *maker;
      p.taker_fee_rate = *taker;
      p.slippage = *slip;
      if (const auto v = paper->get("default_quote_asset")) p.default_quote_asset = v->value_or(p.default_quote_asset);
      if (const auto v = paper->get("cap_limit_fills_at_quote_size")) {
        p.cap_limit_fills_at_quote_size = v->value_or(false);
      }
    }
    return c;
  }

  // Credentials in the main file would end up in version control and logs.
  static Result<void> RejectSecrets(const toml::table* provider_table, std::string_view section) {
    if (!provider_table) return {};
    for (const char* key : {"api_key", "api_secret", "app_key", "app_secret", "secret", "password", "token",
                            "access_token", "refresh_token"}) {
      if (provider_table->contains(key)) {
        return MakeError(ErrorCategory::kInvalidArgument,
                         "[" + std::string(section) + "] contains '" + key +
                             "': credentials must come from environment variables or the secrets file, "
                             "never the main config");
      }
    }
    return {};
  }

  static LoadOptions WithDefaults(LoadOptions o) {
    if (!o.getenv) {
      o.getenv = [](const std::string& name) -> std::optional<std::string> {
        const char* v = std::getenv(name.c_str());
        if (v && *v) return std::string(v);
        return std::nullopt;
      };
    }
    if (o.base_dir.empty()) o.base_dir = ".";
    return o;
  }

  // The secrets file: KESTREL_SECRETS_FILE, else [general] secrets_file, else
  // <base_dir>/secrets.toml if present.
  static Result<std::optional<toml::table>> Secrets(const toml::table& root, const LoadOptions& o) {
    std::filesystem::path path;
    bool explicit_path = false;
    if (const auto env = o.getenv("KESTREL_SECRETS_FILE")) {
      path = *env;
      explicit_path = true;
    } else if (const auto configured = root["general"]["secrets_file"].value<std::string>()) {
      path = *configured;
      explicit_path = true;
    } else {
      path = "secrets.toml";
    }
    if (path.is_relative()) path = o.base_dir / path;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
      if (explicit_path) return MakeError(ErrorCategory::kNotFound, "secrets file " + path.string() + " does not exist");
      return std::optional<toml::table>{};
    }
    struct stat st{};
    if (::stat(path.c_str(), &st) == 0 && (st.st_mode & (S_IRWXG | S_IRWXO))) {
      spdlog::warn("secrets file {} is accessible to other users; run: chmod 600 {}", path.string(), path.string());
    }
    auto text = ReadFile(path);
    if (!text) return std::unexpected(text.error());
    auto table = ParseToml(*text, path.string());
    if (!table) return std::unexpected(table.error());
    return std::optional<toml::table>(std::move(*table));
  }

  // Environment variable first, then the secrets file.
  static std::optional<std::string> Secret(const LoadOptions& o, const std::optional<toml::table>& secrets,
                                           const char* section, const char* field, const std::string& env_name) {
    if (const auto env = o.getenv(env_name)) return env;
    if (secrets) {
      if (const auto v = (*secrets)[section][field].value<std::string>(); v && !v->empty()) return v;
    }
    return std::nullopt;
  }

  template <typename Cred, typename Make>
  static Result<std::optional<Cred>> Credentials(const toml::table& root, const LoadOptions& o,
                                                 const char* section, const char* upper, const char* f1,
                                                 const char* f2, Make make) {
    auto secrets = Secrets(root, o);
    if (!secrets) return std::unexpected(secrets.error());
    const auto a = Secret(o, *secrets, section, f1, std::string("KESTREL_") + upper + "_" + ToUpper(f1));
    const auto b = Secret(o, *secrets, section, f2, std::string("KESTREL_") + upper + "_" + ToUpper(f2));
    if (a && b) return std::optional<Cred>(make(*a, *b));
    if (a || b) {
      return MakeError(ErrorCategory::kInvalidArgument,
                       std::string("incomplete ") + section + " credentials: need both " + f1 + " and " + f2);
    }
    return std::optional<Cred>{};
  }

  static std::string ToUpper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
  }

  static std::string Str(const toml::table* t, const char* key, std::string fallback) {
    if (t) {
      if (const auto v = t->get(key)) return v->value_or(fallback);
    }
    return fallback;
  }

  static LoadOptions ForFile(const std::filesystem::path& path) {
    LoadOptions o;
    o.base_dir = path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
    return o;
  }
};

Result<KrakenConfiguration> KrakenConfiguration::Parse(std::string_view text, const LoadOptions& raw) {
  const auto options = ConfigParser::WithDefaults(raw);
  auto root = ConfigParser::ParseToml(text, "config");
  if (!root) return std::unexpected(root.error());
  auto common = ConfigParser::Common(*root, Provider::kKraken);
  if (!common) return std::unexpected(common.error());
  const auto* section = (*root)["kraken"].as_table();
  if (auto ok = ConfigParser::RejectSecrets(section, "kraken"); !ok) return std::unexpected(ok.error());
  auto creds = ConfigParser::Credentials<KrakenCredentials>(
      *root, options, "kraken", "KRAKEN", "api_key", "api_secret",
      [](std::string k, std::string s) { return KrakenCredentials{std::move(k), std::move(s)}; });
  if (!creds) return std::unexpected(creds.error());
  return KrakenConfiguration(std::move(*common),
                             ConfigParser::Str(section, "rest_url", "https://api.kraken.com"),
                             ConfigParser::Str(section, "ws_url", "wss://ws.kraken.com/v2"), std::move(*creds));
}

Result<KrakenConfiguration> KrakenConfiguration::Load(const std::filesystem::path& path) {
  auto text = ReadFile(path);
  if (!text) return std::unexpected(text.error());
  return Parse(*text, ConfigParser::ForFile(path));
}

Result<BinanceUSConfiguration> BinanceUSConfiguration::Parse(std::string_view text, const LoadOptions& raw) {
  const auto options = ConfigParser::WithDefaults(raw);
  auto root = ConfigParser::ParseToml(text, "config");
  if (!root) return std::unexpected(root.error());
  auto common = ConfigParser::Common(*root, Provider::kBinanceUS);
  if (!common) return std::unexpected(common.error());
  const auto* section = (*root)["binanceus"].as_table();
  if (auto ok = ConfigParser::RejectSecrets(section, "binanceus"); !ok) return std::unexpected(ok.error());
  auto creds = ConfigParser::Credentials<BinanceUSCredentials>(
      *root, options, "binanceus", "BINANCEUS", "api_key", "api_secret",
      [](std::string k, std::string s) { return BinanceUSCredentials{std::move(k), std::move(s)}; });
  if (!creds) return std::unexpected(creds.error());
  return BinanceUSConfiguration(std::move(*common),
                                ConfigParser::Str(section, "rest_url", "https://api.binance.us"),
                                ConfigParser::Str(section, "ws_url", "wss://stream.binance.us:9443"),
                                std::move(*creds));
}

Result<BinanceUSConfiguration> BinanceUSConfiguration::Load(const std::filesystem::path& path) {
  auto text = ReadFile(path);
  if (!text) return std::unexpected(text.error());
  return Parse(*text, ConfigParser::ForFile(path));
}

Result<SchwabConfiguration> SchwabConfiguration::Parse(std::string_view text, const LoadOptions& raw) {
  const auto options = ConfigParser::WithDefaults(raw);
  auto root = ConfigParser::ParseToml(text, "config");
  if (!root) return std::unexpected(root.error());
  auto common = ConfigParser::Common(*root, Provider::kSchwab);
  if (!common) return std::unexpected(common.error());
  const auto* section = (*root)["schwab"].as_table();
  if (auto ok = ConfigParser::RejectSecrets(section, "schwab"); !ok) return std::unexpected(ok.error());
  auto creds = ConfigParser::Credentials<SchwabCredentials>(
      *root, options, "schwab", "SCHWAB", "app_key", "app_secret",
      [](std::string k, std::string s) { return SchwabCredentials{std::move(k), std::move(s)}; });
  if (!creds) return std::unexpected(creds.error());
  std::filesystem::path token_file = ConfigParser::Str(section, "token_file", "schwab_tokens.json");
  if (token_file.is_relative()) token_file = options.base_dir / token_file;
  return SchwabConfiguration(std::move(*common),
                             ConfigParser::Str(section, "redirect_uri", "https://127.0.0.1:8182"),
                             std::move(token_file), std::move(*creds));
}

Result<SchwabConfiguration> SchwabConfiguration::Load(const std::filesystem::path& path) {
  auto text = ReadFile(path);
  if (!text) return std::unexpected(text.error());
  return Parse(*text, ConfigParser::ForFile(path));
}

Result<std::unique_ptr<Configuration>> LoadConfiguration(const std::filesystem::path& path) {
  auto text = ReadFile(path);
  if (!text) return std::unexpected(text.error());
  auto root = ConfigParser::ParseToml(*text, path.string());
  if (!root) return std::unexpected(root.error());
  const auto provider = (*root)["general"]["provider"].value<std::string>();
  if (!provider) return MakeError(ErrorCategory::kInvalidArgument, "[general] provider is required");

  const auto options = ConfigParser::ForFile(path);
  auto wrap = [](auto result) -> Result<std::unique_ptr<Configuration>> {
    if (!result) return std::unexpected(result.error());
    return std::make_unique<std::remove_cvref_t<decltype(*result)>>(std::move(*result));
  };
  if (*provider == "kraken") return wrap(KrakenConfiguration::Parse(*text, options));
  if (*provider == "binanceus") return wrap(BinanceUSConfiguration::Parse(*text, options));
  if (*provider == "schwab") return wrap(SchwabConfiguration::Parse(*text, options));
  return MakeError(ErrorCategory::kInvalidArgument,
                   "unknown provider '" + *provider + "' (expected schwab, kraken or binanceus)");
}

}  // namespace kestrel::config
