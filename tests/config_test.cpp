#include <filesystem>
#include <fstream>
#include <thread>

#include "kestrel/backtest/replay_data_source.hpp"
#include "kestrel/config/configuration.hpp"
#include "kestrel/config/schwab_token_store.hpp"
#include "kestrel/factory.hpp"
#include "strategy_fakes.hpp"
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

TEST(Config, LimitFillModelDefaultsToCrossAndAcceptsConservative) {
    auto def = config::KrakenConfiguration::Parse(kKrakenToml, NoEnv());
    ASSERT_OK(def);
    EXPECT_EQ(def->GetPaperOptions().limit_fill_model,
              trading::LimitFillModel::kCross);

    const std::string base = kKrakenToml;
    auto conservative = config::KrakenConfiguration::Parse(
        base + "limit_fill_model = \"conservative\"\n", NoEnv());
    ASSERT_OK(conservative);
    EXPECT_EQ(conservative->GetPaperOptions().limit_fill_model,
              trading::LimitFillModel::kConservative);

    EXPECT_FALSE(config::KrakenConfiguration::Parse(
                     base + "limit_fill_model = \"nope\"\n", NoEnv())
                     .has_value());
}

TEST(Config, PaperKeysOfTheWrongTypeAreRefused) {
    const std::string base = kKrakenToml;
    EXPECT_FALSE(config::KrakenConfiguration::Parse(
                     base + "default_quote_asset = 100\n", NoEnv())
                     .has_value());
    EXPECT_FALSE(
        config::KrakenConfiguration::Parse(
            base + "cap_limit_fills_at_quote_size = \"yes\"\n", NoEnv())
            .has_value());
    auto ok = config::KrakenConfiguration::Parse(
        base + "cap_limit_fills_at_quote_size = true\n", NoEnv());
    ASSERT_OK(ok);
    EXPECT_TRUE(ok->GetPaperOptions().cap_limit_fills_at_quote_size);
}

TEST(Config, PaperShortingIsOffByDefaultAndTakesAMargin) {
    auto def = config::KrakenConfiguration::Parse(kKrakenToml, NoEnv());
    ASSERT_OK(def);
    EXPECT_FALSE(def->GetPaperOptions().allow_short);
    EXPECT_EQ(def->GetPaperOptions().short_margin, D("1.5"));

    const std::string base = kKrakenToml;
    auto set = config::KrakenConfiguration::Parse(
        base + "allow_short = true\nshort_margin = 2\n", NoEnv());
    ASSERT_OK(set);
    EXPECT_TRUE(set->GetPaperOptions().allow_short);
    EXPECT_EQ(set->GetPaperOptions().short_margin, D("2"));

    EXPECT_FALSE(config::KrakenConfiguration::Parse(
                     base + "short_margin = 0.9\n", NoEnv())
                     .has_value());
    EXPECT_FALSE(config::KrakenConfiguration::Parse(
                     base + "allow_short = \"yes\"\n", NoEnv())
                     .has_value());
}

TEST(Config, PaperLatencyDefaultsToNone) {
    auto def = config::KrakenConfiguration::Parse(kKrakenToml, NoEnv());
    ASSERT_OK(def);
    EXPECT_EQ(def->GetPaperOptions().order_latency.count(), 0);
    EXPECT_EQ(def->GetPaperOptions().report_latency.count(), 0);

    const std::string base = kKrakenToml;
    auto set = config::KrakenConfiguration::Parse(
        base + "order_latency_ms = 130\nreport_latency_ms = 600\n", NoEnv());
    ASSERT_OK(set);
    EXPECT_EQ(set->GetPaperOptions().order_latency.count(), 130);
    EXPECT_EQ(set->GetPaperOptions().report_latency.count(), 600);
    EXPECT_FALSE(config::KrakenConfiguration::Parse(
                     base + "order_latency_ms = -1\n", NoEnv())
                     .has_value());
    EXPECT_FALSE(config::KrakenConfiguration::Parse(
                     base + "report_latency_ms = 0.5\n", NoEnv())
                     .has_value());
}

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
    EXPECT_EQ(cfg->GetDatabase()->statement_timeout, std::chrono::minutes(5));
}

// The shipped examples must load: every key in them is one Kestrel knows.
TEST(Config, TheExampleConfigsLoad) {
    const auto dir =
        std::filesystem::path(KESTREL_TEST_DATA_DIR) / ".." / ".." / "config";
    int loaded = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        const auto name = entry.path().filename().string();
        if (!name.ends_with(".example.toml") || name.starts_with("secrets")) {
            continue;
        }
        auto cfg = config::LoadConfiguration(entry.path());
        EXPECT_TRUE(cfg.has_value())
            << name << ": " << (cfg ? "" : cfg.error().ToString());
        ++loaded;
    }
    EXPECT_GE(loaded, 3);
}

