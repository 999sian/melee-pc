/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "file_cache.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <list>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/resource.h>
#endif

#include <dolphin/ar.h>
#include <dolphin/dvd.h>
#include <dolphin/os.h>

namespace {

struct CachedEntry {
    std::vector<uint8_t> data;
    bool pinned = false;
    std::list<std::string>::iterator lru_it;
};

std::unordered_map<std::string, CachedEntry> s_fileCache;
std::list<std::string> s_lruList;  // Front = most recently accessed, Back = least recently accessed
size_t s_totalCacheBytes = 0;
size_t s_maxCacheBytes = 0;  // 0 = uninitialized, will be auto-detected
std::mutex s_cacheMutex;

std::string normalize_key(const char* filename) {
    if (filename == nullptr)
        return "";
    std::string key = filename;
    while (!key.empty() && key[0] == '/') {
        key.erase(key.begin());
    }
    return key;
}

bool is_archive_name(const std::string& name) {
    if (name.size() < 4)
        return false;
    std::string ext = name.substr(name.size() - 4);
    for (char& c : ext) {
        c = static_cast<char>(std::tolower(c));
    }
    return (ext == ".dat" || ext == ".usd");
}

bool is_pinned_file(const std::string& key) {
    // Core system, UI, menu, and common tables that must never be evicted by LRU
    static const char* const s_pinned[] = {"PlCo.dat", "EfCoData.dat", "EfMnData.dat",
        "MnMaAll.usd", "MnMaAll.dat", "MnSlChr.usd", "MnSlChr.dat", "MnSlMap.usd", "MnSlMap.dat",
        "MnExtAll.usd", "MnExtAll.dat", "SdSlChr.usd", "SdSlChr.dat", "IfAll.usd", "IfAll.dat",
        "ItCo.dat", "LbRb.dat", "LbMcGame.usd", "NtMemAc.usd", "SdMenu.usd", "SdIntro.dat",
        "GmPause.usd", "IfCoGet.dat", "LbBf.dat",
        /* Pokemon Stadium's transformations: loaded mid-fight, and netplay
         * re-simulates that load from memory (pc_net_pure_load), so an
         * eviction between a load and its re-run must not happen. */
        "GrPs1.dat", "GrPs2.dat", "GrPs3.dat", "GrPs4.dat"};
    for (const char* p : s_pinned) {
        if (key == p)
            return true;
    }
    return false;
}

size_t detect_system_ram_mb() {
#if defined(_SC_PHYS_PAGES) && defined(_SC_PAGE_SIZE)
    int64_t pages = sysconf(_SC_PHYS_PAGES);
    int64_t page_size = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && page_size > 0) {
        return static_cast<size_t>((pages * page_size) / (1024 * 1024));
    }
#endif
    return 8192;  // default fallback assumption
}

enum CacheProfile {
    PROFILE_DESKTOP,   // > 4 GB RAM: 512 MB budget, full pre-warm (all 889 archives)
    PROFILE_HANDHELD,  // 2 - 4 GB RAM (Switch, Steam Deck, 4 GB PCs): 64 MB budget, Tier 1 pre-warm
    PROFILE_LOW_RAM    // <= 2 GB RAM (Pi 4, low-RAM SBCs): 24 MB budget, Tier 1 essentials
};

CacheProfile get_effective_profile() {
    const char* env_mb = getenv("MELEE_CACHE_MAX_MB");
    if (env_mb != nullptr && env_mb[0] != '\0') {
        size_t mb = static_cast<size_t>(strtoul(env_mb, nullptr, 10));
        if (mb <= 24)
            return PROFILE_LOW_RAM;
        if (mb <= 96)
            return PROFILE_HANDHELD;
        return PROFILE_DESKTOP;
    }

    size_t ram_mb = detect_system_ram_mb();
    if (ram_mb <= 2048) {
        return PROFILE_LOW_RAM;
    } else if (ram_mb <= 4096) {
        return PROFILE_HANDHELD;
    }
    return PROFILE_DESKTOP;
}

size_t get_default_budget_bytes() {
    const char* env_mb = getenv("MELEE_CACHE_MAX_MB");
    if (env_mb != nullptr && env_mb[0] != '\0') {
        return static_cast<size_t>(strtoul(env_mb, nullptr, 10)) * 1024 * 1024;
    }

    switch (get_effective_profile()) {
    case PROFILE_LOW_RAM:
        return 24 * 1024 * 1024;  // 24 MB
    case PROFILE_HANDHELD:
        return 64 * 1024 * 1024;  // 64 MB
    case PROFILE_DESKTOP:
    default:
        return 512 * 1024 * 1024;  // 512 MB
    }
}

