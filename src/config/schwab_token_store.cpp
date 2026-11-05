#include "kestrel/config/schwab_token_store.hpp"

#include <fcntl.h>
#include <spdlog/spdlog.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

namespace kestrel::config {

using types::ErrorCategory;
using types::MakeError;

types::Result<SchwabTokens> SchwabTokenStore::Load() const {
    std::ifstream in(mPath);
    if (!in) {
        return MakeError(ErrorCategory::kNotFound,
                         "no Schwab token file at " + mPath.string());
    }
    std::stringstream buf;
    buf << in.rdbuf();
    try {
        const auto j = nlohmann::json::parse(buf.str());
        SchwabTokens t;
        t.access_token = j.at("access_token").get<std::string>();
        t.refresh_token = j.at("refresh_token").get<std::string>();
        auto access =
            types::ParseIso8601(j.at("access_expires_at").get<std::string>());
        auto refresh =
            types::ParseIso8601(j.at("refresh_expires_at").get<std::string>());
        if (!access) {
            return std::unexpected(access.error());
        }
        if (!refresh) {
            return std::unexpected(refresh.error());
        }
        t.access_expires_at = *access;
        t.refresh_expires_at = *refresh;
        return t;
    } catch (const nlohmann::json::exception& e) {
        return MakeError(
            ErrorCategory::kParse,
            "corrupt Schwab token file " + mPath.string() + ": " + e.what());
    }
}

types::Result<void> SchwabTokenStore::Save(const SchwabTokens& t) const {
    const nlohmann::json j = {
        {"access_token", t.access_token},
        {"refresh_token", t.refresh_token},
        {"access_expires_at", types::FormatIso8601(t.access_expires_at)},
        {"refresh_expires_at", types::FormatIso8601(t.refresh_expires_at)}};
    const std::string text = j.dump(2) + '\n';

    // Written beside the file, then renamed over it, so a reader sees the old
    // file or the new one, never part of one. The name is this process's and
    // this call's, so `authorize` and a running renewal cannot write into
    // each other's file; whichever renames last wins, whole. Created 0600, so
    // the tokens are never readable by others, and synced before the rename
    // (and the directory after), so a power loss cannot leave it empty.
    static std::atomic<unsigned> counter{0};
    auto tmp = mPath;
    tmp +=
        ".tmp." + std::to_string(::getpid()) + "." + std::to_string(counter++);
    // Removes the temporary file; `error` is the errno of what failed.
    const auto fail = [&](const std::string& what, int error) {
        ::unlink(tmp.c_str());
        return MakeError(ErrorCategory::kInternal,
                         what + ": " + std::strerror(error));
    };
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                          S_IRUSR | S_IWUSR);
    if (fd < 0) {
        return MakeError(
            ErrorCategory::kInternal,
            "cannot write " + tmp.string() + ": " + std::strerror(errno));
    }
    for (std::size_t done = 0; done < text.size();) {
        const ssize_t n = ::write(fd, text.data() + done, text.size() - done);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            const int error = n < 0 ? errno : EIO;
            ::close(fd);
            return fail("failed writing " + tmp.string(), error);
        }
        done += static_cast<std::size_t>(n);
    }
    if (::fsync(fd) != 0) {
        const int error = errno;
        ::close(fd);
        return fail("failed syncing " + tmp.string(), error);
    }
    if (::close(fd) != 0) {
        return fail("failed writing " + tmp.string(), errno);
    }
    if (::rename(tmp.c_str(), mPath.c_str()) != 0) {
        return fail("cannot replace " + mPath.string(), errno);
    }
    // The rename itself is durable once the directory is synced; a failure
    // here leaves the new file in place, so it is only logged.
    auto dir = mPath.parent_path();
    if (dir.empty()) {
        dir = ".";
    }
    if (const int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        dfd >= 0) {
        if (::fsync(dfd) != 0) {
            spdlog::warn("could not sync {}: {}", dir.string(),
                         std::strerror(errno));
        }
        ::close(dfd);
    }
    return {};
}

SchwabTokenHealth SchwabTokenStore::Check(const SchwabTokens& tokens,
                                          types::Timestamp now) {
    if (now >= tokens.refresh_expires_at) {
        spdlog::error(
            "SCHWAB RE-AUTHORIZATION REQUIRED: the refresh token expired at "
            "{}. Trading and market data "
            "are unavailable until you re-authorize with `kestrel_app "
            "authorize`.",
            types::FormatIso8601(tokens.refresh_expires_at));
        return SchwabTokenHealth::kReauthorizationRequired;
    }
    const auto remaining = tokens.refresh_expires_at - now;
    if (remaining < std::chrono::hours(24)) {
        spdlog::warn(
            "Schwab refresh token expires in {} minutes (at {}); re-authorize "
            "soon with `kestrel_app authorize` to avoid an outage.",
            std::chrono::duration_cast<std::chrono::minutes>(remaining).count(),
            types::FormatIso8601(tokens.refresh_expires_at));
    }
    return now >= tokens.access_expires_at ? SchwabTokenHealth::kAccessExpired
                                           : SchwabTokenHealth::kValid;
}

void SchwabTokenStore::LogReauthorizationInstructions() {
    spdlog::error(
        "SCHWAB RE-AUTHORIZATION REQUIRED: run `kestrel_app -c <config> "
        "authorize`, open the URL it prints, log in to Schwab, approve access "
        "and paste back the address the browser is sent to.");
}

}  // namespace kestrel::config
