#pragma once

#include <spdlog/spdlog.h>

#include <exception>
#include <functional>
#include <string_view>
#include <utility>

namespace kestrel::data {

// Calls a stream's callback on the thread that delivers it (a socket's, a
// poller's), logging and swallowing anything it throws: a throw would
// otherwise end that thread's delivery or, out of a std::thread, the
// process. Callbacks must not throw (see QuoteCallback); this is the
// backstop every dispatch site uses, so `what` names it in the log, e.g.
// "kraken ws: quote callback".
template <typename Callback, typename... Args>
void CallGuarded(std::string_view what, Callback&& callback,
                 Args&&... args) noexcept {
    // Logging can throw as well: spdlog rethrows what is not a
    // std::exception, and a sink (an alert's, say) may fail. Nothing gets
    // out of here, or the noexcept would end the process after all.
    const auto log = [what](const char* thrown) noexcept {
        try {
            spdlog::error("{} threw: {}", what, thrown);
        } catch (...) {
        }
    };
    try {
        std::invoke(std::forward<Callback>(callback),
                    std::forward<Args>(args)...);
    } catch (const std::exception& e) {
        log(e.what());
    } catch (...) {
        log("something other than a std::exception");
    }
}

}  // namespace kestrel::data
