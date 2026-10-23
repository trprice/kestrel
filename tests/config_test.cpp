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
        if (auto it = env.find(k); it != env.end()) {
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

TEST(Config, DatabaseSectionIsOptionalAndTakesPasswordFromEnv) {
    auto none = config::KrakenConfiguration::Parse(kKrakenToml, NoEnv());
    ASSERT_OK(none);
    EXPECT_FALSE(none->GetDatabase().has_value());

    const std::string toml = std::string(kKrakenToml) +
                             "\n[database]\nhost = \"db.local\"\nport = 5433\n"
                             "dbname = \"bars\"\nuser = \"kes\"\n";
    auto cfg = config::KrakenConfiguration::Parse(
        toml, NoEnv({{"KESTREL_DATABASE_PASSWORD", "hunter2"}}));
    ASSERT_OK(cfg);
    ASSERT_TRUE(cfg->GetDatabase().has_value());
    EXPECT_EQ(cfg->GetDatabase()->host, "db.local");
    EXPECT_EQ(cfg->GetDatabase()->port, 5433);
    EXPECT_EQ(cfg->GetDatabase()->dbname, "bars");
    EXPECT_EQ(cfg->GetDatabase()->user, "kes");
    EXPECT_EQ(cfg->GetDatabase()->password, "hunter2");
}

TEST(Config, DatabasePasswordInMainConfigIsRejected) {
    const std::string toml =
        std::string(kKrakenToml) + "\n[database]\npassword = \"x\"\n";
    EXPECT_FALSE(config::KrakenConfiguration::Parse(toml, NoEnv()).has_value());
}

TEST(Config, KrakenPaperDefaultsAndOverrides) {
    auto cfg = config::KrakenConfiguration::Parse(kKrakenToml, NoEnv());
    ASSERT_OK(cfg);
    EXPECT_EQ(cfg->GetMode(), config::Mode::kPaper);
    EXPECT_EQ(cfg->GetProvider(), config::Provider::kKraken);
    EXPECT_EQ(cfg->GetSymbols(),
              (std::vector<std::string>{"BTC/USD", "ETH/USD"}));
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
    auto live = config::KrakenConfiguration::Parse(
        "[general]\nmode='live'\nsymbols=['BTC/USD']", NoEnv());
    ASSERT_OK(live);
    EXPECT_EQ(live->GetMode(), config::Mode::kLive);

    EXPECT_FALSE(
        config::KrakenConfiguration::Parse("[general]\nsymbols=[]", NoEnv())
            .has_value());
    EXPECT_FALSE(config::KrakenConfiguration::Parse(
                     "[general]\nmode='yolo'\nsymbols=['A']", NoEnv())
                     .has_value());
    EXPECT_FALSE(config::KrakenConfiguration::Parse(
                     "[general]\nsymbols=['not valid!']", NoEnv())
                     .has_value());
    EXPECT_FALSE(
        config::KrakenConfiguration::Parse("symbols = [", NoEnv()).has_value());
    EXPECT_FALSE(config::KrakenConfiguration::Parse(
                     "[general]\nprovider='schwab'\nsymbols=['A']", NoEnv())
                     .has_value());
}

TEST(Config, CredentialsFromEnvironment) {
    auto cfg = config::KrakenConfiguration::Parse(
        kKrakenToml, NoEnv({{"KESTREL_KRAKEN_API_KEY", "key"},
                            {"KESTREL_KRAKEN_API_SECRET", "c2VjcmV0"}}));
    ASSERT_OK(cfg);
    ASSERT_TRUE(cfg->Credentials().has_value());
    EXPECT_EQ(cfg->Credentials()->api_key, "key");
    EXPECT_EQ(cfg->Credentials()->api_secret, "c2VjcmV0");

    auto half = config::KrakenConfiguration::Parse(
        kKrakenToml, NoEnv({{"KESTREL_KRAKEN_API_KEY", "key"}}));
    EXPECT_FALSE(half.has_value());
}

TEST(Config, CredentialsFromSecretsFile) {
    const auto dir =
        std::filesystem::temp_directory_path() / "kestrel-config-test";
    std::filesystem::create_directories(dir);
    {
        std::ofstream(dir / "secrets.toml")
            << "[binanceus]\napi_key='k'\napi_secret='s'\n";
    }
    auto o = NoEnv();
    o.base_dir = dir;
    auto cfg = config::BinanceUSConfiguration::Parse(
        "[general]\nprovider='binanceus'\nsymbols=['BTC/USD']", o);
    ASSERT_OK(cfg);
    ASSERT_TRUE(cfg->Credentials().has_value());
    EXPECT_EQ(cfg->Credentials()->api_secret, "s");

    // An explicitly named secrets file that is missing is an error, not a
    // silent skip.
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
    EXPECT_FALSE(
        config::SchwabConfiguration::Parse(
            "[general]\nsymbols=['AAPL']\n[schwab]\napp_secret='x'", NoEnv())
            .has_value());
}

TEST(Config, SchwabSettings) {
    auto cfg = config::SchwabConfiguration::Parse(
        "[general]\nprovider='schwab'\nsymbols=['aapl','MSFT']\n[schwab]"
        "\ntoken_file='tok.json'",
        NoEnv());
    ASSERT_OK(cfg);
    EXPECT_EQ(cfg->GetSymbols().front(), "AAPL");
    EXPECT_EQ(cfg->TokenFile().filename(), "tok.json");
}

TEST(Config, LoadConfigurationDispatchesOnProvider) {
    const auto dir =
        std::filesystem::temp_directory_path() / "kestrel-config-dispatch";
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "c.toml")
        << "[general]\nprovider='binanceus'\nsymbols=['BTC/USD']\n";
    auto cfg = config::LoadConfiguration(dir / "c.toml");
    ASSERT_OK(cfg);
    EXPECT_EQ((*cfg)->GetProvider(), config::Provider::kBinanceUS);
    EXPECT_FALSE(config::LoadConfiguration(dir / "missing.toml").has_value());
    std::filesystem::remove_all(dir);
}

