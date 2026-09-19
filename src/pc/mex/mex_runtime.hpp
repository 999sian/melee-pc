/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_MEX_RUNTIME_HPP
#define PC_MEX_RUNTIME_HPP

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "pc/endian.hpp"

struct FighterPartsTable {
    BE<uint32_t> joint_to_part = 0;
    BE<uint32_t> part_to_joint = 0;
    BE<uint32_t> parts_num = 0;
};
static_assert(sizeof(FighterPartsTable) == 12, "FighterPartsTable size error");

namespace pc::mex {

struct MexFighter {
    int kind = -1;
    std::string name;
    int series_id = 0;
    int sound_bank = -1;
    uint32_t ssm_bitfield1 = 0;
    uint32_t ssm_bitfield2 = 0;
    int announcer_call = 0;
    int victory_theme = 0;
    int music1 = 0;
    int music2 = 0;
    std::string filename;
    std::string data_symbol;
    std::string rst_anim_file;
    std::string anim_file;
    int anim_count = 0;
    std::vector<std::string> costumes;

    uint32_t bone_count = 0;
    std::vector<uint8_t> part_to_joint;
    std::vector<uint8_t> joint_to_part;
    std::shared_ptr<struct FighterPartsTable> parts_table;
};

struct MexStage {
    int stage_id = -1;
    std::string name;
    std::string filename;
    int music_id = 0;
};

struct MexCssSlot {
    int slot_id = -1;
    int fighter_kind = -1;
    int sfx_id = 0;
    float collision_offset_x = 0.0f;
    float collision_offset_y = 0.0f;
    float collision_size_x = 7.05f;
    float collision_size_y = 7.2f;
    float x = 0.0f;
    float y = 0.0f;
    float scale_x = 0.77f;
    float scale_y = 0.72f;
};

class MexRuntime {
public:
    static MexRuntime& instance();

    // Reset all loaded expansion data.
    void reset();

    // Ingests an m-ex project directly from a directory containing .mexproj or data/
    bool load_project(const std::filesystem::path& project_dir);

    // Returns true if an m-ex expansion is currently active.
    bool is_active() const { return m_active; }

    // Returns project name and version.
    const std::string& get_project_name() const { return m_project_name; }
    const std::string& get_project_version() const { return m_project_version; }

    // Fighter queries.
    size_t get_fighter_count() const { return m_fighters.size(); }
    const MexFighter* get_fighter_by_kind(int kind) const;
    const MexFighter* get_fighter_by_index(size_t index) const;
    struct FighterPartsTable* get_fighter_parts_table(int kind);

    // Stage queries.
    size_t get_stage_count() const { return m_stages.size(); }
    const MexStage* get_stage_by_id(int stage_id) const;

    // CSS queries.
    const std::vector<MexCssSlot>& get_css_slots() const { return m_css_slots; }

private:
    MexRuntime() = default;
    bool m_active = false;
    std::string m_project_name;
    std::string m_project_version;

    std::vector<MexFighter> m_fighters;
    std::unordered_map<int, size_t> m_fighter_by_kind;

    std::vector<MexStage> m_stages;
    std::unordered_map<int, size_t> m_stage_by_id;

    std::vector<MexCssSlot> m_css_slots;
};

}  // namespace pc::mex

// C ABI helpers
#ifdef __cplusplus
extern "C" {
#endif

bool pc_mex_is_active(void);
int pc_mex_get_css_icon_count(void);
bool pc_mex_populate_css_icons(void* out_icons, int max_icons);
const char* pc_mex_get_fighter_name(int kind);
const char* pc_mex_get_fighter_filename(int kind);
int pc_mex_get_fighter_costume_count(int kind);
const char* pc_mex_get_fighter_costume_file(int kind, int costume_idx);
int pc_mex_get_fighter_count(void);
const char* pc_mex_get_fighter_rst_anim_file(int kind);
int pc_mex_get_fighter_announcer_call(int kind);

#ifdef __cplusplus
}
#endif

#endif  // PC_MEX_RUNTIME_HPP
