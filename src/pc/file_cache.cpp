/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "file_cache.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <sys/stat.h>

#include <dolphin/dvd.h>
#include <dolphin/os.h>

namespace {
struct CachedEntry {
    std::vector<uint8_t> data;
};

std::unordered_map<std::string, CachedEntry> s_fileCache;
std::mutex s_cacheMutex;
std::string s_looseDir;

std::string normalize_key(const char* filename) {
    if (filename == nullptr) return "";
    std::string key = filename;
    while (!key.empty() && key[0] == '/') {
        key.erase(key.begin());
    }
    return key;
}

bool is_archive_name(const std::string& name) {
    if (name.size() < 4) return false;
    std::string ext = name.substr(name.size() - 4);
    for (char& c : ext) {
        c = static_cast<char>(std::tolower(c));
    }
    return (ext == ".dat" || ext == ".usd");
}

std::string resolve_loose_path(const char* filename) {
    if (filename == nullptr) return "";
    std::string dir = s_looseDir;
    if (dir.empty()) {
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
    }
    if (dir.empty()) return "";

    const char* rel = filename;
    while (*rel == '/') rel++;
    return dir + "/" + rel;
}

} // namespace

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
            const auto& entry = it->second;
            *size = entry.data.size();
            std::memcpy(dst, entry.data.data(), entry.data.size());
            OSReport("[FileCache] HIT: %s (%zu bytes, 0ms)\n", key.c_str(), entry.data.size());
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
                std::vector<uint8_t> buf(st.st_size);
                in.read(reinterpret_cast<char*>(buf.data()), st.st_size);
                *size = buf.size();
                std::memcpy(dst, buf.data(), buf.size());
                pc_file_cache_put(key.c_str(), buf.data(), buf.size());
                OSReport("[FileCache] LOOSE HIT: %s from %s (%zu bytes, 0ms)\n",
                         key.c_str(), loose_path.c_str(), buf.size());
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

    std::string key = normalize_key(filename);

    std::lock_guard<std::mutex> lock(s_cacheMutex);
    if (s_fileCache.find(key) != s_fileCache.end()) {
        return;
    }

    auto& entry = s_fileCache[key];
    const auto* src = static_cast<const uint8_t*>(data);
    entry.data.assign(src, src + size);
    OSReport("[FileCache] STORED: %s (%zu bytes)\n", key.c_str(), size);
}

void pc_file_cache_clear(void) {
    std::lock_guard<std::mutex> lock(s_cacheMutex);
    s_fileCache.clear();
}

void pc_file_cache_set_loose_dir(const char* dir) {
    if (dir != nullptr) {
        s_looseDir = dir;
    } else {
        s_looseDir.clear();
    }
}

void pc_file_cache_start_prewarm(void) {
    static std::atomic<bool> s_started{false};
    if (s_started.exchange(true)) {
        return;
    }

    const char* env_prewarm = getenv("MELEE_PREWARM");
    if (env_prewarm != nullptr && (strcmp(env_prewarm, "0") == 0 || strcmp(env_prewarm, "false") == 0)) {
        OSReport("[FileCache] Background prewarm disabled by MELEE_PREWARM=0\n");
        return;
    }

    std::thread worker([] {
        // Sleep 50ms so main thread finishes window initialization without contention
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        OSReport("[FileCache] Background asset prewarm started...\n");
        auto t_start = std::chrono::steady_clock::now();

        size_t loaded_count = 0;
        size_t loaded_bytes = 0;

        auto preload_file = [&](const char* name, int entryNum) {
            std::string key = normalize_key(name);

            {
                std::lock_guard<std::mutex> lock(s_cacheMutex);
                if (s_fileCache.find(key) != s_fileCache.end()) {
                    return;
                }
            }

            // Check if loose file exists
            std::string loose_path = resolve_loose_path(key.c_str());
            if (!loose_path.empty()) {
                struct stat st;
                if (stat(loose_path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
                    std::ifstream in(loose_path, std::ios::binary);
                    if (in.is_open()) {
                        std::vector<uint8_t> buf(st.st_size);
                        in.read(reinterpret_cast<char*>(buf.data()), st.st_size);
                        pc_file_cache_put(key.c_str(), buf.data(), buf.size());
                        loaded_count++;
                        loaded_bytes += buf.size();
                        return;
                    }
                }
            }

            // Read from DVD
            DVDFileInfo fi;
            BOOL opened = FALSE;
            if (entryNum >= 0) {
                opened = DVDFastOpen(entryNum, &fi);
            } else {
                opened = DVDOpen(name, &fi);
            }

            if (!opened) {
                return;
            }

            size_t file_len = fi.length;
            if (file_len > 0) {
                size_t aligned_sz = (file_len + 31) & ~31;
                void* raw_buf = nullptr;
                if (posix_memalign(&raw_buf, 32, aligned_sz) == 0 && raw_buf != nullptr) {
                    s32 bytesRead = DVDReadPrio(&fi, raw_buf, static_cast<s32>(aligned_sz), 0, 1);
                    if (bytesRead >= 0) {
                        pc_file_cache_put(key.c_str(), raw_buf, file_len);
                        loaded_count++;
                        loaded_bytes += file_len;
                    }
                    free(raw_buf);
                }
            }
            DVDClose(&fi);
        };

        // 1. High-priority common archives
        static const char* const s_priorityList[] = {
            "PlCo.dat", "EfCoData.dat", "EfMnData.dat",
            "MnMaAll.usd", "MnMaAll.dat",
            "MnSlChr.usd", "MnSlChr.dat",
            "MnSlMap.usd", "MnSlMap.dat",
            "MnExtAll.usd", "MnExtAll.dat",
            "SdSlChr.usd", "SdSlChr.dat",
            "IfAll.usd", "IfAll.dat",
            "ItCo.dat", "LbRb.dat",
            "PlFx.dat", "PlFxNr.dat", "EfFxData.dat",
            "PlMs.dat", "PlMsNr.dat", "EfMsData.dat",
            "PlFc.dat", "PlFcNr.dat", "EfFcData.dat",
            "PlSh.dat", "PlShNr.dat", "EfShData.dat",
            "PlCa.dat", "PlCaNr.dat", "EfCaData.dat",
            "PlPr.dat", "PlPrNr.dat", "EfPrData.dat",
            "PlPc.dat", "PlPcNr.dat", "EfPcData.dat",
            "GrNLa.dat", "GrSt.dat", "GrPs.dat", "GrOp.dat", "GrYs.dat"
        };

        for (const char* priority_file : s_priorityList) {
            preload_file(priority_file, -1);
        }

        // 2. Full directory scan of DVD root
        DVDDir dir;
        if (DVDOpenDir("/", &dir)) {
            DVDDirEntry dirent;
            while (DVDReadDir(&dir, &dirent)) {
                if (!dirent.isDir && dirent.name != nullptr) {
                    if (is_archive_name(dirent.name)) {
                        preload_file(dirent.name, static_cast<int>(dirent.entryNum));
                        std::this_thread::sleep_for(std::chrono::microseconds(200));
                    }
                }
            }
            DVDCloseDir(&dir);
        }

        auto t_end = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_start).count();
        OSReport("[FileCache] Prewarm complete: %zu archives (%.2f MB) cached in %ld ms\n",
                 loaded_count, loaded_bytes / (1024.0 * 1024.0), static_cast<long>(ms));
    });

    worker.detach();
}

} // extern "C"