TEST(Factory, LiveModeWithoutCredentialsFailsClosed) {
    auto cfg = config::BinanceUSConfiguration::Parse(
        "[general]\nmode='live'\nsymbols=['BTC/USD']", NoEnv());
    ASSERT_OK(cfg);
    auto components = CreateComponents(*cfg);
    ASSERT_FALSE(components.has_value());
    EXPECT_EQ(components.error().category, types::ErrorCategory::kAuth);
}

TEST(Factory, LiveModeBuildsTheProviderVenue) {
    auto cfg = config::BinanceUSConfiguration::Parse(
        "[general]\nmode='live'\nsymbols=['BTC/USD']",
        NoEnv({{"KESTREL_BINANCEUS_API_KEY", "k"},
               {"KESTREL_BINANCEUS_API_SECRET", "s"}}));
    ASSERT_OK(cfg);
    auto components = CreateComponents(*cfg);
    ASSERT_OK(components);
    EXPECT_EQ(components->venue->Name(), "binanceus");
    // Stubbed provider: honest about it.
    auto balances = components->venue->GetBalances();
    ASSERT_FALSE(balances.has_value());
    EXPECT_EQ(balances.error().category, types::ErrorCategory::kNotImplemented);
}

std::string WithTables(const std::string& tables, const char* mode = "paper") {
    return std::string("[general]\nmode='") + mode +
           "'\nsymbols=['BTC/USD','ETH/USD']\n" + tables;
}

TEST(Config, RiskSectionIsParsedIntoLimits) {
    auto cfg = config::BinanceUSConfiguration::Parse(
        WithTables("[risk]\nmax_order_notional='1000'\n"
                   "max_position_notional=5000\nmax_open_orders=3\n"
                   "max_orders_per_minute=10\nmax_daily_loss=250.5\n"
                   "allowed_symbols=['btc-usd']\nmax_price_age_ms=5000\n"),
        NoEnv());
    ASSERT_OK(cfg);
    ASSERT_TRUE(cfg->GetRiskLimits().has_value());
    const auto& r = *cfg->GetRiskLimits();
    EXPECT_EQ(r.max_order_notional, D("1000"));
    EXPECT_EQ(r.max_position_notional, D("5000"));
    EXPECT_EQ(r.max_open_orders, 3u);
    EXPECT_EQ(r.max_orders_per_minute, 10u);
    EXPECT_EQ(r.max_daily_loss, D("250.5"));
    EXPECT_EQ(r.allowed_symbols, std::set<types::Symbol>{"BTC/USD"});
    EXPECT_EQ(r.max_price_age, std::chrono::milliseconds(5000));
}

