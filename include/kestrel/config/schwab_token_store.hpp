#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "kestrel/types/error.hpp"
#include "kestrel/types/time.hpp"

namespace kestrel::config {

// Schwab access tokens last about 30 minutes and are renewed silently with the
// refresh token. The refresh token itself expires after 7 days, and only a
// human can replace it (by redoing the browser authorization), so the store
// tracks its age and logs loudly when that is needed.
struct SchwabTokens {
    std::string access_token;
    std::string refresh_token;
    types::Timestamp access_expires_at;
    types::Timestamp refresh_expires_at;
};

enum class SchwabTokenHealth {
    kValid,
    kAccessExpired,            // refreshable automatically
    kReauthorizationRequired,  // refresh token expired: the user must
                               // re-authorize
};

class SchwabTokenStore {
   public:
    explicit SchwabTokenStore(std::filesystem::path path)
        : mPath(std::move(path)) {}

    const std::filesystem::path& Path() const { return mPath; }

    // kNotFound when the file does not exist yet (first run).
    types::Result<SchwabTokens> Load() const;
    // Atomic (write + rename); the file is created readable by the owner only.
    // Saves in other threads and processes take turns (an flock on the
    // file's directory).
    types::Result<void> Save(const SchwabTokens& tokens) const;
    // Saves renewed tokens unless the file holds a later authorization: one
    // whose refresh token outlasts `tokens`'s, which only a new `authorize`
    // gives, since renewing keeps the deadline. Then the file is left as it
    // is and its tokens returned, so a process that renews with an older
    // authorization never undoes a re-authorization made while it ran.
    // nullopt when `tokens` were saved.
    types::Result<std::optional<SchwabTokens>> SaveUnlessSuperseded(
        const SchwabTokens& tokens) const;

    // Classifies the tokens and logs: a warning when the refresh token has
    // under 24h left, an error with re-authorization instructions once it has
    // expired.
    static SchwabTokenHealth Check(const SchwabTokens& tokens,
                                   types::Timestamp now);

    // Logged when there are no usable tokens at all (first run, expired or
    // refused): says to run the `authorize` command.
    static void LogReauthorizationInstructions();

   private:
    std::filesystem::path mPath;
};

}  // namespace kestrel::config