void evict_lru_locked(size_t required_bytes) {
    if (s_maxCacheBytes == 0) {
        s_maxCacheBytes = get_default_budget_bytes();
    }

    while (s_totalCacheBytes + required_bytes > s_maxCacheBytes && !s_lruList.empty()) {
        std::string evict_key = s_lruList.back();
        s_lruList.pop_back();

        auto it = s_fileCache.find(evict_key);
        if (it != s_fileCache.end()) {
            size_t entry_size = it->second.data.size();
            s_totalCacheBytes =
                (s_totalCacheBytes >= entry_size) ? (s_totalCacheBytes - entry_size) : 0;
            s_fileCache.erase(it);
            OSReport("[FileCache] LRU EVICTED: %s (freed %zu bytes, current total: %.2f MB)\n",
                evict_key.c_str(), entry_size, s_totalCacheBytes / (1024.0 * 1024.0));
        }
    }
}

std::string resolve_loose_path(const char* filename) {
    if (filename == nullptr)
        return "";
    std::string dir;
    const char* env = getenv("MELEE_FILES_DIR");
    if (env != nullptr && env[0] != '\0') {
        dir = env;
    } else {
        struct stat st;
        if (stat("./files", &st) == 0 && S_ISDIR(st.st_mode)) {
            dir = "./files";
        } else if (stat("../iso/extracted_usa/files", &st) == 0 && S_ISDIR(st.st_mode)) {
            dir = "../iso/extracted_usa/files";
        }
    }
    if (dir.empty())
        return "";

    const char* rel = filename;
    while (*rel == '/')
        rel++;
    return dir + "/" + rel;
}

bool preload_single_file(const char* name, int entryNum) {
    std::string key = normalize_key(name);

    {
        std::lock_guard<std::mutex> lock(s_cacheMutex);
        if (s_fileCache.find(key) != s_fileCache.end()) {
            return false;
        }
    }

    // 1. Check if loose file exists
    std::string loose_path = resolve_loose_path(key.c_str());
    if (!loose_path.empty()) {
        struct stat st;
        if (stat(loose_path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            std::ifstream in(loose_path, std::ios::binary);
            if (in.is_open()) {
                std::vector<uint8_t> buf(st.st_size);
                in.read(reinterpret_cast<char*>(buf.data()), st.st_size);
                pc_file_cache_put(key.c_str(), buf.data(), buf.size());
                return true;
            }
        }
    }

    // 2. Read from DVD
    DVDFileInfo fi;
    BOOL opened = FALSE;
    if (entryNum >= 0) {
        opened = DVDFastOpen(entryNum, &fi);
    } else {
        opened = DVDOpen(name, &fi);
    }

    if (!opened) {
        return false;
    }

    size_t file_len = fi.length;
    bool success = false;
    if (file_len > 0) {
        size_t aligned_sz = (file_len + 31) & ~31;
        uint8_t* raw_mem = static_cast<uint8_t*>(malloc(aligned_sz + 32));
        if (raw_mem != nullptr) {
            void* raw_buf = reinterpret_cast<void*>(
                (reinterpret_cast<uintptr_t>(raw_mem) + 31) & ~uintptr_t(31));
            s32 bytesRead = DVDReadPrio(&fi, raw_buf, static_cast<s32>(aligned_sz), 0, 1);
            if (bytesRead >= 0) {
                pc_file_cache_put(key.c_str(), raw_buf, file_len);
                success = true;
            }
            free(raw_mem);
        }
    }
    DVDClose(&fi);
    return success;
}

#ifndef PC_IS_ARAM_ADDR
#define PC_IS_ARAM_ADDR(a) ((uintptr_t)(a) < 0x01000000u)
#endif

uint8_t* resolve_host_dst(void* dst, size_t size) {
    if (dst == nullptr) {
        return nullptr;
    }
    if (PC_IS_ARAM_ADDR(dst)) {
        uint8_t* aram = aurora_aram_base();
        if (aram == nullptr) {
            return nullptr;
        }
        uintptr_t offset = reinterpret_cast<uintptr_t>(dst);
        if (offset + size > 0x01000000u) {
            return nullptr;
        }
        return aram + offset;
    }
    return static_cast<uint8_t*>(dst);
}

const uint8_t* resolve_host_src(const void* src, size_t size) {
    if (src == nullptr) {
        return nullptr;
    }
    if (PC_IS_ARAM_ADDR(src)) {
        uint8_t* aram = aurora_aram_base();
        if (aram == nullptr) {
            return nullptr;
        }
        uintptr_t offset = reinterpret_cast<uintptr_t>(src);
        if (offset + size > 0x01000000u) {
            return nullptr;
        }
        return aram + offset;
    }
    return static_cast<const uint8_t*>(src);
}

}  // namespace

