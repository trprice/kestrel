#include <filesystem>
#include <fstream>

#include "kestrel/config/configuration.hpp"
#include "kestrel/config/schwab_token_store.hpp"
#include "kestrel/factory.hpp"
#include "test_helpers.hpp"

namespace {

using namespace kestrel;
using kestrel::testing::D;

config::LoadOptions NoEnv(std::map<std::string, std::string> env = {}) {
  config::LoadOptions o;
  o.getenv = [env](const std::string& k) -> std::optional<std::string> {
    if (auto it = env.find(k); it != env.end())
    {
      return it->second;
    }
    return std::nullopt;
  };
  o.base_dir = std::filesystem::temp_directory_path() / "kestrel-no-such-dir";
  return o;
}

constexpr const char* kKrakenToml = R"(
[general]
provider = "kraken"
mode = "paper"
symbols = ["btc/usd", "ETH-USD"]

[paper]
initial_balances = { USD = "25000.50", BTC = 2 }
maker_fee_bps = 10
taker_fee_bps = 12.5
slippage_bps = "3"
)";

TEST(Config, KrakenPaperDefaultsAndOverrides) {
  auto cfg = config::KrakenConfiguration::Parse(kKrakenToml, NoEnv());
  ASSERT_OK(cfg);
  EXPECT_EQ(cfg->GetMode(), config::Mode::kPaper);
  EXPECT_EQ(cfg->GetProvider(), config::Provider::kKraken);
  EXPECT_EQ(cfg->GetSymbols(), (std::vector<std::string>{"BTC/USD", "ETH/USD"}));
  const auto& paper = cfg->GetPaperOptions();
  EXPECT_EQ(paper.initial_balances.at("USD"), D("25000.50"));
  EXPECT_EQ(paper.initial_balances.at("BTC"), D("2"));
  EXPECT_EQ(paper.maker_fee_rate, D("0.0010"));
  EXPECT_EQ(paper.taker_fee_rate, D("0.00125"));
  EXPECT_EQ(paper.slippage, D("0.0003"));
  EXPECT_EQ(cfg->RestUrl(), "https://api.kraken.com");
  EXPECT_FALSE(cfg->Credentials().has_value());
}

TEST(Config, LiveModeAndSymbolsValidation) {
  auto live = config::KrakenConfiguration::Parse("[general]\nmode='live'\nsymbols=['BTC/USD']", NoEnv());
  ASSERT_OK(live);
  EXPECT_EQ(live->GetMode(), config::Mode::kLive);

  EXPECT_FALSE(config::KrakenConfiguration::Parse("[general]\nsymbols=[]", NoEnv()).has_value());
  EXPECT_FALSE(config::KrakenConfiguration::Parse("[general]\nmode='yolo'\nsymbols=['A']", NoEnv()).has_value());
  EXPECT_FALSE(config::KrakenConfiguration::Parse("[general]\nsymbols=['not valid!']", NoEnv()).has_value());
  EXPECT_FALSE(config::KrakenConfiguration::Parse("symbols = [", NoEnv()).has_value());
  EXPECT_FALSE(config::KrakenConfiguration::Parse("[general]\nprovider='schwab'\nsymbols=['A']", NoEnv()).has_value());
}

TEST(Config, CredentialsFromEnvironment) {
  auto cfg = config::KrakenConfiguration::Parse(
      kKrakenToml, NoEnv({{"KESTREL_KRAKEN_API_KEY", "key"}, {"KESTREL_KRAKEN_API_SECRET", "c2VjcmV0"}}));
  ASSERT_OK(cfg);
  ASSERT_TRUE(cfg->Credentials().has_value());
  EXPECT_EQ(cfg->Credentials()->api_key, "key");
  EXPECT_EQ(cfg->Credentials()->api_secret, "c2VjcmV0");

  auto half = config::KrakenConfiguration::Parse(kKrakenToml, NoEnv({{"KESTREL_KRAKEN_API_KEY", "key"}}));
  EXPECT_FALSE(half.has_value());
}

TEST(Config, CredentialsFromSecretsFile) {
  const auto dir = std::filesystem::temp_directory_path() / "kestrel-config-test";
  std::filesystem::create_directories(dir);
  {
    std::ofstream(dir / "secrets.toml") << "[binanceus]\napi_key='k'\napi_secret='s'\n";
  }
  auto o = NoEnv();
  o.base_dir = dir;
  auto cfg = config::BinanceUSConfiguration::Parse("[general]\nprovider='binanceus'\nsymbols=['BTC/USD']", o);
  ASSERT_OK(cfg);
  ASSERT_TRUE(cfg->Credentials().has_value());
  EXPECT_EQ(cfg->Credentials()->api_secret, "s");

  // An explicitly named secrets file that is missing is an error, not a silent skip.
  auto missing = config::BinanceUSConfiguration::Parse(
      "[general]\nsymbols=['BTC/USD']\nsecrets_file='nope.toml'", o);
  EXPECT_FALSE(missing.has_value());
  std::filesystem::remove_all(dir);
}

