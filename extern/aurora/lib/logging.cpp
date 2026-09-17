#include "logging.hpp"

#include <fmt/base.h>
#include <fmt/format.h>
#include <aurora/aurora.h>

#include <cstdio>
#include <string_view>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

namespace aurora {
void log_internal(const AuroraLogLevel level, const char* module, const char* message,
                  const unsigned int len) noexcept {
  if (module == nullptr) {
    module = "";
  }
  if (g_config.logCallback == nullptr) {
#if defined(__ANDROID__)
    int priority = ANDROID_LOG_INFO;
    switch (level) {
    case LOG_DEBUG: priority = ANDROID_LOG_DEBUG; break;
    case LOG_INFO: priority = ANDROID_LOG_INFO; break;
    case LOG_WARNING: priority = ANDROID_LOG_WARN; break;
    case LOG_ERROR: priority = ANDROID_LOG_ERROR; break;
    case LOG_FATAL: priority = ANDROID_LOG_FATAL; break;
    }
    __android_log_print(priority, "Aurora", "[%s] %.*s", module, len, message);
#else
    fmt::println(stderr, "[{}] [{}] {}", level, module, std::string_view(message, len));
#endif
  } else {
    g_config.logCallback(level, module, message, len);
  }
}
} // namespace aurora

auto fmt::formatter<AuroraLogLevel>::format(const AuroraLogLevel level, format_context& ctx) const
    -> format_context::iterator {
  std::string_view name = "unknown";
  switch (level) {
  case LOG_DEBUG:
    name = "debug";
    break;
  case LOG_INFO:
    name = "info";
    break;
  case LOG_WARNING:
    name = "warning";
    break;
  case LOG_ERROR:
    name = "error";
    break;
  case LOG_FATAL:
    name = "fatal";
    break;
  default:
    break;
  }
  return formatter<std::string_view>::format(name, ctx);
}