TEST(Config, RiskSectionRejectsTyposAndBadValues) {
    for (const char* bad :
         {"[risk]\nmax_order_notinal='1'\n",
          "[risk]\nmax_order_notional='-1'\n", "[risk]\nmax_order_notional=0\n",
          "[risk]\nmax_open_orders=0\n", "[risk]\nmax_open_orders='3'\n",
          "[risk]\nallowed_symbols='BTC/USD'\n"}) {
        EXPECT_FALSE(
            config::BinanceUSConfiguration::Parse(WithTables(bad), NoEnv())
                .has_value())
            << bad;
    }
}

TEST(Config, RiskSectionParsesTheStrategyLimits) {
    auto cfg = config::BinanceUSConfiguration::Parse(
        WithTables("[risk]\nmax_orders_per_day=20\n"
                   "max_consecutive_losses=3\nmax_drawdown='400'\n"),
        NoEnv());
    ASSERT_OK(cfg);
    const auto& r = *cfg->GetRiskLimits();
    EXPECT_EQ(r.max_orders_per_day, 20u);
    EXPECT_EQ(r.max_consecutive_losses, 3u);
    EXPECT_EQ(r.max_drawdown, D("400"));
}

TEST(Config, PerStrategyRiskOverridesTheGlobalLimitsKeyByKey) {
    auto cfg = config::BinanceUSConfiguration::Parse(
        WithTables("[risk]\nmax_order_notional='1000'\nmax_open_orders=5\n"
                   "allowed_symbols=['BTC/USD','ETH/USD']\n"
                   "[risk.sma_crossover]\nmax_open_orders=1\n"
                   "max_orders_per_day=6\nallowed_symbols=['BTC/USD']\n"),
        NoEnv());
    ASSERT_OK(cfg);
    // The global limits are untouched.
    EXPECT_EQ(cfg->GetRiskLimits()->max_open_orders, 5u);
    EXPECT_FALSE(cfg->GetRiskLimits()->max_orders_per_day.has_value());

    const auto sma = cfg->GetRiskLimits("sma_crossover");
    ASSERT_TRUE(sma.has_value());
    EXPECT_EQ(sma->max_order_notional, D("1000"));  // inherited
    EXPECT_EQ(sma->max_open_orders, 1u);            // overridden
    EXPECT_EQ(sma->max_orders_per_day, 6u);         // added
    EXPECT_EQ(sma->allowed_symbols, std::set<types::Symbol>{"BTC/USD"});

    // A strategy without a section gets the global limits.
    EXPECT_EQ(cfg->GetRiskLimits("other")->max_open_orders, 5u);
}

TEST(Config, PerStrategyRiskWorksWithoutAGlobalSection) {
    auto cfg = config::BinanceUSConfiguration::Parse(
        WithTables("[risk.sma_crossover]\nmax_open_orders=2\n"), NoEnv());
    ASSERT_OK(cfg);
    EXPECT_FALSE(cfg->GetRiskLimits().has_value());
    EXPECT_EQ(cfg->GetRiskLimits("sma_crossover")->max_open_orders, 2u);
    EXPECT_FALSE(cfg->GetRiskLimits("other").has_value());
}

TEST(Config, PerStrategyRiskIsCheckedLikeTheGlobalSection) {
    for (const char* bad :
         {"[risk.sma_crossover]\nmax_open_ordrs=1\n",
          "[risk.sma_crossover]\nmax_open_orders=0\n",
          "[risk.sma_crossover.deeper]\nmax_open_orders=1\n"}) {
        auto cfg =
            config::BinanceUSConfiguration::Parse(WithTables(bad), NoEnv());
        EXPECT_FALSE(cfg.has_value()) << bad;
    }
}

