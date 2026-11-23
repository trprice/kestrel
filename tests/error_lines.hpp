#pragma once

#include <spdlog/sinks/base_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "kestrel/util/notice.hpp"

namespace kestrel::testing {

// Captures error-level lines logged to spdlog's default logger while it lives
// (the lines Discord alerts send). Construct before starting threads that log,
// and destroy after joining them, since spdlog's sink vector is unsynchronised.
class ErrorLines {
   public:
    ErrorLines() : mSink(std::make_shared<Sink>()) {
        mSink->set_level(spdlog::level::err);
        spdlog::default_logger()->sinks().push_back(mSink);
    }
    ~ErrorLines() { std::erase(spdlog::default_logger()->sinks(), mSink); }
    ErrorLines(const ErrorLines&) = delete;
    ErrorLines& operator=(const ErrorLines&) = delete;

    std::size_t Count(std::string_view text) const {
        std::lock_guard lock(mSink->mutex());
        return static_cast<std::size_t>(
            std::ranges::count_if(mSink->lines, [&](const std::string& line) {
                return line.find(text) != std::string::npos;
            }));
    }

   private:
    struct Sink final : spdlog::sinks::base_sink<std::mutex> {
        std::vector<std::string> lines;
        std::mutex& mutex() { return mutex_; }
        void sink_it_(const spdlog::details::log_msg& msg) override {
            lines.emplace_back(msg.payload.data(), msg.payload.size());
        }
        void flush_() override {}
    };
    std::shared_ptr<Sink> mSink;
};

// Collects util::Notice's notices while it lives (the alerts post them).
class Notices {
   public:
    Notices() {
        util::SetNoticeHandler([this](std::string_view text) {
            std::lock_guard lock(mMutex);
            mTexts.emplace_back(text);
        });
    }
    ~Notices() { util::SetNoticeHandler({}); }
    Notices(const Notices&) = delete;
    Notices& operator=(const Notices&) = delete;

    std::size_t Count(std::string_view text) const {
        std::lock_guard lock(mMutex);
        return static_cast<std::size_t>(
            std::ranges::count_if(mTexts, [&](const std::string& line) {
                return line.find(text) != std::string::npos;
            }));
    }

   private:
    mutable std::mutex mMutex;
    std::vector<std::string> mTexts;
};

}  // namespace kestrel::testing
