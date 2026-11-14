#pragma once

#include <format>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kestrel::strategy {

// A strategy run's log context: prepends the strategy's name and its
// key=value fields (such as symbol=BTC/USD) to each message, so log lines
// from one strategy run can be told apart from another's and filtered by
// field.
//
// Immutable once built: WithStrategy, With and WithSink return a new
// LogContext, so the runner's context can be shared across the strategy,
// poller and stream threads without locking.
class LogContext {
   public:
    enum class Level {
        kDebug,
        kInfo,
        kWarn,
        kError,
    };

    struct Field {
        std::string key;
        std::string value;
        bool operator==(const Field&) const = default;
    };

    // Optional destination for formatted messages (including the context
    // prefix); when unset, messages go to spdlog's default logger.
    using Sink = std::function<void(Level, std::string_view)>;

    LogContext() = default;
    explicit LogContext(std::string_view strategy, Sink sink = {});

    LogContext WithStrategy(std::string_view strategy) const;

    // Sets `key` to `value` (updating an existing field in place, or
    // appending a new one), or removes `key` when `value` is empty.
    LogContext With(std::string_view key, std::string_view value) const;

    LogContext WithSink(Sink sink) const;

    std::string_view Strategy() const { return mStrategy; }
    std::span<const Field> Fields() const { return mFields; }
    bool Has(std::string_view key) const;

    // The rendered context header ("<strategy> <k>=<v> ..."), without the
    // trailing ": " separator. Empty when neither a strategy name nor any
    // field is set.
    std::string_view Prefix() const { return mPrefix; }

    template <typename... Args>
    void Debug(std::format_string<Args...> fmt, Args&&... args) const {
        if (ShouldLog(Level::kDebug)) {
            Write(Level::kDebug, std::format(fmt, std::forward<Args>(args)...));
        }
    }

    template <typename... Args>
    void Info(std::format_string<Args...> fmt, Args&&... args) const {
        if (ShouldLog(Level::kInfo)) {
            Write(Level::kInfo, std::format(fmt, std::forward<Args>(args)...));
        }
    }

    template <typename... Args>
    void Warn(std::format_string<Args...> fmt, Args&&... args) const {
        if (ShouldLog(Level::kWarn)) {
            Write(Level::kWarn, std::format(fmt, std::forward<Args>(args)...));
        }
    }

    template <typename... Args>
    void Error(std::format_string<Args...> fmt, Args&&... args) const {
        if (ShouldLog(Level::kError)) {
            Write(Level::kError, std::format(fmt, std::forward<Args>(args)...));
        }
    }

   private:
    bool ShouldLog(Level level) const;
    void Write(Level level, std::string_view message) const;
    void RebuildPrefix();

    std::string mStrategy;
    std::vector<Field> mFields;
    std::string mPrefix;
    Sink mSink;
};

}  // namespace kestrel::strategy