TEST(Config, RefusesCredentialsInTheMainFile) {
  auto cfg = config::KrakenConfiguration::Parse(
      "[general]\nsymbols=['BTC/USD']\n[kraken]\napi_secret='oops'", NoEnv());
  ASSERT_FALSE(cfg.has_value());
  EXPECT_NE(cfg.error().message.find("secrets"), std::string::npos);
  EXPECT_FALSE(config::SchwabConfiguration::Parse(
                   "[general]\nsymbols=['AAPL']\n[schwab]\napp_secret='x'", NoEnv()).has_value());
}

TEST(Config, SchwabSettings) {
  auto cfg = config::SchwabConfiguration::Parse(
      "[general]\nprovider='schwab'\nsymbols=['aapl','MSFT']\n[schwab]\ntoken_file='tok.json'", NoEnv());
  ASSERT_OK(cfg);
  EXPECT_EQ(cfg->GetSymbols().front(), "AAPL");
  EXPECT_EQ(cfg->TokenFile().filename(), "tok.json");
}

TEST(Config, LoadConfigurationDispatchesOnProvider) {
  const auto dir = std::filesystem::temp_directory_path() / "kestrel-config-dispatch";
  std::filesystem::create_directories(dir);
  std::ofstream(dir / "c.toml") << "[general]\nprovider='binanceus'\nsymbols=['BTC/USD']\n";
  auto cfg = config::LoadConfiguration(dir / "c.toml");
  ASSERT_OK(cfg);
  EXPECT_EQ((*cfg)->GetProvider(), config::Provider::kBinanceUS);
  EXPECT_FALSE(config::LoadConfiguration(dir / "missing.toml").has_value());
  std::filesystem::remove_all(dir);
}

TEST(Factory, LiveModeWithoutCredentialsFailsClosed) {
  auto cfg = config::BinanceUSConfiguration::Parse("[general]\nmode='live'\nsymbols=['BTC/USD']", NoEnv());
  ASSERT_OK(cfg);
  auto components = CreateComponents(*cfg);
  ASSERT_FALSE(components.has_value());
  EXPECT_EQ(components.error().category, types::ErrorCategory::kAuth);
}

TEST(Factory, LiveModeBuildsTheProviderVenue) {
  auto cfg = config::BinanceUSConfiguration::Parse(
      "[general]\nmode='live'\nsymbols=['BTC/USD']",
      NoEnv({{"KESTREL_BINANCEUS_API_KEY", "k"}, {"KESTREL_BINANCEUS_API_SECRET", "s"}}));
  ASSERT_OK(cfg);
  auto components = CreateComponents(*cfg);
  ASSERT_OK(components);
  EXPECT_EQ(components->venue->Name(), "binanceus");
  // Stubbed provider: honest about it.
  auto balances = components->venue->GetBalances();
  ASSERT_FALSE(balances.has_value());
  EXPECT_EQ(balances.error().category, types::ErrorCategory::kNotImplemented);
}

TEST(SchwabTokenStore, SaveLoadRoundTripAndPermissions) {
  const auto dir = std::filesystem::temp_directory_path() / "kestrel-token-test";
  std::filesystem::create_directories(dir);
  config::SchwabTokenStore store(dir / "tokens.json");
  EXPECT_EQ(store.Load().error().category, types::ErrorCategory::kNotFound);

  config::SchwabTokens t{"access", "refresh", types::FromEpochSeconds(1'700'000'000),
                         types::FromEpochSeconds(1'700'000'000 + 7 * 86400)};
  ASSERT_OK(store.Save(t));
  auto loaded = store.Load();
  ASSERT_OK(loaded);
  EXPECT_EQ(loaded->refresh_token, "refresh");
  EXPECT_EQ(loaded->refresh_expires_at, t.refresh_expires_at);
  EXPECT_EQ(std::filesystem::status(dir / "tokens.json").permissions() &
                (std::filesystem::perms::group_all | std::filesystem::perms::others_all),
            std::filesystem::perms::none);
  std::filesystem::remove_all(dir);
}

TEST(SchwabTokenStore, HealthClassification) {
  using config::SchwabTokenHealth;
  const auto issued = types::FromEpochSeconds(1'700'000'000);
  config::SchwabTokens t{"a", "r", issued + std::chrono::minutes(30), issued + std::chrono::hours(24 * 7)};
  EXPECT_EQ(config::SchwabTokenStore::Check(t, issued), SchwabTokenHealth::kValid);
  EXPECT_EQ(config::SchwabTokenStore::Check(t, issued + std::chrono::hours(1)), SchwabTokenHealth::kAccessExpired);
  EXPECT_EQ(config::SchwabTokenStore::Check(t, issued + std::chrono::hours(24 * 7 - 2)),
            SchwabTokenHealth::kAccessExpired);  // warns, still refreshable
  EXPECT_EQ(config::SchwabTokenStore::Check(t, issued + std::chrono::hours(24 * 7)),
            SchwabTokenHealth::kReauthorizationRequired);
}

}  // namespace
