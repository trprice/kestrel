#include "kestrel/data/guarded_callback.hpp"

#include <gtest/gtest.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

namespace kestrel::data {
namespace {

TEST(CallGuarded, PassesTheArgumentsThrough) {
    int sum = 0;
    std::function<void(int, int)> add = [&](int a, int b) { sum = a + b; };
    CallGuarded("test callback", add, 2, 3);  // a stored callback
    EXPECT_EQ(sum, 5);
    CallGuarded("test callback", [&](int a) { sum = a; }, 7);  // inline
    EXPECT_EQ(sum, 7);
}

TEST(CallGuarded, SwallowsAStdException) {
    EXPECT_NO_THROW(
        CallGuarded("test callback", [] { throw std::runtime_error("boom"); }));
}

TEST(CallGuarded, SwallowsAnythingElseThrown) {
    EXPECT_NO_THROW(
        CallGuarded("test callback", [] { throw std::string("not one"); }));
    EXPECT_NO_THROW(CallGuarded("test callback", [] { throw 42; }));
}

// A sink that fails while the guard logs is swallowed too: the guard is
// noexcept, so anything getting out would end the process.
TEST(CallGuarded, SurvivesASinkThatThrowsWhileLogging) {
    struct ThrowingSink : spdlog::sinks::base_sink<std::mutex> {
        void sink_it_(const spdlog::details::log_msg&) override { throw 7; }
        void flush_() override {}
    };
    auto sink = std::make_shared<ThrowingSink>();
    auto& sinks = spdlog::default_logger()->sinks();
    sinks.push_back(sink);
    EXPECT_NO_THROW(CallGuarded("test callback", [] { throw 42; }));
    std::erase(sinks, sink);
}

}  // namespace
}  // namespace kestrel::data