TEST(Factory, ResolveRiskLimitsRefusesAnUnknownStrategySection) {
    auto cfg = config::BinanceUSConfiguration::Parse(
        WithTables("[risk]\nmax_open_orders=5\n"
                   "[risk.sma_crosover]\nmax_open_orders=1\n"),
        NoEnv());
    ASSERT_OK(cfg);
    auto limits = ResolveRiskLimits("sma_crossover", *cfg);
    ASSERT_FALSE(limits.has_value());
    EXPECT_EQ(limits.error().category, types::ErrorCategory::kInvalidArgument);

    auto good = config::BinanceUSConfiguration::Parse(
        WithTables("[risk]\nmax_open_orders=5\n"
                   "[risk.sma_crossover]\nmax_open_orders=1\n"),
        NoEnv());
    ASSERT_OK(good);
    auto resolved = ResolveRiskLimits("sma_crossover", *good);
    ASSERT_OK(resolved);
    EXPECT_EQ((*resolved)->max_open_orders, 1u);
}

TEST(Config, EmptyOrMissingRiskMeansNoLimits) {
    auto none = config::BinanceUSConfiguration::Parse(WithTables(""), NoEnv());
    ASSERT_OK(none);
    EXPECT_FALSE(none->GetRiskLimits().has_value());
    auto empty =
        config::BinanceUSConfiguration::Parse(WithTables("[risk]\n"), NoEnv());
    ASSERT_OK(empty);
    EXPECT_FALSE(empty->GetRiskLimits().has_value());
}

TEST(Config, StrategyParametersKeepTheirText) {
    auto cfg = config::BinanceUSConfiguration::Parse(
        WithTables("[strategy]\nsymbol='BTC/USD'\nfast_period=5\n"
                   "quantity=0.01\n"),
        NoEnv());
    ASSERT_OK(cfg);
    EXPECT_EQ(cfg->GetStrategyParams().at("symbol"), "BTC/USD");
    EXPECT_EQ(cfg->GetStrategyParams().at("fast_period"), "5");
    EXPECT_EQ(cfg->GetStrategyParams().at("quantity"), "0.01");
    EXPECT_FALSE(config::BinanceUSConfiguration::Parse(
                     WithTables("[strategy]\nsymbol=['a']\n"), NoEnv())
                     .has_value());
}

TEST(Factory, CreateStrategyBuildsSmaCrossoverFromParameters) {
    auto cfg = config::BinanceUSConfiguration::Parse(
        WithTables("[strategy]\nsymbol='eth-usd'\nquantity='0.5'\n"
                   "timeframe='4h'\nfast_period=3\nslow_period=8\n"),
        NoEnv());
    ASSERT_OK(cfg);
    auto strategy = CreateStrategy("sma_crossover", *cfg);
    ASSERT_OK(strategy);
    const auto needs = (*strategy)->Needs();
    EXPECT_EQ(needs.symbols, std::vector<types::Symbol>{"ETH/USD"});
    EXPECT_EQ(needs.bar_timeframes,
              std::vector<types::Timeframe>{types::Timeframe::kH4});
    EXPECT_EQ(needs.warmup_bars, 9u);
}

TEST(Factory, CreateStrategyRejectsBadInput) {
    auto make = [](const char* strategy_table) {
        auto cfg = config::BinanceUSConfiguration::Parse(
            WithTables(strategy_table), NoEnv());
        EXPECT_TRUE(cfg.has_value());
        return CreateStrategy("sma_crossover", *cfg);
    };
    EXPECT_EQ(make("").error().category,
              types::ErrorCategory::kInvalidArgument);  // no symbol
    EXPECT_FALSE(make("[strategy]\nsymbol='BTC/USD'\n").has_value());  // qty
    EXPECT_FALSE(
        make("[strategy]\nsymbol='SOL/USD'\nquantity=1\n").has_value());
    EXPECT_FALSE(make("[strategy]\nsymbol='BTC/USD'\nquantity=1\n"
                      "fast_perod=3\n")
                     .has_value());
    EXPECT_FALSE(make("[strategy]\nsymbol='BTC/USD'\nquantity=1\n"
                      "fast_period=30\nslow_period=10\n")
                     .has_value());
    EXPECT_FALSE(
        make("[strategy]\nsymbol='BTC/USD'\nquantity=0\n").has_value());
    EXPECT_FALSE(make("[strategy]\nsymbol='BTC/USD'\nquantity=1\n"
                      "timeframe='7x'\n")
                     .has_value());

    auto cfg = config::BinanceUSConfiguration::Parse(WithTables(""), NoEnv());
    ASSERT_OK(cfg);
    EXPECT_EQ(CreateStrategy("nope", *cfg).error().category,
              types::ErrorCategory::kNotFound);
}

