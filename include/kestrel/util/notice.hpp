#pragma once

#include <functional>
#include <string_view>

namespace kestrel::util {

// A notice: something a person should hear of that is not an error, such as
// a risk-gate halt or a loss-limit close. It is logged at warning level and
// handed to the notice handler, which the alerts set to post it to Discord
// (app/commands/alerts.cpp). With no handler it is only logged.
void Notice(std::string_view text);

// Sets the handler, or clears it with an empty function. Any thread may call
// Notice, so the handler must be thread-safe, and must not block or throw. A
// Notice already under way when the handler is replaced may still call the
// old one.
using NoticeHandler = std::function<void(std::string_view)>;
void SetNoticeHandler(NoticeHandler handler);

}  // namespace kestrel::util
