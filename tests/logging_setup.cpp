// Creates spdlog's default logger on the main thread before any test runs.
// Otherwise a test whose first log line comes from a worker thread creates
// it there while another thread logs; with a prebuilt (uninstrumented)
// spdlog, as in the CI image, ThreadSanitizer cannot see the synchronization
// inside the library and reports a data race. The app logs on its main
// thread before starting any.
#include <spdlog/spdlog.h>

namespace {

[[maybe_unused]] const bool kDefaultLoggerMade =
    static_cast<bool>(spdlog::default_logger());

}  // namespace