TEST(Config, UnknownKeysAndTablesAreErrors) {
    const std::string base = kKrakenToml;
    for (const char* extra :
         {"\n[recordng]\nenabled = true\n", "\n[database]\nprot = 5433\n",
          "\n[kraken]\nrest_ulr = \"https://example.test\"\n"}) {
        auto cfg = config::KrakenConfiguration::Parse(base + extra, NoEnv());
        ASSERT_FALSE(cfg.has_value()) << extra;
        EXPECT_NE(cfg.error().message.find("unknown"), std::string::npos)
            << cfg.error().message;
    }
    // Each provider checks its own section.
    const auto as = [&base](const char* provider) {
        std::string text = base;
        text.replace(text.find("\"kraken\""), 8,
                     "\"" + std::string(provider) + "\"");
        return text;
    };
    auto binance = config::BinanceUSConfiguration::Parse(
        as("binanceus") + "\n[binanceus]\nws = \"wss://example.test\"\n",
        NoEnv());
    ASSERT_FALSE(binance.has_value());
    EXPECT_NE(binance.error().message.find("unknown key 'ws'"),
              std::string::npos)
        << binance.error().message;
    auto schwab = config::SchwabConfiguration::Parse(
        as("schwab") + "\n[schwab]\ntoken_fiile = \"t.json\"\n", NoEnv());
    ASSERT_FALSE(schwab.has_value());
    EXPECT_NE(schwab.error().message.find("unknown key 'token_fiile'"),
              std::string::npos)
        << schwab.error().message;
    // [general] and [paper] are checked too.
    std::string general = base;
    general.insert(general.find("[general]") + 10, "simbols = [\"X\"]\n");
    EXPECT_FALSE(config::KrakenConfiguration::Parse(general, NoEnv()));
    std::string paper = base;
    paper.insert(paper.find("[paper]") + 8, "slipage_bps = 5\n");
    EXPECT_FALSE(config::KrakenConfiguration::Parse(paper, NoEnv()));
    // Another provider's section is allowed, and not read.
    EXPECT_TRUE(config::KrakenConfiguration::Parse(
        base + "\n[schwab]\nextended_hours = true\n", NoEnv()));
    // A credential in the main file still says so, not "unknown key".
    auto secret = config::KrakenConfiguration::Parse(
        base + "\n[database]\npassword = \"x\"\n", NoEnv());
    ASSERT_FALSE(secret.has_value());
    EXPECT_NE(secret.error().message.find("credentials"), std::string::npos);
}

TEST(Config, DatabaseStatementTimeout) {
    const auto parse = [](const std::string& value) {
        return config::KrakenConfiguration::Parse(
            std::string(kKrakenToml) +
                "\n[database]\nstatement_timeout_ms = " + value + "\n",
            NoEnv());
    };
    auto set = parse("60000");
    ASSERT_OK(set);
    EXPECT_EQ(set->GetDatabase()->statement_timeout, std::chrono::minutes(1));
    auto none = parse("0");
    ASSERT_OK(none);
    EXPECT_EQ(none->GetDatabase()->statement_timeout.count(), 0);
    for (const char* bad : {"-1", "1.5", "\"30s\""}) {
        EXPECT_FALSE(parse(bad).has_value()) << bad;
    }
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
    EXPECT_FALSE(cfg->ExtendedHours());

    const std::string base = "[general]\nprovider='schwab'\nsymbols=['SPY']\n";
    auto extended = config::SchwabConfiguration::Parse(
        base + "[schwab]\nextended_hours=true\n", NoEnv());
    ASSERT_OK(extended);
    EXPECT_TRUE(extended->ExtendedHours());
    EXPECT_FALSE(config::SchwabConfiguration::Parse(
                     base + "[schwab]\nextended_hours='yes'\n", NoEnv())
                     .has_value());

    EXPECT_EQ(cfg->TakeoverReconnect(), std::chrono::milliseconds(0));
    auto reconnect = config::SchwabConfiguration::Parse(
        base + "[schwab]\ntakeover_reconnect_ms=300000\n", NoEnv());
    ASSERT_OK(reconnect);
    EXPECT_EQ(reconnect->TakeoverReconnect(), std::chrono::minutes(5));
    EXPECT_FALSE(config::SchwabConfiguration::Parse(
                     base + "[schwab]\ntakeover_reconnect_ms=-1\n", NoEnv())
                     .has_value());
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
    // No call on the venue: it would go to the real exchange. Its behaviour is
    // tested against a fake HTTP client in binanceus_trading_venue_test.cpp.
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
          "[risk]\nallowed_symbols='BTC/USD'\n",
          "[risk]\nmax_symbol_share=1.5\n", "[risk]\nmax_symbol_share=0\n",
          "[risk]\nmax_quote_age_ms=0\n",
          "[risk]\nmax_loss_per_position='-5'\n"}) {
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

TEST(Config, RiskSectionParsesThePortfolioAndClosingLimits) {
    auto cfg = config::BinanceUSConfiguration::Parse(
        WithTables("[risk]\nmax_total_notional='5000'\nmax_symbol_share=0.4\n"
                   "max_drawdown_close='600'\nmax_loss_per_position='150'\n"
                   "max_quote_age_ms=60000\n"),
        NoEnv());
    ASSERT_OK(cfg);
    const auto& r = *cfg->GetRiskLimits();
    EXPECT_EQ(r.max_total_notional, D("5000"));
    EXPECT_EQ(r.max_symbol_share, D("0.4"));
    EXPECT_EQ(r.max_drawdown_close, D("600"));
    EXPECT_EQ(r.max_loss_per_position, D("150"));
    EXPECT_EQ(r.max_quote_age, std::chrono::milliseconds(60000));

    auto merged = config::BinanceUSConfiguration::Parse(
        WithTables("[risk]\nmax_symbol_share=0.4\n"
                   "[risk.breakout]\nmax_symbol_share=0.2\n"
                   "max_loss_per_position='50'\n"),
        NoEnv());
    ASSERT_OK(merged);
    const auto breakout = merged->GetRiskLimits("breakout");
    EXPECT_EQ(breakout->max_symbol_share, D("0.2"));
    EXPECT_EQ(breakout->max_loss_per_position, D("50"));
}

TEST(Config, PerStrategyRiskOverridesTheGlobalLimitsKeyByKey) {
    auto cfg = config::BinanceUSConfiguration::Parse(
        WithTables("[risk]\nmax_order_notional='1000'\nmax_open_orders=5\n"
                   "allowed_symbols=['BTC/USD','ETH/USD']\n"
                   "[risk.ma_crossover]\nmax_open_orders=1\n"
                   "max_orders_per_day=6\nallowed_symbols=['BTC/USD']\n"),
        NoEnv());
    ASSERT_OK(cfg);
    // The global limits are untouched.
    EXPECT_EQ(cfg->GetRiskLimits()->max_open_orders, 5u);
    EXPECT_FALSE(cfg->GetRiskLimits()->max_orders_per_day.has_value());

    const auto sma = cfg->GetRiskLimits("ma_crossover");
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
        WithTables("[risk.ma_crossover]\nmax_open_orders=2\n"), NoEnv());
    ASSERT_OK(cfg);
    EXPECT_FALSE(cfg->GetRiskLimits().has_value());
    EXPECT_EQ(cfg->GetRiskLimits("ma_crossover")->max_open_orders, 2u);
    EXPECT_FALSE(cfg->GetRiskLimits("other").has_value());
}