extern "C" {

bool pc_file_cache_get(const char* filename, void* dst, size_t* size) {
    if (filename == nullptr || dst == nullptr || size == nullptr) {
        return false;
    }

    std::string key = normalize_key(filename);

    {
        std::lock_guard<std::mutex> lock(s_cacheMutex);
        auto it = s_fileCache.find(key);
        if (it != s_fileCache.end()) {
            auto& entry = it->second;
            uint8_t* host_dst = resolve_host_dst(dst, entry.data.size());
            if (host_dst == nullptr) {
                return false;
            }
            *size = entry.data.size();
            std::memcpy(host_dst, entry.data.data(), entry.data.size());

            // Move to front of LRU queue if unpinned
            if (!entry.pinned && entry.lru_it != s_lruList.begin()) {
                s_lruList.erase(entry.lru_it);
                s_lruList.push_front(key);
                entry.lru_it = s_lruList.begin();
            }

            OSReport("[FileCache] HIT: %s (%zu bytes, 0ms)%s\n", key.c_str(), entry.data.size(),
                PC_IS_ARAM_ADDR(dst) ? " [ARAM]" : "");
            return true;
        }
    }

    // Check loose file directory override
    std::string loose_path = resolve_loose_path(key.c_str());
    if (!loose_path.empty()) {
        struct stat st;
        if (stat(loose_path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            std::ifstream in(loose_path, std::ios::binary);
            if (in.is_open()) {
                uint8_t* host_dst = resolve_host_dst(dst, static_cast<size_t>(st.st_size));
                if (host_dst == nullptr) {
                    return false;
                }
                std::vector<uint8_t> buf(st.st_size);
                in.read(reinterpret_cast<char*>(buf.data()), st.st_size);
                *size = buf.size();
                std::memcpy(host_dst, buf.data(), buf.size());
                pc_file_cache_put(key.c_str(), buf.data(), buf.size());
                OSReport("[FileCache] LOOSE HIT: %s from %s (%zu bytes, 0ms)%s\n", key.c_str(),
                    loose_path.c_str(), buf.size(), PC_IS_ARAM_ADDR(dst) ? " [ARAM]" : "");
                return true;
            }
        }
    }

    return false;
}

bool pc_file_cache_get_size(const char* filename, size_t* size) {
    if (filename == nullptr || size == nullptr) {
        return false;
    }

    std::string key = normalize_key(filename);

    {
        std::lock_guard<std::mutex> lock(s_cacheMutex);
        auto it = s_fileCache.find(key);
        if (it != s_fileCache.end()) {
            *size = it->second.data.size();
            return true;
        }
    }

    std::string loose_path = resolve_loose_path(key.c_str());
    if (!loose_path.empty()) {
        struct stat st;
        if (stat(loose_path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            *size = static_cast<size_t>(st.st_size);
            return true;
        }
    }

    return false;
}

void pc_file_cache_put(const char* filename, const void* data, size_t size) {
    if (filename == nullptr || data == nullptr || size == 0) {
        return;
    }

    const uint8_t* host_src = resolve_host_src(data, size);
    if (host_src == nullptr) {
        return;
    }

    std::string key = normalize_key(filename);

    std::lock_guard<std::mutex> lock(s_cacheMutex);
    if (s_fileCache.find(key) != s_fileCache.end()) {
        return;
    }

    bool pinned = is_pinned_file(key);
    if (!pinned) {
        evict_lru_locked(size);
    }

    auto& entry = s_fileCache[key];
    entry.data.assign(host_src, host_src + size);
    entry.pinned = pinned;

    if (!pinned) {
        s_lruList.push_front(key);
        entry.lru_it = s_lruList.begin();
    }
    s_totalCacheBytes += size;

    OSReport("[FileCache] STORED: %s (%zu bytes%s, total: %.2f MB)%s\n", key.c_str(), size,
        pinned ? ", pinned" : "", s_totalCacheBytes / (1024.0 * 1024.0),
        PC_IS_ARAM_ADDR(data) ? " [from ARAM]" : "");
}

bool pc_file_cache_require(const char* filename) {
    size_t size;
    if (pc_file_cache_get_size(filename, &size)) {
        return true;
    }
    preload_single_file(filename, -1);
    return pc_file_cache_get_size(filename, &size);
}

void pc_file_cache_start_prewarm(void) {
    static std::atomic<bool> s_started{false};
    if (s_started.exchange(true)) {
        return;
    }

    const char* env_prewarm = getenv("MELEE_PREWARM");
    if (env_prewarm != nullptr &&
        (strcmp(env_prewarm, "0") == 0 || strcmp(env_prewarm, "false") == 0))
    {
        OSReport("[FileCache] Background prewarm disabled by MELEE_PREWARM=0\n");
        return;
    }

    std::thread worker([] {
        // Yield to allow main game loop and window initialization to proceed without contention
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // Demote worker priority on Linux / 4-core / handheld devices to avoid stealing gameplay
        // cycles
#if defined(__linux__)
        setpriority(PRIO_PROCESS, 0, 10);
#endif

        CacheProfile profile = get_effective_profile();
        if (s_maxCacheBytes == 0) {
            s_maxCacheBytes = get_default_budget_bytes();
        }

        const char* profile_name = (profile == PROFILE_LOW_RAM)  ? "Low-RAM (<=2GB)" :
                                   (profile == PROFILE_HANDHELD) ? "Handheld/Switch (2-4GB)" :
                                                                   "Desktop (>4GB)";
        OSReport("[FileCache] Starting prewarm [Profile: %s, Budget: %zu MB]...\n", profile_name,
            s_maxCacheBytes / (1024 * 1024));

        auto t_start = std::chrono::steady_clock::now();

        // High-priority core tournament and UI archives (Tier 1)
        static const char* const s_priorityList[] = {"PlCo.dat", "EfCoData.dat", "EfMnData.dat",
            "MnMaAll.usd", "MnMaAll.dat", "MnSlChr.usd", "MnSlChr.dat", "MnSlMap.usd",
            "MnSlMap.dat", "MnExtAll.usd", "MnExtAll.dat", "SdSlChr.usd", "SdSlChr.dat",
            "IfAll.usd", "IfAll.dat", "ItCo.dat", "LbRb.dat", "PlFx.dat", "PlFxNr.dat",
            "EfFxData.dat", "PlMs.dat", "PlMsNr.dat", "EfMsData.dat", "PlFc.dat", "PlFcNr.dat",
            "EfFcData.dat", "PlSh.dat", "PlShNr.dat", "EfShData.dat", "PlCa.dat", "PlCaNr.dat",
            "EfCaData.dat", "PlPr.dat", "PlPrNr.dat", "EfPrData.dat", "PlPc.dat", "PlPcNr.dat",
            "EfPcData.dat", "GrNLa.dat", "GrSt.dat", "GrPs.dat", "GrOp.dat", "GrYs.dat"};

        auto sleep_duration = (profile == PROFILE_DESKTOP)  ? std::chrono::microseconds(200) :
                              (profile == PROFILE_HANDHELD) ? std::chrono::milliseconds(5) :
                                                              std::chrono::milliseconds(15);

        for (const char* priority_file : s_priorityList) {
            preload_single_file(priority_file, -1);
            std::this_thread::sleep_for(sleep_duration);
        }

        // On desktop profile (or if MELEE_PREWARM_MODE=full), continue with full directory scan
        const char* mode_env = getenv("MELEE_PREWARM_MODE");
        bool full_scan =
            (profile == PROFILE_DESKTOP) || (mode_env != nullptr && strcmp(mode_env, "full") == 0);

        if (full_scan) {
            DVDDir dir;
            if (DVDOpenDir("/", &dir)) {
                DVDDirEntry dirent;
                while (DVDReadDir(&dir, &dirent)) {
                    if (!dirent.isDir && dirent.name != nullptr) {
                        if (is_archive_name(dirent.name)) {
                            preload_single_file(dirent.name, static_cast<int>(dirent.entryNum));
                            std::this_thread::sleep_for(sleep_duration);
                        }
                    }
                }
                DVDCloseDir(&dir);
            }
        }

        auto t_end = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_start).count();
        size_t total_mb;
        size_t total_files;
        {
            std::lock_guard<std::mutex> lock(s_cacheMutex);
            total_mb = s_totalCacheBytes / (1024 * 1024);
            total_files = s_fileCache.size();
        }
        OSReport("[FileCache] Prewarm finished: %zu archives (%zu MB) cached in %u ms\n",
            total_files, total_mb, static_cast<uint32_t>(ms));
    });

    worker.detach();
}

}  // extern "C"
