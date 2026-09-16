/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_FILE_CACHE_H
#define PC_FILE_CACHE_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Lookup a file in the host in-memory cache (or loose directory overlay).
 * If found, copies the raw file bytes into dst, writes length to *size, and returns true.
 * If not found, returns false. */
bool pc_file_cache_get(const char* filename, void* dst, size_t* size);

/* Queries cached file size without copying data. Returns true if cached, false otherwise. */
bool pc_file_cache_get_size(const char* filename, size_t* size);

/* Stores a pristine copy of raw file data in host memory cache. */
void pc_file_cache_put(const char* filename, const void* data, size_t size);

/* Clear all entries from the file cache. */
void pc_file_cache_clear(void);

/* Set the directory path for loose file overrides (or reads MELEE_FILES_DIR). */
void pc_file_cache_set_loose_dir(const char* dir);

/* Launches a detached background pre-warming worker thread. */
void pc_file_cache_start_prewarm(void);

#ifdef __cplusplus
}
#endif

#endif /* PC_FILE_CACHE_H */
