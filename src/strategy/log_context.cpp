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

bool IsControl(char c) {
    return static_cast<unsigned char>(c) < 0x20 || c == 0x7f;
}

// `value` as is, or quoted if it would make "k=v k=v" ambiguous or split the
// line.
void AppendValue(std::string& out, std::string_view value) {
    const bool plain = std::ranges::none_of(value, [](char c) {
        return c == ' ' || c == '=' || c == '"' || c == '\\' || IsControl(c);
    });
    if (plain) {
        out += value;
        return;
    }
    out += '"';
    for (const char c : value) {
        switch (c) {
            case '"':
            case '\\':
                out += '\\';
                out += c;
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (IsControl(c)) {
                    out +=
                        std::format("\\x{:02x}", static_cast<unsigned char>(c));
                } else {
                    out += c;
                }
        }
    }
    out += '"';
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

LogContext LogContext::WithDefault(std::string_view key,
                                   std::string_view value) const {
    return Has(key) ? *this : With(key, value);
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
        AppendValue(mPrefix, field.value);
    }
}

}  // namespace kestrel::strategy