TEST(Factory, ValidateForRunRefusesUnsafeLiveModes) {
    auto paper = config::BinanceUSConfiguration::Parse(WithTables(""), NoEnv());
    ASSERT_OK(paper);
    EXPECT_TRUE(ValidateForRun(*paper, paper->GetRiskLimits()).has_value());

    const auto env = NoEnv({{"KESTREL_BINANCEUS_API_KEY", "k"},
                            {"KESTREL_BINANCEUS_API_SECRET", "s"}});
    auto no_risk =
        config::BinanceUSConfiguration::Parse(WithTables("", "live"), env);
    ASSERT_OK(no_risk);
    auto refused = ValidateForRun(*no_risk, no_risk->GetRiskLimits());
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().category, types::ErrorCategory::kInvalidArgument);

    auto stub = config::BinanceUSConfiguration::Parse(
        WithTables("[risk]\nmax_open_orders=1\n", "live"), env);
    ASSERT_OK(stub);
    auto stubbed = ValidateForRun(*stub, stub->GetRiskLimits());
    ASSERT_FALSE(stubbed.has_value());
    EXPECT_EQ(stubbed.error().category, types::ErrorCategory::kNotImplemented);
}

TEST(SchwabTokenStore, SaveLoadRoundTripAndPermissions) {
    const auto dir =
        std::filesystem::temp_directory_path() / "kestrel-token-test";
    std::filesystem::create_directories(dir);
    config::SchwabTokenStore store(dir / "tokens.json");
    EXPECT_EQ(store.Load().error().category, types::ErrorCategory::kNotFound);

    config::SchwabTokens t{"access", "refresh",
                           types::FromEpochSeconds(1'700'000'000),
                           types::FromEpochSeconds(1'700'000'000 + 7 * 86400)};
    ASSERT_OK(store.Save(t));
    auto loaded = store.Load();
    ASSERT_OK(loaded);
    EXPECT_EQ(loaded->refresh_token, "refresh");
    EXPECT_EQ(loaded->refresh_expires_at, t.refresh_expires_at);
    EXPECT_EQ(std::filesystem::status(dir / "tokens.json").permissions() &
                  (std::filesystem::perms::group_all |
                   std::filesystem::perms::others_all),
              std::filesystem::perms::none);
    std::filesystem::remove_all(dir);
}

TEST(SchwabTokenStore, HealthClassification) {
    using config::SchwabTokenHealth;
    const auto issued = types::FromEpochSeconds(1'700'000'000);
    config::SchwabTokens t{"a", "r", issued + std::chrono::minutes(30),
                           issued + std::chrono::hours(24 * 7)};
    EXPECT_EQ(config::SchwabTokenStore::Check(t, issued),
              SchwabTokenHealth::kValid);
    EXPECT_EQ(
        config::SchwabTokenStore::Check(t, issued + std::chrono::hours(1)),
        SchwabTokenHealth::kAccessExpired);
    EXPECT_EQ(config::SchwabTokenStore::Check(
                  t, issued + std::chrono::hours(24 * 7 - 2)),
              SchwabTokenHealth::kAccessExpired);  // warns, still refreshable
    EXPECT_EQ(
        config::SchwabTokenStore::Check(t, issued + std::chrono::hours(24 * 7)),
        SchwabTokenHealth::kReauthorizationRequired);
}

}  // namespace
