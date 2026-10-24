#pragma once

#if defined(__ANDROID__)
#include <android/log.h>
#include <android/performance_hint.h>
#include <cstdlib>
#include <dlfcn.h>
#include <unistd.h>
#endif
#include <cstdint>

namespace aurora {
/* Per-thread sessions avoid cross-thread access and stale worker TIDs. Native
 * API 33 symbols are optional so the APK still loads on minimum API 26. */
class PerformanceHints {
#if defined(__ANDROID__)
  struct Api {
    // Exact NDK signatures; decltype(&function) is unavailable at API 26.
    APerformanceHintManager* (*manager)() = reinterpret_cast<decltype(manager)>(
        dlsym(RTLD_DEFAULT, "APerformanceHint_getManager"));
    APerformanceHintSession* (*create)(APerformanceHintManager*, const int32_t*, size_t, int64_t) = reinterpret_cast<decltype(create)>(
        dlsym(RTLD_DEFAULT, "APerformanceHint_createSession"));
    int (*report)(APerformanceHintSession*, int64_t) = reinterpret_cast<decltype(report)>(
        dlsym(RTLD_DEFAULT, "APerformanceHint_reportActualWorkDuration"));
    int (*update)(APerformanceHintSession*, int64_t) = reinterpret_cast<decltype(update)>(
        dlsym(RTLD_DEFAULT, "APerformanceHint_updateTargetWorkDuration"));
    void (*close)(APerformanceHintSession*) = reinterpret_cast<decltype(close)>(
        dlsym(RTLD_DEFAULT, "APerformanceHint_closeSession"));
  };
  static const Api& api() { static const Api value; return value; }
  APerformanceHintSession* session = nullptr;
  bool attempted = false;
  int64_t target = 0;
  bool enabled() const {
    const char* value = std::getenv("MELEE_ANDROID_PERF_HINTS");
    return value != nullptr && value[0] == '1' && value[1] == '\0';
  }
#endif
public:
  PerformanceHints() = default;
  PerformanceHints(const PerformanceHints&) = delete;
  PerformanceHints& operator=(const PerformanceHints&) = delete;
  ~PerformanceHints() { reset(); }
  void reset() {
#if defined(__ANDROID__)
    if (session) api().close(session);
    session = nullptr;
    attempted = false;
    target = 0;
#endif
  }
  void report(uint64_t durationNs, uint64_t targetNs) {
#if defined(__ANDROID__)
    if (!enabled() || durationNs == 0 || targetNs == 0 || durationNs > INT64_MAX || targetNs > INT64_MAX) return;
    const auto& a = api();
    if (!attempted) {
      attempted = true;
      const int32_t tid = gettid();
      if (a.manager && a.create && a.report && a.update && a.close) {
        if (auto* manager = a.manager()) session = a.create(manager, &tid, 1, static_cast<int64_t>(targetNs));
      }
      target = static_cast<int64_t>(targetNs);
      __android_log_print(ANDROID_LOG_INFO, "Aurora", "ADPF tid %d: %s", tid,
                          session ? "session active" : "unsupported; using normal scheduling");
    }
    if (!session) return;
    int error = 0;
    if (target != static_cast<int64_t>(targetNs)) {
      error = a.update(session, static_cast<int64_t>(targetNs));
      target = static_cast<int64_t>(targetNs);
    }
    if (!error) error = a.report(session, static_cast<int64_t>(durationNs));
    if (error) {
      __android_log_print(ANDROID_LOG_INFO, "Aurora", "ADPF report failed (%d); using normal scheduling", error);
      a.close(session);
      session = nullptr;
    }
#else
    (void)durationNs;
    (void)targetNs;
#endif
  }
};
}
