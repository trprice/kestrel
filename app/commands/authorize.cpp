#include <spdlog/spdlog.h>

#include <iostream>
#include <string>

#include "command.hpp"
#include "common.hpp"
#include "kestrel/data/schwab_auth.hpp"
#include "kestrel/net/timed_http_client.hpp"

namespace kestrel::app {

namespace {

// Authorizes Kestrel to use the Schwab account: prints the page to open in a
// browser, reads back the address the browser was sent to, exchanges the code
// in it for tokens and saves them to [schwab] token_file. Needed once, then
// again whenever the refresh token's 7 days are up. Nothing has to listen at
// the redirect address: the browser shows an error page there, and the code is
// in its address bar. With --status it only says what the token file holds.
class AuthorizeCommand : public Command {
   public:
    explicit AuthorizeCommand(bool status) : mStatus(status) {}

    int Run(const config::Configuration& config) override {
        if (config.GetProvider() != config::Provider::kSchwab) {
            spdlog::error(
                "authorize is for provider = \"schwab\"; {} needs no "
                "authorization",
                config::ToString(config.GetProvider()));
            return 2;
        }
        const auto& cfg =
            dynamic_cast<const config::SchwabConfiguration&>(config);
        if (mStatus) {
            return Status(cfg);
        }
        if (!cfg.Credentials()) {
            spdlog::error(
                "authorize needs the Schwab app's key and secret: set "
                "KESTREL_SCHWAB_APP_KEY and KESTREL_SCHWAB_APP_SECRET or add "
                "them to the secrets file");
            return 1;
        }
        data::SchwabAuth auth(
            *cfg.Credentials(), config::SchwabTokenStore(cfg.TokenFile()),
            net::MakeTimedHttpClient(cfg.GetHttp().slow_request),
            cfg.RedirectUri());

        std::cout
            << "1. Open this page in a browser, log in to Schwab and approve "
               "access:\n\n   "
            << data::SchwabAuthorizeUrl(cfg.Credentials()->app_key,
                                        cfg.RedirectUri())
            << "\n\n2. Schwab then sends the browser to " << cfg.RedirectUri()
            << ",\n   which shows an error page; that is expected. Copy the "
               "whole address from\n   the address bar and paste it here "
               "straight away (the code in it is\n   short-lived).\n\nAddress: "
            << std::flush;
        std::string pasted;
        if (!std::getline(std::cin, pasted)) {
            spdlog::error("nothing was read from standard input");
            return 1;
        }
        auto code = data::SchwabCodeFromRedirect(pasted);
        if (!code) {
            spdlog::error("{}", code.error().message);
            return 1;
        }
        if (auto ok = auth.Authorize(*code); !ok) {
            spdlog::error("Schwab did not exchange the code for tokens: {}",
                          ok.error().ToString());
            return 1;
        }
        std::cout << "\nSaved the Schwab tokens to " << cfg.TokenFile().string()
                  << ".\nThe access token is renewed automatically. The "
                     "refresh token expires at "
                  << types::FormatIso8601(*auth.RefreshExpiresAt())
                  << ";\nrun authorize again before then.\n";
        return 0;
    }

   private:
    static int Status(const config::SchwabConfiguration& cfg) {
        const config::SchwabTokenStore store(cfg.TokenFile());
        auto tokens = store.Load();
        if (!tokens) {
            std::cout << tokens.error().message
                      << "; run authorize to create it\n";
            return 1;
        }
        const auto now = types::Now();
        std::cout << "token file " << cfg.TokenFile().string() << '\n'
                  << "access token expires  "
                  << types::FormatIso8601(tokens->access_expires_at)
                  << (now >= tokens->access_expires_at
                          ? " (expired; renewed on next use)"
                          : "")
                  << '\n'
                  << "refresh token expires "
                  << types::FormatIso8601(tokens->refresh_expires_at);
        const bool expired = now >= tokens->refresh_expires_at;
        if (expired) {
            std::cout << " (expired; run authorize)";
        } else {
            const auto left = std::chrono::duration_cast<std::chrono::hours>(
                tokens->refresh_expires_at - now);
            std::cout << " (" << left.count() << " h left)";
        }
        std::cout << '\n';
        return expired ? 1 : 0;
    }

    bool mStatus;
};

}  // namespace

void AddAuthorizeCommand(CLI::App& app, CommandSlot& slot) {
    auto* sub = AddSubcommand(
        app, "authorize",
        "Authorize Kestrel to use the Schwab account (once, then every 7 "
        "days): open the printed page in a browser and paste back the "
        "address it is sent to");
    auto status = std::make_shared<bool>(false);
    sub->add_flag("--status", *status,
                  "Only show when the saved tokens expire; contacts nothing");
    sub->callback([&slot, status] {
        slot = std::make_unique<AuthorizeCommand>(*status);
    });
}

}  // namespace kestrel::app
