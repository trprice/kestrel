#include "kestrel/factory.hpp"

#include <spdlog/spdlog.h>

#include "kestrel/config/schwab_token_store.hpp"
#include "kestrel/data/binanceus_data_source.hpp"
#include "kestrel/data/kraken_data_source.hpp"
#include "kestrel/data/schwab_data_source.hpp"
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

Result<VenuePtr> MakePaper(const config::Configuration& cfg, std::shared_ptr<data::DataSource> ds) {
  auto venue = std::make_unique<trading::PaperTradingVenue>(std::move(ds), cfg.GetPaperOptions());
  if (auto started = venue->Start(cfg.GetSymbols()); !started) return std::unexpected(started.error());
  return VenuePtr(std::move(venue));
}

types::Error MissingCredentials(std::string_view provider, std::string_view hint) {
  return MakeError(ErrorCategory::kAuth, "live mode on " + std::string(provider) +
                                             " needs credentials; set " + std::string(hint) +
                                             " or add them to the secrets file")
      .error();
}

Result<Components> BuildKraken(const config::KrakenConfiguration& cfg) {
  data::KrakenDataSource::Options options;
  options.rest_base_url = cfg.RestUrl();
  options.ws_url = cfg.WsUrl();
  auto ds = std::make_shared<data::KrakenDataSource>(std::make_shared<net::CprHttpClient>(), options);
  if (cfg.GetMode() == config::Mode::kPaper) {
    auto venue = MakePaper(cfg, ds);
    if (!venue) return std::unexpected(venue.error());
    return Components{ds, std::move(*venue)};
  }
  if (!cfg.Credentials()) {
    return std::unexpected(MissingCredentials("kraken", "KESTREL_KRAKEN_API_KEY and KESTREL_KRAKEN_API_SECRET"));
  }
  return Components{ds, std::make_unique<trading::KrakenTradingVenue>(*cfg.Credentials(), ds)};
}

Result<Components> BuildBinanceUS(const config::BinanceUSConfiguration& cfg) {
  auto ds = std::make_shared<data::BinanceUSDataSource>();
  if (cfg.GetMode() == config::Mode::kPaper) {
    auto venue = MakePaper(cfg, ds);
    if (!venue) return std::unexpected(venue.error());
    return Components{ds, std::move(*venue)};
  }
  if (!cfg.Credentials()) {
    return std::unexpected(MissingCredentials("binanceus", "KESTREL_BINANCEUS_API_KEY and KESTREL_BINANCEUS_API_SECRET"));
  }
  return Components{ds, std::make_unique<trading::BinanceUSTradingVenue>(*cfg.Credentials(), ds)};
}

Result<Components> BuildSchwab(const config::SchwabConfiguration& cfg) {
  auto ds = std::make_shared<data::SchwabDataSource>();
  if (cfg.GetMode() == config::Mode::kPaper) {
    auto venue = MakePaper(cfg, ds);
    if (!venue) return std::unexpected(venue.error());
    return Components{ds, std::move(*venue)};
  }
  if (!cfg.Credentials()) {
    return std::unexpected(MissingCredentials("schwab", "KESTREL_SCHWAB_APP_KEY and KESTREL_SCHWAB_APP_SECRET"));
  }
  // Surface an expired refresh token now, loudly, instead of on the first order.
  const config::SchwabTokenStore store(cfg.TokenFile());
  auto tokens = store.Load();
  if (!tokens) {
    config::SchwabTokenStore::LogReauthorizationInstructions(cfg.Credentials()->app_key, cfg.RedirectUri());
    return MakeError(ErrorCategory::kAuth, "no usable Schwab tokens: " + tokens.error().message);
  }
  if (config::SchwabTokenStore::Check(*tokens, types::Now()) == config::SchwabTokenHealth::kReauthorizationRequired) {
    config::SchwabTokenStore::LogReauthorizationInstructions(cfg.Credentials()->app_key, cfg.RedirectUri());
    return MakeError(ErrorCategory::kAuth, "Schwab refresh token expired; re-authorization required");
  }
  return Components{ds, std::make_unique<trading::SchwabTradingVenue>(*cfg.Credentials(), ds)};
}

}  // namespace

Result<Components> CreateComponents(const config::Configuration& configuration) {
  switch (configuration.GetProvider()) {
    case config::Provider::kKraken:
      return BuildKraken(dynamic_cast<const config::KrakenConfiguration&>(configuration));
    case config::Provider::kBinanceUS:
      return BuildBinanceUS(dynamic_cast<const config::BinanceUSConfiguration&>(configuration));
    case config::Provider::kSchwab:
      return BuildSchwab(dynamic_cast<const config::SchwabConfiguration&>(configuration));
  }
  return MakeError(ErrorCategory::kInternal, "unknown provider");
}

}  // namespace kestrel
