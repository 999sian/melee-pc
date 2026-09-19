/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_VFS_HPP
#define PC_VFS_HPP

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include <optional>

namespace pc::vfs {

struct VfsMount {
    std::string name;
    std::filesystem::path root_path;
    int priority = 50;  // Higher priority = searched earlier
    bool enabled = true;
};

// Initializes the VFS. If base_dir is specified, mounts it as the base layer.
void init(const std::filesystem::path& base_dir = {});

// Resets all mounts and clears internal caches.
void reset();

// Mounts a host directory layer with a given priority.
// If a mount with the same name exists, it is updated.
bool mount(const std::string& name, const std::filesystem::path& path, int priority = 50);

// Unmounts a layer by name.
bool unmount(const std::string& name);

// Enables or disables an existing mount by name.
bool set_mount_enabled(const std::string& name, bool enabled);

// Returns true if any active mount contains the given virtual path.
bool has_file(const std::string& virtual_path);

// Resolves a virtual path (e.g. "PlFxNr.dat", "/files/PlFxNr.dat") to an absolute host path.
// Performs case-insensitive matching across active mounts in descending priority order.
std::filesystem::path resolve_path(const std::string& virtual_path);

// Reads an entire file from the VFS into a memory buffer.
// Returns an empty vector if the file cannot be found or read.
std::vector<uint8_t> read_file(const std::string& virtual_path);

// Lists files matching a virtual directory prefix across all active mounts.
std::vector<std::string> list_files(const std::string& virtual_dir = "", bool recursive = false);

// Returns a copy of the current mount list, sorted by descending priority.
std::vector<VfsMount> get_mounts();

// Normalizes a path: strips leading slashes, converts backslashes to forward slashes.
std::string normalize_virtual_path(const std::string& path);

// Registers all active VFS files as Aurora DVD overlays so that GameCube DVD
// routines (DVDConvertPathToEntrynum, DVDFastOpen, etc.) resolve them directly.
void sync_dvd_overlays();

}  // namespace pc::vfs

// C-linkage helpers for interoperability with C modules (file_cache, disc_open, etc.)
#ifdef __cplusplus
extern "C" {
#endif

// Resolves a virtual path into out_buf (up to out_len chars).
// Returns 1 on success, 0 if not found.
int pc_vfs_resolve(const char* virtual_path, char* out_buf, size_t out_len);

// Checks if a virtual path exists in the VFS.
int pc_vfs_has_file(const char* virtual_path);

// C-linkage helper to synchronize DVD overlays with active VFS mounts.
void pc_vfs_sync_dvd_overlays(void);

#ifdef __cplusplus
}
#endif

#endif  // PC_VFS_HPP
