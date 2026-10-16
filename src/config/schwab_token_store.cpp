#include "kestrel/config/schwab_token_store.hpp"

#include <spdlog/spdlog.h>
#include <sys/stat.h>

#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace kestrel::config {

using types::ErrorCategory;
using types::MakeError;

types::Result<SchwabTokens> SchwabTokenStore::Load() const {
  std::ifstream in(mPath);
  if (!in)
  {
    return MakeError(ErrorCategory::kNotFound, "no Schwab token file at " + mPath.string());
  }
  std::stringstream buf;
  buf << in.rdbuf();
  try {
    const auto j = nlohmann::json::parse(buf.str());
    SchwabTokens t;
    t.access_token = j.at("access_token").get<std::string>();
    t.refresh_token = j.at("refresh_token").get<std::string>();
    auto access = types::ParseIso8601(j.at("access_expires_at").get<std::string>());
    auto refresh = types::ParseIso8601(j.at("refresh_expires_at").get<std::string>());
    if (!access)
    {
      return std::unexpected(access.error());
    }
    if (!refresh)
    {
      return std::unexpected(refresh.error());
    }
    t.access_expires_at = *access;
    t.refresh_expires_at = *refresh;
    return t;
  } catch (const nlohmann::json::exception& e) {
    return MakeError(ErrorCategory::kParse, "corrupt Schwab token file " + mPath.string() + ": " + e.what());
  }
}

types::Result<void> SchwabTokenStore::Save(const SchwabTokens& t) const {
  const nlohmann::json j = {{"access_token", t.access_token},
                            {"refresh_token", t.refresh_token},
                            {"access_expires_at", types::FormatIso8601(t.access_expires_at)},
                            {"refresh_expires_at", types::FormatIso8601(t.refresh_expires_at)}};
  auto tmp = mPath;
  tmp += ".tmp";
  {
    std::ofstream probe(tmp, std::ios::trunc);  // create, then lock down before writing secrets
    if (!probe)
    {
      return MakeError(ErrorCategory::kInternal, "cannot write " + tmp.string());
    }
  }
  ::chmod(tmp.c_str(), S_IRUSR | S_IWUSR);
  {
    std::ofstream out(tmp, std::ios::trunc);
    out << j.dump(2) << '\n';
    if (!out)
    {
      return MakeError(ErrorCategory::kInternal, "failed writing " + tmp.string());
    }
  }
  std::error_code ec;
  std::filesystem::rename(tmp, mPath, ec);
  if (ec)
  {
    return MakeError(ErrorCategory::kInternal, "cannot replace " + mPath.string() + ": " + ec.message());
  }
  return {};
}

SchwabTokenHealth SchwabTokenStore::Check(const SchwabTokens& tokens, types::Timestamp now) {
  if (now >= tokens.refresh_expires_at)
  {
    spdlog::error(
        "SCHWAB RE-AUTHORIZATION REQUIRED: the refresh token expired at {}. Trading and market data "
        "are unavailable until you re-authorize in a browser and store the new tokens.",
        types::FormatIso8601(tokens.refresh_expires_at));
    return SchwabTokenHealth::kReauthorizationRequired;
  }
  const auto remaining = tokens.refresh_expires_at - now;
  if (remaining < std::chrono::hours(24))
  {
    spdlog::warn("Schwab refresh token expires in {} minutes (at {}); re-authorize soon to avoid an outage.",
                 std::chrono::duration_cast<std::chrono::minutes>(remaining).count(),
                 types::FormatIso8601(tokens.refresh_expires_at));
  }
  return now >= tokens.access_expires_at ? SchwabTokenHealth::kAccessExpired : SchwabTokenHealth::kValid;
}

void SchwabTokenStore::LogReauthorizationInstructions(const std::string& app_key,
                                                      const std::string& redirect_uri) {
  spdlog::error(
      "SCHWAB RE-AUTHORIZATION REQUIRED: open https://api.schwabapi.com/v1/oauth/authorize?client_id={}"
      "&redirect_uri={} in a browser, log in, approve access, and exchange the returned code for tokens.",
      app_key, redirect_uri);
}

}  // namespace kestrel::config