TEST(Config, PerStrategyRiskIsCheckedLikeTheGlobalSection) {
    for (const char* bad :
         {"[risk.ma_crossover]\nmax_open_ordrs=1\n",
          "[risk.ma_crossover]\nmax_open_orders=0\n",
          "[risk.ma_crossover.deeper]\nmax_open_orders=1\n"}) {
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
    auto limits = ResolveRiskLimits("ma_crossover", *cfg);
    ASSERT_FALSE(limits.has_value());
    EXPECT_EQ(limits.error().category, types::ErrorCategory::kInvalidArgument);

    auto good = config::BinanceUSConfiguration::Parse(
        WithTables("[risk]\nmax_open_orders=5\n"
                   "[risk.ma_crossover]\nmax_open_orders=1\n"),
        NoEnv());
    ASSERT_OK(good);
    auto resolved = ResolveRiskLimits("ma_crossover", *good);
    ASSERT_OK(resolved);
    EXPECT_EQ((*resolved)->max_open_orders, 1u);
}

TEST(Factory, ManualOrdersKeepOnlyTheLimitsOneOrderCanTrip) {
    EXPECT_FALSE(ManualOrderLimits(std::nullopt));
    trading::RiskLimits losses;
    losses.max_daily_loss = types::Decimal::FromInt(100);
    losses.max_orders_per_minute = 5;
    EXPECT_FALSE(ManualOrderLimits(losses));  // none applies to one order

    trading::RiskLimits all = losses;
    all.allowed_symbols = {"BTC/USDT"};
    all.max_order_notional = types::Decimal::FromInt(50);
    all.max_position_notional = types::Decimal::FromInt(500);
    all.max_open_orders = 3;
    all.max_drawdown = types::Decimal::FromInt(10);
    all.max_price_age = std::chrono::milliseconds(5000);
    all.max_total_notional = types::Decimal::FromInt(900);
    all.max_symbol_share = types::Decimal(5, 1);
    all.max_loss_per_position = types::Decimal::FromInt(20);
    all.max_quote_age = std::chrono::milliseconds(60000);
    const auto manual = ManualOrderLimits(all);
    ASSERT_TRUE(manual);
    EXPECT_EQ(manual->allowed_symbols, all.allowed_symbols);
    EXPECT_EQ(manual->max_order_notional, all.max_order_notional);
    EXPECT_EQ(manual->max_position_notional, all.max_position_notional);
    EXPECT_EQ(manual->max_open_orders, all.max_open_orders);
    EXPECT_EQ(manual->max_price_age, all.max_price_age);
    EXPECT_FALSE(manual->max_daily_loss);
    EXPECT_FALSE(manual->max_orders_per_minute);
    EXPECT_FALSE(manual->max_drawdown);
    EXPECT_EQ(manual->max_total_notional, all.max_total_notional);
    EXPECT_EQ(manual->max_symbol_share, all.max_symbol_share);
    EXPECT_FALSE(manual->max_loss_per_position);
    EXPECT_FALSE(manual->max_quote_age);
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

TEST(Factory, CreateStrategyBuildsMaCrossoverFromParameters) {
    auto cfg = config::BinanceUSConfiguration::Parse(
        WithTables("[strategy]\nsymbol='eth-usd'\nquantity='0.5'\n"
                   "timeframe='4h'\nfast_period=3\nslow_period=8\n"),
        NoEnv());
    ASSERT_OK(cfg);
    auto strategy = CreateStrategy("ma_crossover", *cfg);
    ASSERT_OK(strategy);
    const auto needs = (*strategy)->Needs();
    EXPECT_EQ(needs.symbols, std::vector<types::Symbol>{"ETH/USD"});
    EXPECT_EQ(needs.bar_timeframes,
              std::vector<types::Timeframe>{types::Timeframe::kH4});
    EXPECT_EQ(needs.warmup_bars, 9u);
}

TEST(Factory, MaCrossoverTakesAnExponentialAverage) {
    auto make = [](const char* average) {
        auto cfg = config::BinanceUSConfiguration::Parse(
            WithTables(
                std::string("[strategy]\nsymbol='eth-usd'\n"
                            "quantity='0.5'\nfast_period=3\nslow_period=8\n") +
                average),
            NoEnv());
        EXPECT_TRUE(cfg.has_value());
        return CreateStrategy("ma_crossover", *cfg);
    };
    auto ema = make("average='ema'\n");
    ASSERT_OK(ema);
    EXPECT_EQ((*ema)->Needs().warmup_bars, 33u);  // 4 * 8 + 1
    auto sma = make("average='sma'\n");
    ASSERT_OK(sma);
    EXPECT_EQ((*sma)->Needs().warmup_bars, 9u);
    EXPECT_FALSE(make("average='wma'\n").has_value());
}

TEST(Factory, MarketMakerProtectionRulesAreOptIn) {
    const std::string base =
        "[strategy]\nsymbol='BTC/USD'\nquantity='0.05'\n"
        "half_spread_bps=2\nmax_inventory='0.5'\n";
    auto make = [&](const std::string& extra) {
        auto cfg = config::BinanceUSConfiguration::Parse(
            WithTables(base + extra), NoEnv());
        EXPECT_TRUE(cfg.has_value());
        return CreateStrategy("market_maker", *cfg);
    };
    EXPECT_TRUE(make("").has_value());
    EXPECT_TRUE(make("stop_loss=true\nstop_loss_bps=25\nloss_cap=true\n"
                     "loss_cap_amount='3'\nspike_standdown=false\n"
                     "cooldown_ms=60000\n")
                    .has_value());
    // An enabled rule needs its amount; a bad flag or a typo is an error.
    EXPECT_FALSE(make("loss_cap=true\n").has_value());
    EXPECT_FALSE(make("stop_loss='yes'\n").has_value());
    EXPECT_FALSE(make("stop_los=true\n").has_value());
    // Queue position.
    EXPECT_TRUE(make("keep_at_touch=true\ntouch='join'\n").has_value());
    EXPECT_TRUE(make("touch='improve'\n").has_value());
    EXPECT_FALSE(make("touch='best'\n").has_value());
    auto quiet = make("quiet_pull_ms=2000\n");
    ASSERT_OK(quiet);
    EXPECT_EQ((*quiet)->Needs().timer_interval, std::chrono::milliseconds(500));
    EXPECT_FALSE(make("quiet_pull_ms=-1\n").has_value());
    EXPECT_TRUE(
        make("skew_bps=4\nskew_power=2\nskew_age_ms=600000\n").has_value());
    EXPECT_FALSE(make("skew_power=0.5\n").has_value());
    EXPECT_TRUE(make("levels=3\nlevel_step_bps=4\n").has_value());
    EXPECT_FALSE(make("levels=0\n").has_value());
    EXPECT_FALSE(make("levels=11\n").has_value());
}

TEST(Factory, MarketMakerVolatilityScalingIsOptIn) {
    const std::string base =
        "[strategy]\nsymbol='BTC/USD'\nquantity='0.05'\n"
        "half_spread_bps=2\nmax_inventory='0.5'\n";
    auto make = [&](const std::string& extra) {
        auto cfg = config::BinanceUSConfiguration::Parse(
            WithTables(base + extra), NoEnv());
        EXPECT_TRUE(cfg.has_value());
        return CreateStrategy("market_maker", *cfg);
    };
    EXPECT_TRUE(make("vol_multiplier=0.4\nvol_window_ms=300000\n"
                     "max_half_spread_bps=10\n")
                    .has_value());
    EXPECT_FALSE(make("vol_multiplier=-1\n").has_value());
    EXPECT_FALSE(make("vol_multiplier=1\nvol_window_ms=0\n").has_value());
    EXPECT_FALSE(make("max_half_spread_bps=1\n").has_value());  // under 2
}

TEST(Factory, MarketMakerReadsTheTowardsRequoteThreshold) {
    const std::string base =
        "[strategy]\nsymbol='BTC/USD'\nquantity='0.05'\n"
        "half_spread_bps=2\nmax_inventory='0.5'\nrequote_bps=3\n";
    auto make = [&](const std::string& extra) {
        auto cfg = config::BinanceUSConfiguration::Parse(
            WithTables(base + extra), NoEnv());
        EXPECT_TRUE(cfg.has_value());
        return CreateStrategy("market_maker", *cfg);
    };
    EXPECT_TRUE(make("").has_value());
    EXPECT_TRUE(make("requote_toward_bps=0\n").has_value());
    EXPECT_FALSE(make("requote_toward_bps=-1\n").has_value());
    EXPECT_FALSE(make("requote_toward_bps='x'\n").has_value());
    EXPECT_TRUE(make("microprice_weight=0.5\n").has_value());
    EXPECT_FALSE(make("microprice_weight=2\n").has_value());
    EXPECT_TRUE(
        make("flow_pull_imbalance=0.6\nflow_window_ms=5000\n").has_value());
    EXPECT_FALSE(make("flow_pull_imbalance=1.5\n").has_value());
    EXPECT_FALSE(
        make("flow_pull_imbalance=0.6\nflow_window_ms=0\n").has_value());
}

TEST(Factory, CreateStrategyBuildsBreakoutFromParameters) {
    const std::string base = "[strategy]\nsymbol='BTC/USD'\nquantity='0.1'\n";
    auto make = [&](const std::string& extra) {
        auto cfg = config::BinanceUSConfiguration::Parse(
            WithTables(base + extra), NoEnv());
        EXPECT_TRUE(cfg.has_value());
        return CreateStrategy("breakout", *cfg);
    };
    auto plain = make("");
    ASSERT_OK(plain);
    EXPECT_EQ((*plain)->Name(), "breakout");
    EXPECT_EQ((*plain)->Needs().warmup_bars, 4u);  // NR4
    auto range = make(
        "timeframe='30m'\nsetup='range'\nrange_bars=20\nconfirm='closes'\n"
        "confirm_closes=2\nstop_buffer_bps=20\ntarget_multiple=1.5\n"
        "arm_bars=3\nmax_hold_bars=0\ntrend_sma=50\nflatten_at_close=true\n");
    ASSERT_OK(range);
    EXPECT_EQ((*range)->Needs().warmup_bars, 50u);  // the trend SMA's
    EXPECT_EQ((*range)->Needs().bar_timeframes,
              std::vector<types::Timeframe>{types::Timeframe::kM30});
    EXPECT_TRUE(make("setup='inside'\nconfirm='percent'\nconfirm_percent=1\n")
                    .has_value());
    EXPECT_TRUE(make("entry='throwback'\nthrowback_bars=5\n").has_value());
    auto rectangle = make(
        "setup='rectangle'\nrange_bars=30\ntouch_zone=0.15\n"
        "min_touches=3\nshortfall=true\n");
    ASSERT_OK(rectangle);
    EXPECT_EQ((*rectangle)->Needs().warmup_bars, 30u);
    EXPECT_FALSE(make("setup='rectangle'\ntouch_zone=0.5\n").has_value());
    EXPECT_FALSE(make("min_touches=0\n").has_value());
    auto pipe = make("setup='pipe'\ndecline_bars=15\n");
    ASSERT_OK(pipe);
    EXPECT_EQ((*pipe)->Needs().warmup_bars, 17u);  // the look back and two
    EXPECT_FALSE(make("decline_bars=0\n").has_value());
    auto failed =
        make("setup='failed_breakdown'\nrange_bars=20\nreclaim_bars=2\n");
    ASSERT_OK(failed);
    EXPECT_EQ((*failed)->Needs().warmup_bars, 23u);  // range, break, newest
    EXPECT_FALSE(make("reclaim_bars=0\n").has_value());
    auto triple = make("setup='triple_bottom'\nrange_bars=60\nswing_bars=3\n");
    ASSERT_OK(triple);
    EXPECT_EQ((*triple)->Needs().warmup_bars, 60u);
    EXPECT_TRUE(make("setup='double_bottom'\n").has_value());
    auto hs = make("setup='inverse_head_shoulders'\nrange_bars=40\n");
    ASSERT_OK(hs);
    EXPECT_EQ((*hs)->Needs().warmup_bars, 40u);
    EXPECT_FALSE(make("setup='inverse_head_shoulders'\nrange_bars=10\n"));
    auto tri = make("setup='symmetrical_triangle'\nrange_bars=30\n");
    ASSERT_OK(tri);
    EXPECT_EQ((*tri)->Needs().warmup_bars, 30u);
    EXPECT_TRUE(make("setup='ascending_triangle'\n").has_value());
    EXPECT_TRUE(make("resting_target=true\n").has_value());
    EXPECT_TRUE(make("direction='short'\nreverse=true\n").has_value());
    EXPECT_FALSE(make("direction='both'\n").has_value());
    auto cup = make("setup='cup_handle'\nrange_bars=120\n");
    ASSERT_OK(cup);
    EXPECT_EQ((*cup)->Needs().warmup_bars, 120u);
    EXPECT_FALSE(make("setup='cup_handle'\nrange_bars=9\n").has_value());
    EXPECT_FALSE(make("setup='ascending_triangle'\nmin_touches=1\n"));
    EXPECT_FALSE(make("swing_bars=0\n").has_value());
    auto opening =
        make("timeframe='5m'\nsetup='opening_range'\nopening_bars=3\n");
    ASSERT_OK(opening);
    EXPECT_EQ((*opening)->Needs().warmup_bars, 4u);  // and the bar before
    EXPECT_FALSE(make("opening_bars=0\n").has_value());
    EXPECT_FALSE(make("timeframe='1d'\nsetup='opening_range'\n").has_value());
    auto volume = make(
        "confirm='closes'\nvolume_ratio=2\nvolume_bars=30\nbreakeven_r=1\n"
        "trail_bars=5\n");
    ASSERT_OK(volume);
    EXPECT_EQ((*volume)->Needs().warmup_bars, 31u);      // and the newest bar
    EXPECT_FALSE(make("volume_ratio=2\n").has_value());  // a buy-stop
    EXPECT_FALSE(make("confirm='closes'\nvolume_bars=0\n").has_value());
    EXPECT_FALSE(make("breakeven_r=-1\n").has_value());
    auto atr = make("atr_stop=2.5\natr_bars=10\n");
    ASSERT_OK(atr);
    EXPECT_EQ((*atr)->Needs().warmup_bars, 61u);  // 6 * 10 + 1
    EXPECT_FALSE(make("atr_stop=-1\n").has_value());
    EXPECT_FALSE(make("atr_stop=1\natr_bars=0\n").has_value());
    // Unknown words, bad numbers and typos are errors.
    EXPECT_FALSE(make("entry='pullback'\n").has_value());
    EXPECT_FALSE(make("entry='throwback'\nthrowback_bars=0\n").has_value());
    EXPECT_FALSE(make("setup='triangle'\n").has_value());
    EXPECT_FALSE(make("confirm='maybe'\n").has_value());
    EXPECT_FALSE(make("nr_bars=1\n").has_value());
    EXPECT_FALSE(make("target_multiple=-1\n").has_value());
    EXPECT_FALSE(make("trend_sma='x'\n").has_value());
    EXPECT_FALSE(make("stop_bufer_bps=5\n").has_value());
}

TEST(Factory, CreateStrategyRejectsBadInput) {
    auto make = [](const char* strategy_table) {
        auto cfg = config::BinanceUSConfiguration::Parse(
            WithTables(strategy_table), NoEnv());
        EXPECT_TRUE(cfg.has_value());
        return CreateStrategy("ma_crossover", *cfg);
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

// CreateRunner opens the provider connection only after every check below has
// passed, so these need no network.
types::Error RunnerFailure(const config::Configuration& cfg,
                           const std::string& strategy = "ma_crossover") {
    auto runner = CreateRunner(strategy, cfg);
    EXPECT_FALSE(runner.has_value());
    return runner ? types::Error{} : runner.error();
}

TEST(Factory, CreateRunnerRefusesWhatCannotRun) {
    const char* strategy = "[strategy]\nsymbol='BTC/USD'\nquantity='0.01'\n";
    const auto env = NoEnv({{"KESTREL_BINANCEUS_API_KEY", "k"},
                            {"KESTREL_BINANCEUS_API_SECRET", "s"}});

    // Unknown strategy.
    auto paper =
        config::BinanceUSConfiguration::Parse(WithTables(strategy), NoEnv());
    ASSERT_OK(paper);
    EXPECT_EQ(RunnerFailure(*paper, "nope").category,
              types::ErrorCategory::kNotFound);

    // Bad strategy parameters, with the step named.
    auto bad = config::BinanceUSConfiguration::Parse(
        WithTables("[strategy]\nsymbol='BTC/USD'\n"), NoEnv());
    ASSERT_OK(bad);
    EXPECT_NE(RunnerFailure(*bad).message.find("strategy: "),
              std::string::npos);

    // A [risk.<name>] section naming no strategy.
    auto typo = config::BinanceUSConfiguration::Parse(
        WithTables(std::string(strategy) +
                   "[risk.sma_crosover]\nmax_open_orders=1\n"),
        NoEnv());
    ASSERT_OK(typo);
    EXPECT_NE(RunnerFailure(*typo).message.find("risk: "), std::string::npos);

    // Live mode without limits, and live mode on a stub venue.
    auto live = config::BinanceUSConfiguration::Parse(
        WithTables(strategy, "live"), env);
    ASSERT_OK(live);
    EXPECT_EQ(RunnerFailure(*live).category,
              types::ErrorCategory::kInvalidArgument);
    auto limited = config::KrakenConfiguration::Parse(
        WithTables(std::string(strategy) + "[risk]\nmax_open_orders=1\n",
                   "live"),
        NoEnv({{"KESTREL_KRAKEN_API_KEY", "key"},
               {"KESTREL_KRAKEN_API_SECRET", "c2VjcmV0"}}));
    ASSERT_OK(limited);
    EXPECT_EQ(RunnerFailure(*limited).category,
              types::ErrorCategory::kNotImplemented);
}

// A provider connection made of fakes, so CreateRunner needs no network.
struct FakeComponents {
    std::shared_ptr<kestrel::testing::RecordingVenue> venue =
        std::make_shared<kestrel::testing::RecordingVenue>();
    int built = 0;

    ComponentsFactory Factory() {
        return [this](
                   const config::Configuration&) -> types::Result<Components> {
            ++built;
            // The runner takes ownership of its venue, so hand over a
            // forwarding wrapper and keep the real one for the test.
            return Components{std::make_shared<backtest::ReplayDataSource>(),
                              std::make_unique<Forward>(venue)};
        };
    }

   private:
    class Forward : public trading::TradingVenue {
       public:
        explicit Forward(std::shared_ptr<trading::TradingVenue> v)
            : mVenue(std::move(v)) {}
        std::string_view Name() const override { return mVenue->Name(); }
        types::Result<types::OrderAcknowledgement> PlaceOrder(
            const types::OrderRequest& r) override {
            return mVenue->PlaceOrder(r);
        }
        types::Result<types::OcoAcknowledgement> PlaceOco(
            const types::OcoRequest& r) override {
            return mVenue->PlaceOco(r);
        }
        types::Result<trading::CancelOutcome> CancelOrder(
            const std::string& id) override {
            return mVenue->CancelOrder(id);
        }
        types::Result<std::vector<std::string>> CancelAll() override {
            return mVenue->CancelAll();
        }
        types::Result<std::vector<types::OrderStatus>> GetOpenOrders()
            override {
            return mVenue->GetOpenOrders();
        }
        types::Result<types::OrderStatus> GetOrder(
            const std::string& id) override {
            return mVenue->GetOrder(id);
        }
        types::Result<std::vector<types::Fill>> GetFills(
            types::Timestamp since) override {
            return mVenue->GetFills(since);
        }
        types::Result<std::vector<types::Position>> GetPositions() override {
            return mVenue->GetPositions();
        }
        types::Result<std::vector<types::Balance>> GetBalances() override {
            return mVenue->GetBalances();
        }

       private:
        std::shared_ptr<trading::TradingVenue> mVenue;
    };
};

constexpr const char* kSmaParams =
    "[strategy]\nsymbol='BTC/USD'\nquantity='0.01'\n";

TEST(Factory, CreateRunnerBuildsARunnerWithAGateWhenLimitsApply) {
    auto cfg = config::BinanceUSConfiguration::Parse(
        WithTables(std::string(kSmaParams) + "[risk]\nmax_daily_loss=100\n"),
        NoEnv());
    ASSERT_OK(cfg);
    FakeComponents fake;
    auto runner = CreateRunner("ma_crossover", *cfg, fake.Factory());
    ASSERT_OK(runner);
    EXPECT_EQ(fake.built, 1);
    ASSERT_NE((*runner)->RiskGate(), nullptr);
    EXPECT_FALSE((*runner)->RiskGate()->IsHalted());
}

TEST(Config, RecordingSectionIsParsed) {
    auto off = config::BinanceUSConfiguration::Parse(WithTables(""), NoEnv());
    ASSERT_OK(off);
    EXPECT_FALSE(off->GetRecording().enabled);
    EXPECT_EQ(off->GetRecording().pnl_interval, std::chrono::seconds(60));
    EXPECT_TRUE(off->GetRecording().latency);
    EXPECT_EQ(off->GetRecording().latency_interval, std::chrono::seconds(60));

    auto on = config::BinanceUSConfiguration::Parse(
        WithTables("[recording]\nenabled=true\npnl_interval_ms=5000\n"),
        NoEnv());
    ASSERT_OK(on);
    EXPECT_TRUE(on->GetRecording().enabled);
    EXPECT_EQ(on->GetRecording().pnl_interval, std::chrono::seconds(5));

    auto latency = config::BinanceUSConfiguration::Parse(
        WithTables("[recording]\nlatency=false\nlatency_interval_ms=15000\n"),
        NoEnv());
    ASSERT_OK(latency);
    EXPECT_FALSE(latency->GetRecording().latency);
    EXPECT_EQ(latency->GetRecording().latency_interval,
              std::chrono::seconds(15));

    for (const char* bad :
         {"[recording]\nenabled='yes'\n", "[recording]\npnl_interval_ms=-1\n",
          "[recording]\npnl_interval_ms=1.5\n", "[recording]\nenable=true\n",
          "[recording]\nlatency='yes'\n",
          "[recording]\nlatency_interval_ms=0\n",
          "[recording]\nlatency_interval_ms=2.5\n"}) {
        EXPECT_FALSE(
            config::BinanceUSConfiguration::Parse(WithTables(bad), NoEnv())
                .has_value())
            << bad;
    }
}

TEST(Config, HttpSectionIsParsed) {
    auto off = config::BinanceUSConfiguration::Parse(WithTables(""), NoEnv());
    ASSERT_OK(off);
    EXPECT_EQ(off->GetHttp().slow_request, std::chrono::seconds(2));

    auto on = config::BinanceUSConfiguration::Parse(
        WithTables("[http]\nslow_request_ms=750\n"), NoEnv());
    ASSERT_OK(on);
    EXPECT_EQ(on->GetHttp().slow_request, std::chrono::milliseconds(750));

    for (const char* bad :
         {"[http]\nslow_request_ms=0\n", "[http]\nslow_request_ms='2s'\n",
          "[http]\nslow_request_ms=1.5\n", "[http]\nslow=1\n"}) {
        EXPECT_FALSE(
            config::BinanceUSConfiguration::Parse(WithTables(bad), NoEnv())
                .has_value())
            << bad;
    }
}

TEST(Factory, CreateRunnerRecordsThroughTheLogWhenEnabled) {
    auto cfg = config::BinanceUSConfiguration::Parse(
        WithTables(std::string(kSmaParams) +
                   "[recording]\nenabled=true\npnl_interval_ms=0\n"),
        NoEnv());
    ASSERT_OK(cfg);
    FakeComponents fake;
    auto log = std::make_shared<kestrel::testing::FakeTradeLog>();
    std::string strategy_seen, venue_seen;
    auto runner =
        CreateRunner("ma_crossover", *cfg, fake.Factory(),
                     [&](const config::Configuration&,
                         const std::string& strategy, const std::string& venue)
                         -> types::Result<std::shared_ptr<trading::TradeLog>> {
                         strategy_seen = strategy;
                         venue_seen = venue;
                         return log;
                     });
    ASSERT_OK(runner);
    EXPECT_EQ(strategy_seen, "ma_crossover");
    EXPECT_EQ(venue_seen, "recording");
}

TEST(Factory, CreateRunnerDoesNotRecordUnlessEnabled) {
    auto cfg =
        config::BinanceUSConfiguration::Parse(WithTables(kSmaParams), NoEnv());
    ASSERT_OK(cfg);
    FakeComponents fake;
    bool asked = false;
    auto runner =
        CreateRunner("ma_crossover", *cfg, fake.Factory(),
                     [&](const config::Configuration&, const std::string&,
                         const std::string&)
                         -> types::Result<std::shared_ptr<trading::TradeLog>> {
                         asked = true;
                         return std::shared_ptr<trading::TradeLog>();
                     });
    ASSERT_OK(runner);
    EXPECT_FALSE(asked);
}

TEST(Factory, CreateRunnerFailsWhenRecordingCannotBeSetUp) {
    auto cfg = config::BinanceUSConfiguration::Parse(
        WithTables(std::string(kSmaParams) + "[recording]\nenabled=true\n"),
        NoEnv());
    ASSERT_OK(cfg);
    FakeComponents fake;
    auto failing = CreateRunner(
        "ma_crossover", *cfg, fake.Factory(),
        [](const config::Configuration&, const std::string&, const std::string&)
            -> types::Result<std::shared_ptr<trading::TradeLog>> {
            return types::MakeError(types::ErrorCategory::kNetwork, "down");
        });
    ASSERT_FALSE(failing.has_value());
    EXPECT_NE(failing.error().message.find("recording"), std::string::npos);

    // The default log needs a [database], which this configuration lacks.
    auto no_database = CreateRunner("ma_crossover", *cfg, fake.Factory());
    ASSERT_FALSE(no_database.has_value());
    EXPECT_EQ(no_database.error().category, types::ErrorCategory::kNotFound);
}

// A market maker with a loss cap keeps state; `make_state` stands in for
// the database.
constexpr const char* kCappedMaker =
    "[strategy]\nsymbol='BTC/USD'\nquantity='0.01'\nhalf_spread_bps=4\n"
    "max_inventory='0.1'\nloss_cap=true\nloss_cap_amount='5'\n";

types::Result<std::unique_ptr<strategy::Runner>> MakerRunner(
    const config::Configuration& cfg, FakeComponents& fake,
    types::Result<std::shared_ptr<strategy::StateStore>> store,
    std::string* venue_seen = nullptr) {
    return CreateRunner(
        "market_maker", cfg, fake.Factory(),
        [](const config::Configuration&, const std::string&, const std::string&)
            -> types::Result<std::shared_ptr<trading::TradeLog>> {
            return types::MakeError(types::ErrorCategory::kInternal,
                                    "not asked");
        },
        [store, venue_seen](const config::Configuration&,
                            const std::string& venue) {
            if (venue_seen) {
                *venue_seen = venue;
            }
            return store;
        });
}

TEST(Factory, CreateRunnerGivesAStrategyThatKeepsStateAStore) {
    auto cfg = config::BinanceUSConfiguration::Parse(WithTables(kCappedMaker),
                                                     NoEnv());
    ASSERT_OK(cfg);
    FakeComponents fake;
    std::string venue;
    auto runner = MakerRunner(*cfg, fake,
                              std::shared_ptr<strategy::StateStore>(), &venue);
    ASSERT_OK(runner);
    EXPECT_EQ(venue, "replay");  // the provider: the data source's name
}

TEST(Factory, WithoutADatabaseAPaperRunKeepsNoStateButALiveOneIsRefused) {
    const auto none =
        types::MakeError(types::ErrorCategory::kNotFound, "no [database]");
    auto paper = config::BinanceUSConfiguration::Parse(WithTables(kCappedMaker),
                                                       NoEnv());
    ASSERT_OK(paper);
    FakeComponents fake;
    EXPECT_TRUE(
        MakerRunner(*paper, fake, std::unexpected(none.error())).has_value());
    // A database that is configured but fails is an error in paper too.
    auto down = MakerRunner(
        *paper, fake,
        types::MakeError(types::ErrorCategory::kNetwork, "refused"));
    ASSERT_FALSE(down);
    EXPECT_NE(down.error().message.find("strategy state"), std::string::npos);

    auto live = config::BinanceUSConfiguration::Parse(
        WithTables(std::string(kCappedMaker) + "[risk]\nmax_open_orders=5\n",
                   "live"),
        NoEnv({{"KESTREL_BINANCEUS_API_KEY", "k"},
               {"KESTREL_BINANCEUS_API_SECRET", "s"}}));
    ASSERT_OK(live);
    FakeComponents live_fake;
    auto refused = MakerRunner(*live, live_fake, std::unexpected(none.error()));
    ASSERT_FALSE(refused);
    EXPECT_NE(refused.error().message.find("strategy state"),
              std::string::npos);
}

TEST(Factory, CreateRunnerAppliesThePerStrategyLimits) {
    // Only the override sets a limit, so only ma_crossover gets a gate.
    auto cfg = config::BinanceUSConfiguration::Parse(
        WithTables(std::string(kSmaParams) +
                   "[risk.ma_crossover]\nmax_open_orders=1\n"),
        NoEnv());
    ASSERT_OK(cfg);
    FakeComponents fake;
    auto runner = CreateRunner("ma_crossover", *cfg, fake.Factory());
    ASSERT_OK(runner);
    EXPECT_NE((*runner)->RiskGate(), nullptr);
}

TEST(Factory, CreateRunnerWithoutLimitsHasNoGateInPaperMode) {
    auto cfg =
        config::BinanceUSConfiguration::Parse(WithTables(kSmaParams), NoEnv());
    ASSERT_OK(cfg);
    FakeComponents fake;
    auto runner = CreateRunner("ma_crossover", *cfg, fake.Factory());
    ASSERT_OK(runner);
    EXPECT_EQ((*runner)->RiskGate(), nullptr);
}

TEST(Factory, CreateRunnerFailsIfTheDailyLossCountCannotStart) {
    auto cfg = config::BinanceUSConfiguration::Parse(
        WithTables(std::string(kSmaParams) + "[risk]\nmax_daily_loss=100\n"),
        NoEnv());
    ASSERT_OK(cfg);
    FakeComponents fake;
    fake.venue->fail_reads = true;  // GetPositions fails
    auto runner = CreateRunner("ma_crossover", *cfg, fake.Factory());
    ASSERT_FALSE(runner.has_value());
    EXPECT_NE(runner.error().message.find("cannot start the daily loss count"),
              std::string::npos)
        << runner.error().message;
}

TEST(Factory, CreateRunnerReportsAFailedConnection) {
    auto cfg =
        config::BinanceUSConfiguration::Parse(WithTables(kSmaParams), NoEnv());
    ASSERT_OK(cfg);
    auto runner = CreateRunner(
        "ma_crossover", *cfg,
        [](const config::Configuration&) -> types::Result<Components> {
            return types::MakeError(types::ErrorCategory::kNetwork, "down");
        });
    ASSERT_FALSE(runner.has_value());
    EXPECT_EQ(runner.error().category, types::ErrorCategory::kNetwork);
    EXPECT_NE(runner.error().message.find("startup: "), std::string::npos);
}

TEST(Factory, CreateRunnerChecksBeforeConnecting) {
    auto cfg =
        config::BinanceUSConfiguration::Parse(WithTables(kSmaParams), NoEnv());
    ASSERT_OK(cfg);
    FakeComponents fake;
    EXPECT_FALSE(CreateRunner("nope", *cfg, fake.Factory()).has_value());
    EXPECT_EQ(fake.built, 0);  // refused before any connection was made
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

    auto live = config::BinanceUSConfiguration::Parse(
        WithTables("[risk]\nmax_open_orders=1\n", "live"), env);
    ASSERT_OK(live);
    EXPECT_TRUE(ValidateForRun(*live, live->GetRiskLimits()).has_value());

    // Kraken's venue is still a stub.
    auto stub = config::KrakenConfiguration::Parse(
        WithTables("[risk]\nmax_open_orders=1\n", "live"),
        NoEnv({{"KESTREL_KRAKEN_API_KEY", "key"},
               {"KESTREL_KRAKEN_API_SECRET", "c2VjcmV0"}}));
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

// Each save writes its own temporary file and renames it over the token
// file, so concurrent saves (`authorize` while `run` renews) leave one whole
// file, and nothing behind.
TEST(SchwabTokenStore, ConcurrentSavesLeaveOneWholeFile) {
    const auto dir =
        std::filesystem::temp_directory_path() / "kestrel-token-race-test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const config::SchwabTokenStore store(dir / "tokens.json");
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&store, i] {
            for (int k = 0; k < 50; ++k) {
                const std::string token(
                    static_cast<std::size_t>(100 + (i * 50 + k) % 300), 'a');
                EXPECT_TRUE(store
                                .Save({token, "refresh-" + std::to_string(i),
                                       types::FromEpochSeconds(1'700'000'000),
                                       types::FromEpochSeconds(1'700'600'000)})
                                .has_value());
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    auto loaded = store.Load();
    ASSERT_OK(loaded);
    EXPECT_EQ(loaded->refresh_token.rfind("refresh-", 0), 0u);
    int files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        (void)entry;
        ++files;
    }
    EXPECT_EQ(files, 1);  // no temporary file left behind
    std::filesystem::remove_all(dir);
}

TEST(SchwabTokenStore, AFailedSaveLeavesNoTemporaryFile) {
    const auto dir =
        std::filesystem::temp_directory_path() / "kestrel-token-fail-test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "tokens.json");  // a directory
    const config::SchwabTokenStore store(dir / "tokens.json");
    EXPECT_FALSE(store.Save(
        {"a", "r", types::FromEpochSeconds(1), types::FromEpochSeconds(2)}));
    int files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        (void)entry;
        ++files;
    }
    EXPECT_EQ(files, 1);  // only the directory in the way
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
