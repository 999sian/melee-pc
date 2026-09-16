/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "file_cache.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <dolphin/os.h>

namespace {
struct CachedEntry {
    std::vector<uint8_t> data;
};

std::unordered_map<std::string, CachedEntry> s_fileCache;
std::mutex s_cacheMutex;
} // namespace

extern "C" {

bool pc_file_cache_get(const char* filename, void* dst, size_t* size) {
    if (filename == nullptr || dst == nullptr || size == nullptr) {
        return false;
    }

    std::lock_guard<std::mutex> lock(s_cacheMutex);
    auto it = s_fileCache.find(filename);
    if (it == s_fileCache.end()) {
        return false;
    }

    const auto& entry = it->second;
    *size = entry.data.size();
    std::memcpy(dst, entry.data.data(), entry.data.size());
    OSReport("[FileCache] HIT: %s (%zu bytes, 0ms)\n", filename, entry.data.size());
    return true;
}

void pc_file_cache_put(const char* filename, const void* data, size_t size) {
    if (filename == nullptr || data == nullptr || size == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(s_cacheMutex);
    if (s_fileCache.find(filename) != s_fileCache.end()) {
        return;
    }

    auto& entry = s_fileCache[filename];
    const auto* src = static_cast<const uint8_t*>(data);
    entry.data.assign(src, src + size);
    OSReport("[FileCache] STORED: %s (%zu bytes)\n", filename, size);
}

void pc_file_cache_clear(void) {
    std::lock_guard<std::mutex> lock(s_cacheMutex);
    s_fileCache.clear();
}

} // extern "C"
