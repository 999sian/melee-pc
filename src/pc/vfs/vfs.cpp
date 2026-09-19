/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "vfs.hpp"

#include <aurora/dvd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace pc::vfs {

namespace {

std::mutex s_vfs_mutex;
std::vector<VfsMount> s_mounts;
std::unordered_map<std::string, std::filesystem::path> s_resolve_cache;
bool s_initialized = false;

bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
        {
            return false;
        }
    }
    return true;
}

// Case-insensitive segment lookup inside an existing directory
std::optional<std::filesystem::path> find_case_insensitive_segment(
    const std::filesystem::path& parent, const std::string& target_segment) {
    std::error_code ec;
    if (!std::filesystem::exists(parent, ec) || !std::filesystem::is_directory(parent, ec)) {
        return std::nullopt;
    }

    // Check direct path first (fast path)
    std::filesystem::path direct = parent / target_segment;
    if (std::filesystem::exists(direct, ec)) {
        return direct;
    }

    // Slow path: scan directory for matching name ignoring case
    for (const auto& entry : std::filesystem::directory_iterator(parent, ec)) {
        if (ec)
            break;
        if (iequals(entry.path().filename().string(), target_segment)) {
            return entry.path();
        }
    }

    return std::nullopt;
}

// Walks through a relative path component-by-component case-insensitively
std::optional<std::filesystem::path> find_file_in_root(
    const std::filesystem::path& root, const std::string& rel_path) {
    std::filesystem::path current = root;
    std::string remaining = rel_path;

    // Direct check first (fastest)
    std::error_code ec;
    std::filesystem::path direct = root / rel_path;
    if (std::filesystem::exists(direct, ec) && std::filesystem::is_regular_file(direct, ec)) {
        return direct;
    }

    size_t start = 0;
    while (start < remaining.size()) {
        size_t next_slash = remaining.find_first_of("/\\", start);
        std::string segment = (next_slash == std::string::npos) ?
                                  remaining.substr(start) :
                                  remaining.substr(start, next_slash - start);
        if (!segment.empty() && segment != ".") {
            auto found = find_case_insensitive_segment(current, segment);
            if (!found.has_value()) {
                return std::nullopt;
            }
            current = *found;
        }

        if (next_slash == std::string::npos) {
            break;
        }
        start = next_slash + 1;
    }

    if (std::filesystem::exists(current, ec) && std::filesystem::is_regular_file(current, ec)) {
        return current;
    }

    return std::nullopt;
}

void sort_mounts_locked() {
    std::stable_sort(s_mounts.begin(), s_mounts.end(),
        [](const VfsMount& a, const VfsMount& b) { return a.priority > b.priority; });
}

}  // namespace

std::string normalize_virtual_path(const std::string& path) {
    std::string norm = path;
    for (char& c : norm) {
        if (c == '\\') {
            c = '/';
        }
    }
    while (!norm.empty() && norm.front() == '/') {
        norm.erase(norm.begin());
    }
    return norm;
}

void init(const std::filesystem::path& base_dir) {
    std::lock_guard<std::mutex> lock(s_vfs_mutex);
    s_mounts.clear();
    s_resolve_cache.clear();
    s_initialized = true;

    if (!base_dir.empty()) {
        std::error_code ec;
        if (std::filesystem::exists(base_dir, ec) && std::filesystem::is_directory(base_dir, ec)) {
            s_mounts.push_back(VfsMount{
                .name = "base",
                .root_path = base_dir,
                .priority = 10,
                .enabled = true,
            });
        }
    }
}

void reset() {
    std::lock_guard<std::mutex> lock(s_vfs_mutex);
    s_mounts.clear();
    s_resolve_cache.clear();
}

bool mount(const std::string& name, const std::filesystem::path& path, int priority) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || !std::filesystem::is_directory(path, ec)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(s_vfs_mutex);
    for (auto& m : s_mounts) {
        if (m.name == name) {
            m.root_path = path;
            m.priority = priority;
            m.enabled = true;
            sort_mounts_locked();
            s_resolve_cache.clear();
            return true;
        }
    }

    s_mounts.push_back(VfsMount{
        .name = name,
        .root_path = path,
        .priority = priority,
        .enabled = true,
    });
    sort_mounts_locked();
    s_resolve_cache.clear();
    return true;
}

