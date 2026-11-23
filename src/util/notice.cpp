#include "kestrel/util/notice.hpp"

#include <spdlog/spdlog.h>

#include <memory>
#include <mutex>

namespace kestrel::util {

namespace {

std::mutex& HandlerMutex() {
    static std::mutex mutex;
    return mutex;
}

std::shared_ptr<const NoticeHandler>& Handler() {
    static std::shared_ptr<const NoticeHandler> handler;
    return handler;
}

}  // namespace

void Notice(std::string_view text) {
    spdlog::warn("{}", text);
    std::shared_ptr<const NoticeHandler> handler;
    {
        std::lock_guard lock(HandlerMutex());
        handler = Handler();
    }
    if (handler && *handler) {
        (*handler)(text);
    }
}

void SetNoticeHandler(NoticeHandler handler) {
    auto shared =
        handler ? std::make_shared<const NoticeHandler>(std::move(handler))
                : nullptr;
    std::lock_guard lock(HandlerMutex());
    Handler() = std::move(shared);
}

}  // namespace kestrel::util
