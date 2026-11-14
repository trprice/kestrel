#include "kestrel/strategy/log_context.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace kestrel::strategy {

namespace {

spdlog::level::level_enum ToSpdlog(LogContext::Level level) {
    switch (level) {
        case LogContext::Level::kDebug:
            return spdlog::level::debug;
        case LogContext::Level::kInfo:
            return spdlog::level::info;
        case LogContext::Level::kWarn:
            return spdlog::level::warn;
        case LogContext::Level::kError:
            return spdlog::level::err;
    }
    return spdlog::level::info;
}

}  // namespace

LogContext::LogContext(std::string_view strategy, Sink sink)
    : mStrategy(strategy), mSink(std::move(sink)) {
    RebuildPrefix();
}

LogContext LogContext::WithStrategy(std::string_view strategy) const {
    LogContext copy = *this;
    copy.mStrategy.assign(strategy);
    copy.RebuildPrefix();
    return copy;
}

LogContext LogContext::With(std::string_view key,
                            std::string_view value) const {
    LogContext copy = *this;
    if (key.empty()) {
        return copy;
    }
    const auto it = std::ranges::find(copy.mFields, key, &Field::key);
    if (value.empty()) {
        if (it != copy.mFields.end()) {
            copy.mFields.erase(it);
        }
    } else if (it != copy.mFields.end()) {
        it->value.assign(value);
    } else {
        copy.mFields.push_back(Field{std::string(key), std::string(value)});
    }
    copy.RebuildPrefix();
    return copy;
}

LogContext LogContext::WithSink(Sink sink) const {
    LogContext copy = *this;
    copy.mSink = std::move(sink);
    return copy;
}

bool LogContext::Has(std::string_view key) const {
    return std::ranges::find(mFields, key, &Field::key) != mFields.end();
}

bool LogContext::ShouldLog(Level level) const {
    if (mSink) {
        return true;
    }
    return spdlog::should_log(ToSpdlog(level));
}

void LogContext::Write(Level level, std::string_view message) const {
    if (mSink) {
        if (mPrefix.empty()) {
            mSink(level, message);
        } else {
            const std::string line = std::format("{}: {}", mPrefix, message);
            mSink(level, line);
        }
        return;
    }
    if (mPrefix.empty()) {
        spdlog::log(ToSpdlog(level), "{}", message);
    } else {
        spdlog::log(ToSpdlog(level), "{}: {}", mPrefix, message);
    }
}

void LogContext::RebuildPrefix() {
    mPrefix = mStrategy;
    for (const Field& field : mFields) {
        if (!mPrefix.empty()) {
            mPrefix += ' ';
        }
        mPrefix += field.key;
        mPrefix += '=';
        mPrefix += field.value;
    }
}

}  // namespace kestrel::strategy