bool unmount(const std::string& name) {
    std::lock_guard<std::mutex> lock(s_vfs_mutex);
    for (auto it = s_mounts.begin(); it != s_mounts.end(); ++it) {
        if (it->name == name) {
            s_mounts.erase(it);
            s_resolve_cache.clear();
            return true;
        }
    }
    return false;
}

bool set_mount_enabled(const std::string& name, bool enabled) {
    std::lock_guard<std::mutex> lock(s_vfs_mutex);
    for (auto& m : s_mounts) {
        if (m.name == name) {
            if (m.enabled != enabled) {
                m.enabled = enabled;
                s_resolve_cache.clear();
            }
            return true;
        }
    }
    return false;
}

std::filesystem::path resolve_path(const std::string& virtual_path) {
    std::string norm = normalize_virtual_path(virtual_path);
    if (norm.empty()) {
        return {};
    }

    std::lock_guard<std::mutex> lock(s_vfs_mutex);
    auto cache_it = s_resolve_cache.find(norm);
    if (cache_it != s_resolve_cache.end()) {
        return cache_it->second;
    }

    // Build candidates: with and without "files/" prefix
    std::vector<std::string> candidates;
    candidates.push_back(norm);
    if (norm.rfind("files/", 0) == 0) {
        candidates.push_back(norm.substr(6));
    } else {
        candidates.push_back("files/" + norm);
    }

    for (const auto& m : s_mounts) {
        if (!m.enabled)
            continue;

        for (const auto& cand : candidates) {
            auto found = find_file_in_root(m.root_path, cand);
            if (found.has_value()) {
                s_resolve_cache[norm] = *found;
                return *found;
            }
        }
    }

    return {};
}

bool has_file(const std::string& virtual_path) {
    return !resolve_path(virtual_path).empty();
}

std::vector<uint8_t> read_file(const std::string& virtual_path) {
    std::filesystem::path host_path = resolve_path(virtual_path);
    if (host_path.empty()) {
        return {};
    }

    std::ifstream in(host_path, std::ios::binary | std::ios::ate);
    if (!in.is_open()) {
        return {};
    }

    std::streamsize size = in.tellg();
    if (size <= 0) {
        return {};
    }

    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> buffer(static_cast<size_t>(size));
    if (in.read(reinterpret_cast<char*>(buffer.data()), size)) {
        return buffer;
    }

    return {};
}

std::vector<std::string> list_files(const std::string& virtual_dir, bool recursive) {
    std::lock_guard<std::mutex> lock(s_vfs_mutex);
    std::vector<std::string> results;
    std::unordered_map<std::string, bool> seen;
    std::string norm_dir = normalize_virtual_path(virtual_dir);

    for (const auto& m : s_mounts) {
        if (!m.enabled)
            continue;

        std::filesystem::path target_dir = m.root_path;
        if (!norm_dir.empty()) {
            target_dir /= norm_dir;
        }

        std::error_code ec;
        if (!std::filesystem::exists(target_dir, ec) ||
            !std::filesystem::is_directory(target_dir, ec))
        {
            // Also check under files/
            target_dir = m.root_path / "files" / norm_dir;
            if (!std::filesystem::exists(target_dir, ec) ||
                !std::filesystem::is_directory(target_dir, ec))
            {
                continue;
            }
        }

        if (recursive) {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(target_dir, ec))
            {
                if (ec)
                    break;
                if (entry.is_regular_file(ec)) {
                    std::string rel =
                        std::filesystem::relative(entry.path(), m.root_path, ec).string();
                    if (!seen[rel]) {
                        seen[rel] = true;
                        results.push_back(rel);
                    }
                }
            }
        } else {
            for (const auto& entry : std::filesystem::directory_iterator(target_dir, ec)) {
                if (ec)
                    break;
                if (entry.is_regular_file(ec)) {
                    std::string rel =
                        std::filesystem::relative(entry.path(), m.root_path, ec).string();
                    if (!seen[rel]) {
                        seen[rel] = true;
                        results.push_back(rel);
                    }
                }
            }
        }
    }

    return results;
}

std::vector<VfsMount> get_mounts() {
    std::lock_guard<std::mutex> lock(s_vfs_mutex);
    return s_mounts;
}

