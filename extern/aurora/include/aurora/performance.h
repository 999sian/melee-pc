#pragma once

#include <stdbool.h>
#include <stdint.h>
#ifdef __ANDROID__
#include <android/trace.h>
#endif

static inline bool aurora_trace_begin(const char* name) {
#ifdef __ANDROID__
  if (ATrace_isEnabled()) {
    ATrace_beginSection(name);
    return true;
  }
#else
  (void)name;
#endif
  return false;
}

static inline void aurora_trace_end(bool active) {
#ifdef __ANDROID__
  if (active) ATrace_endSection();
#else
  (void)active;
#endif
}

#ifdef __cplusplus
extern "C" {
#endif
/* Optional Android trial. Report active work, excluding deliberate pacing
 * sleeps. A zero target closes this thread's session (pause/shutdown). */
void aurora_report_frame_work(uint64_t duration_ns, uint64_t target_ns);
#ifdef __cplusplus
}

namespace aurora {
class AndroidTraceScope {
  bool active;
public:
  explicit AndroidTraceScope(const char* name) : active(aurora_trace_begin(name)) {}
  ~AndroidTraceScope() { aurora_trace_end(active); }
};
}
#endif
