#ifndef AURORA_GFX_H
#define AURORA_GFX_H

#ifdef __cplusplus
#include <cstdint>

extern "C" {
#else
#include "stdint.h"
#endif

#if !defined(NDEBUG) && !defined(AURORA_GFX_DEBUG_GROUPS)
#define AURORA_GFX_DEBUG_GROUPS
#endif

void push_debug_group(const char* label);
void pop_debug_group();

typedef struct {
  uint32_t queuedPipelines;
  uint32_t createdPipelines;
  uint32_t drawCallCount;
  uint32_t mergedDrawCallCount;
  uint32_t lastVertSize;
  uint32_t lastUniformSize;
  uint32_t lastIndexSize;
  uint32_t lastStorageSize;
  uint32_t lastTextureUploadSize;
} AuroraStats;

const AuroraStats* aurora_get_stats();
float aurora_get_fps();

void aurora_enable_vsync(bool enabled);
bool aurora_vsync_enabled(void);

/* Blocks up to max_wait_ms while queued pipelines compile -- without worker
 * threads, compiles them on the calling thread for about that long instead --
 * and returns how many are still pending. A load screen can spend its idle
 * time here so known pipeline configs become compiled PSOs before a match,
 * not during. */
uint32_t aurora_wait_pipelines(uint32_t max_wait_ms);

#ifdef __cplusplus
}
#endif

#endif