namespace {

static void* vfs_overlay_open(void* userdata) {
    if (!userdata)
        return nullptr;
    const char* path = static_cast<const char*>(userdata);
    return fopen(path, "rb");
}

static void vfs_overlay_close(void* handle) {
    if (handle) {
        fclose(static_cast<FILE*>(handle));
    }
}

static int64_t vfs_overlay_read(void* handle, uint8_t* buf, size_t len) {
    if (!handle || !buf || len == 0)
        return 0;
    size_t bytes = fread(buf, 1, len, static_cast<FILE*>(handle));
    return static_cast<int64_t>(bytes);
}

static int64_t vfs_overlay_seek(void* handle, int64_t offset, int32_t whence) {
    if (!handle)
        return -1;
    if (fseek(static_cast<FILE*>(handle), static_cast<long>(offset),  // NOLINT: fseek expects long
            whence) != 0)
    {
        return -1;
    }
    return ftell(static_cast<FILE*>(handle));
}

static const AuroraOverlayCallbacks s_overlay_callbacks = {
    .open = vfs_overlay_open,
    .close = vfs_overlay_close,
    .read = vfs_overlay_read,
    .seek = vfs_overlay_seek,
};

struct OverlayData {
    std::vector<std::string> file_names;
    std::vector<std::string> host_paths;
    std::vector<AuroraOverlayFile> overlay_files;
};
static OverlayData s_overlay_data;

}  // namespace

void sync_dvd_overlays() {
    std::lock_guard<std::mutex> lock(s_vfs_mutex);

    s_overlay_data.file_names.clear();
    s_overlay_data.host_paths.clear();
    s_overlay_data.overlay_files.clear();

    std::unordered_map<std::string, bool> seen_rel;

    for (const auto& m : s_mounts) {
        if (!m.enabled)
            continue;

        std::error_code ec;
        std::filesystem::path search_dir = m.root_path;
        if (!std::filesystem::exists(search_dir, ec) ||
            !std::filesystem::is_directory(search_dir, ec))
        {
            continue;
        }

        for (const auto& entry : std::filesystem::recursive_directory_iterator(search_dir, ec)) {
            if (ec)
                break;
            if (entry.is_regular_file(ec)) {
                std::string rel = std::filesystem::relative(entry.path(), search_dir, ec).string();
                for (char& c : rel) {
                    if (c == '\\')
                        c = '/';
                }
                if (rel.rfind("files/", 0) == 0) {
                    rel = rel.substr(6);
                }

                std::string lower_rel = rel;
                for (char& c : lower_rel) {
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }

                if (seen_rel.find(lower_rel) != seen_rel.end()) {
                    continue;
                }
                seen_rel[lower_rel] = true;

                std::string dvd_path = "/" + rel;
                s_overlay_data.file_names.push_back(dvd_path);
                s_overlay_data.host_paths.push_back(entry.path().string());

                auto file_sz = entry.file_size(ec);
                if (ec)
                    file_sz = 0;

                AuroraOverlayFile f;
                f.fileName = nullptr;
                f.userData = nullptr;
                f.size = static_cast<size_t>(file_sz);
                s_overlay_data.overlay_files.push_back(f);
            }
        }
    }

    for (size_t i = 0; i < s_overlay_data.overlay_files.size(); ++i) {
        s_overlay_data.overlay_files[i].fileName = s_overlay_data.file_names[i].c_str();
        s_overlay_data.overlay_files[i].userData =
            const_cast<char*>(s_overlay_data.host_paths[i].c_str());
    }

    aurora_dvd_overlay_callbacks(&s_overlay_callbacks);
    if (!s_overlay_data.overlay_files.empty()) {
        aurora_dvd_overlay_files(
            s_overlay_data.overlay_files.data(), s_overlay_data.overlay_files.size(), nullptr);
    } else {
        aurora_dvd_overlay_files(nullptr, 0, nullptr);
    }
}

}  // namespace pc::vfs

extern "C" {

int pc_vfs_resolve(const char* virtual_path, char* out_buf, size_t out_len) {
    if (virtual_path == nullptr || out_buf == nullptr || out_len == 0) {
        return 0;
    }
    std::filesystem::path p = pc::vfs::resolve_path(virtual_path);
    if (p.empty()) {
        return 0;
    }
    std::string s = p.string();
    if (s.size() >= out_len) {
        return 0;
    }
    std::memcpy(out_buf, s.c_str(), s.size() + 1);
    return 1;
}

int pc_vfs_has_file(const char* virtual_path) {
    if (virtual_path == nullptr) {
        return 0;
    }
    return pc::vfs::has_file(virtual_path) ? 1 : 0;
}

void pc_vfs_sync_dvd_overlays(void) {
    pc::vfs::sync_dvd_overlays();
}
}
