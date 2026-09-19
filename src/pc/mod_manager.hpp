/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_MOD_MANAGER_HPP
#define PC_MOD_MANAGER_HPP

#include <filesystem>
#include <string>
#include <vector>

namespace pc::mod {

enum class ModType {
    Standard,  // Filesystem / DAT asset replacements
    Mex,       // Full m-ex / ACE expansion (contains mexproj or data/mex.json)
    Texture,   // Dolphin HD texture pack (contains textures/)
    Music,     // Custom soundtrack (contains music/)
    GeckoCode  // Pure Gecko code patch
};

struct ModInfo {
    std::string id;
    std::string name;
    std::string version;
    std::string author;
    std::string description;
    std::filesystem::path root_path;
    std::filesystem::path banner_path;
    ModType type = ModType::Standard;
    bool enabled = true;
    int priority = 50;
    bool has_gecko = false;
    std::string gecko_source;  // Raw Gecko hex string if present
    std::vector<std::string> conflict_warnings;
};

class ModManager {
public:
    static ModManager& instance();

    // Initializes mod directories and loads persisted configuration.
    void init(const std::filesystem::path& pref_dir = {});

    // Scans all mod directories for installed mods.
    void scan_mods();

    // Returns all discovered mods, sorted by descending priority.
    const std::vector<ModInfo>& get_mods() const { return m_mods; }

    // Toggle a mod on or off.
    bool set_mod_enabled(const std::string& id, bool enabled);

    // Set priority manually.
    bool set_mod_priority(const std::string& id, int priority);

    // Reordering helpers for UI (moves up/down in priority list).
    void move_mod_up(const std::string& id);
    void move_mod_down(const std::string& id);

    // Mounts all active mods into VFS and configures texture/audio directories.
    void apply_active_mods();

    // Detects file conflicts between enabled mods.
    void check_conflicts();

    // Saves enabled states and priorities to mods.cfg.
    void save_config();

    // Loads configuration from mods.cfg.
    void load_config();

    // Primary mods folder where users can install mods (<prefPath>/mods/).
    const std::filesystem::path& get_primary_mods_dir() const { return m_primary_mods_dir; }

    // Number of active/enabled mods.
    size_t get_active_count() const;

private:
    ModManager() = default;
    std::filesystem::path m_primary_mods_dir;
    std::vector<std::filesystem::path> m_search_dirs;
    std::vector<ModInfo> m_mods;
    std::string m_config_path;

    void inspect_directory(const std::filesystem::path& dir);
};

}  // namespace pc::mod

// C ABI helpers
#ifdef __cplusplus
extern "C" {
#endif

void pc_mod_manager_init(const char* pref_path);
void pc_mod_manager_apply(void);
int pc_mod_manager_get_count(void);
const char* pc_mod_manager_get_name(int index);
int pc_mod_manager_is_enabled(int index);
void pc_mod_manager_set_enabled(int index, int enabled);

#ifdef __cplusplus
}
#endif

#endif  // PC_MOD_MANAGER_HPP
