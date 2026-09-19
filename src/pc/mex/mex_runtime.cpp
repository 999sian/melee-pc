/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mex_runtime.hpp"
#include "../json.hpp"
#include "pc/disc.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

namespace pc::mex {

namespace {

std::string read_file_as_string(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
        return "";
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

int parse_int_from_filename(const std::string& stem) {
    try {
        return std::stoi(stem);
    } catch (...) {
        return -1;
    }
}

}  // namespace

MexRuntime& MexRuntime::instance() {
    static MexRuntime s_instance;
    return s_instance;
}

void MexRuntime::reset() {
    m_active = false;
    m_project_name.clear();
    m_project_version.clear();
    m_fighters.clear();
    m_fighter_by_kind.clear();
    m_stages.clear();
    m_stage_by_id.clear();
    m_css_slots.clear();
}

bool MexRuntime::load_project(const std::filesystem::path& project_dir) {
    reset();

    std::error_code ec;
    if (!std::filesystem::exists(project_dir, ec) ||
        !std::filesystem::is_directory(project_dir, ec))
    {
        return false;
    }

    // 1. Check for .mexproj
    for (const auto& entry : std::filesystem::directory_iterator(project_dir, ec)) {
        if (ec)
            break;
        if (entry.path().extension() == ".mexproj") {
            std::string proj_str = read_file_as_string(entry.path());
            auto proj = pc::json::parse(proj_str);
            if (proj.is_object() && proj.has("build")) {
                const auto& b = proj["build"];
                m_project_name = b["longName"].as_string(b["name"].as_string("m-ex Build"));
                int major = b["majorVersion"].as_int(1);
                int minor = b["minorVersion"].as_int(0);
                int patch = b["patchVersion"].as_int(0);
                m_project_version = std::to_string(major) + "." + std::to_string(minor) + "." +
                                    std::to_string(patch);
            }
            break;
        }
    }

    // 2. Parse data/fighters/*.json
    std::filesystem::path fighters_dir = project_dir / "data" / "fighters";
    if (std::filesystem::exists(fighters_dir, ec) &&
        std::filesystem::is_directory(fighters_dir, ec))
    {
        for (const auto& entry : std::filesystem::directory_iterator(fighters_dir, ec)) {
            if (ec)
                break;
            if (entry.path().extension() == ".json") {
                int kind = parse_int_from_filename(entry.path().stem().string());
                if (kind < 0)
                    continue;

                std::string f_str = read_file_as_string(entry.path());
                auto f_obj = pc::json::parse(f_str);
                if (!f_obj.is_object())
                    continue;

                MexFighter f;
                f.kind = kind;
                f.name = f_obj["name"].as_string("Fighter " + std::to_string(kind));
                f.series_id = static_cast<int>(f_obj["seriesID"].as_int(0));
                f.sound_bank = static_cast<int>(f_obj["soundBank"].as_int(-1));
                f.ssm_bitfield1 = static_cast<uint32_t>(f_obj["ssmBitfield1"].as_int(0));
                f.ssm_bitfield2 = static_cast<uint32_t>(f_obj["ssmBitfield2"].as_int(0));
                f.announcer_call = static_cast<int>(f_obj["announcerCall"].as_int(0));
                f.victory_theme = static_cast<int>(f_obj["victoryTheme"].as_int(0));
                f.music1 = static_cast<int>(f_obj["fighterMusic1"].as_int(0));
                f.music2 = static_cast<int>(f_obj["fighterMusic2"].as_int(0));

                if (f_obj.has("files") && f_obj["files"].is_object()) {
                    const auto& files = f_obj["files"];
                    f.filename = files["fighterDataPath"].as_string("");
                    f.data_symbol = files["fighterDataSymbol"].as_string("");
                    f.rst_anim_file = files["rstAnimFile"].as_string("");
                    f.anim_file = files["animFile"].as_string("");
                    f.anim_count = static_cast<int>(files["animCount"].as_int(0));
                }

                if (f_obj.has("boneDefinitions") && f_obj["boneDefinitions"].is_object()) {
                    const auto& bone_defs = f_obj["boneDefinitions"];
                    if (bone_defs.has("lookup") && bone_defs["lookup"].is_object()) {
                        const auto& lookup = bone_defs["lookup"];
                        f.bone_count = static_cast<uint32_t>(lookup["boneCount"].as_int(0));
                        if (f.bone_count > 0) {
                            static const char* const s_lookup_keys[54] = {"top", "trans", "xRot",
                                "yRot", "hip", "waist", "lLegA", "lLeg", "lKnee", "lFootA", "lFoot",
                                "rLegA", "rLeg", "rKnee", "rFootA", "rFoot", "waistB", "bust",
                                "lClavicle", "lShoulderA", "lShoulder", "lArm", "lHand", "lIndex1",
                                "lIndex2", "lMiddle1", "lMiddle2", "lRing1", "lRing2", "lPinky1",
                                "lPinky2", "lHave", "lThumb1", "lThumb2", "neckN", "headN",
                                "rClavicle", "rShoulderA", "rShoulder", "rArm", "rHand", "rIndex1",
                                "rIndex2", "rMiddle1", "rMiddle2", "rRing1", "rRing2", "rPinky1",
                                "rPinky2", "rHave", "rThumb1", "rThumb2", "throw", "extra"};
                            f.part_to_joint.assign(56, 255);
                            for (size_t p = 0; p < 54; ++p) {
                                f.part_to_joint[p] =
                                    static_cast<uint8_t>(lookup[s_lookup_keys[p]].as_int(255));
                            }
                            f.part_to_joint[54] = 0;
                            f.part_to_joint[55] = 0;

                            f.joint_to_part.assign(f.bone_count, 255);
                            for (size_t p = 0; p < 54; ++p) {
                                uint8_t j = f.part_to_joint[p];
                                if (j != 255 && j < f.bone_count) {
                                    f.joint_to_part[j] = static_cast<uint8_t>(p);
                                }
                            }

                            f.parts_table = std::make_shared<FighterPartsTable>();
                            f.parts_table->parts_num = f.bone_count;
                            DP_SET(f.parts_table->part_to_joint, f.part_to_joint.data());
                            DP_SET(f.parts_table->joint_to_part, f.joint_to_part.data());
                        }
                    }
                }

                if (f_obj.has("costumes") && f_obj["costumes"].is_array()) {
                    for (const auto& c : f_obj["costumes"].as_array()) {
                        if (c.has("file") && c["file"].is_object()) {
                            std::string c_file = c["file"]["fileName"].as_string("");
                            if (!c_file.empty()) {
                                f.costumes.push_back(std::move(c_file));
                            }
                        }
                    }
                }

                m_fighter_by_kind[f.kind] = m_fighters.size();
                m_fighters.push_back(std::move(f));
            }
        }
    }

    // 3. Parse data/stages/*.json
    std::filesystem::path stages_dir = project_dir / "data" / "stages";
    if (std::filesystem::exists(stages_dir, ec) && std::filesystem::is_directory(stages_dir, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(stages_dir, ec)) {
            if (ec)
                break;
            if (entry.path().extension() == ".json") {
                int stage_id = parse_int_from_filename(entry.path().stem().string());
                if (stage_id < 0)
                    continue;

                std::string s_str = read_file_as_string(entry.path());
                auto s_obj = pc::json::parse(s_str);
                if (!s_obj.is_object())
                    continue;

                MexStage s;
                s.stage_id = stage_id;
                s.name = s_obj["name"].as_string("Stage " + std::to_string(stage_id));
                s.filename = s_obj["fileName"].as_string();
                s.music_id = static_cast<int>(s_obj["musicID"].as_int(0));

                m_stage_by_id[s.stage_id] = m_stages.size();
                m_stages.push_back(std::move(s));
            }
        }
    }

    // 4. Parse data/css.json
    std::filesystem::path css_path = project_dir / "data" / "css.json";
    if (std::filesystem::exists(css_path, ec)) {
        std::string css_str = read_file_as_string(css_path);
        auto css_obj = pc::json::parse(css_str);
        if (css_obj.is_object()) {
            float templ_scale_x = 0.77f;
            float templ_scale_y = 0.72f;
            float templ_width = 7.05f;
            float templ_height = 7.2f;
            float templ_drop_x = 0.0f;
            float templ_drop_y = -0.3f;
            if (css_obj.has("template") && css_obj["template"].is_object()) {
                const auto& t = css_obj["template"];
                templ_scale_x = static_cast<float>(t["scaleX"].as_double(templ_scale_x));
                templ_scale_y = static_cast<float>(t["scaleY"].as_double(templ_scale_y));
                templ_width = static_cast<float>(t["iconWidth"].as_double(templ_width));
                templ_height = static_cast<float>(t["iconHeight"].as_double(templ_height));
                templ_drop_x = static_cast<float>(t["iconSideDropX"].as_double(templ_drop_x));
                templ_drop_y = static_cast<float>(t["iconSideDropY"].as_double(templ_drop_y));
            }

            const pc::json::Value* list = nullptr;
            if (css_obj.has("fighterIcons") && css_obj["fighterIcons"].is_array()) {
                list = &css_obj["fighterIcons"];
            } else if (css_obj.has("entries") && css_obj["entries"].is_array()) {
                list = &css_obj["entries"];
            }
            if (list) {
                int slot_id = 0;
                for (const auto& item : list->as_array()) {
                    MexCssSlot slot;
                    slot.slot_id = slot_id++;
                    if (item.has("fighter")) {
                        slot.fighter_kind = static_cast<int>(item["fighter"].as_int(-1));
                    } else {
                        slot.fighter_kind = static_cast<int>(item["characterID"].as_int(-1));
                    }
                    slot.sfx_id = static_cast<int>(item["sfxid"].as_int(0));
                    slot.collision_offset_x =
                        static_cast<float>(item["collisionOffsetX"].as_double(templ_drop_x));
                    slot.collision_offset_y =
                        static_cast<float>(item["collisionOffsetY"].as_double(templ_drop_y));
                    slot.collision_size_x =
                        static_cast<float>(item["collisionSizeX"].as_double(templ_width));
                    slot.collision_size_y =
                        static_cast<float>(item["collisionSizeY"].as_double(templ_height));
                    slot.x = static_cast<float>(item["x"].as_double(0.0));
                    slot.y = static_cast<float>(item["y"].as_double(0.0));
                    slot.scale_x = static_cast<float>(item["scaleX"].as_double(templ_scale_x));
                    slot.scale_y = static_cast<float>(item["scaleY"].as_double(templ_scale_y));
                    m_css_slots.push_back(slot);
                }
            }
        }
    }

    m_active = (!m_fighters.empty() || !m_stages.empty());
    return m_active;
}

const MexFighter* MexRuntime::get_fighter_by_kind(int kind) const {
    auto it = m_fighter_by_kind.find(kind);
    if (it != m_fighter_by_kind.end() && it->second < m_fighters.size()) {
        return &m_fighters[it->second];
    }
    return nullptr;
}

const MexFighter* MexRuntime::get_fighter_by_index(size_t index) const {
    if (index < m_fighters.size()) {
        return &m_fighters[index];
    }
    return nullptr;
}

FighterPartsTable* MexRuntime::get_fighter_parts_table(int kind) {
    auto it = m_fighter_by_kind.find(kind);
    if (it != m_fighter_by_kind.end() && it->second < m_fighters.size()) {
        return m_fighters[it->second].parts_table.get();
    }
    return nullptr;
}

const MexStage* MexRuntime::get_stage_by_id(int stage_id) const {
    auto it = m_stage_by_id.find(stage_id);
    if (it != m_stage_by_id.end() && it->second < m_stages.size()) {
        return &m_stages[it->second];
    }
    return nullptr;
}

}  // namespace pc::mex

namespace {

struct MexCSSIcon {
    uint8_t ft_hudindex;  // 0x00
    uint8_t char_kind;    // 0x01
    uint8_t state;        // 0x02
    uint8_t anim_timer;   // 0x03
    uint8_t joint_id_vs;  // 0x04
    uint8_t joint_id_1p;  // 0x05
    uint8_t pad[2];       // 0x06..0x07
    int32_t sfx;          // 0x08
    float bound_l;        // 0x0C
    float bound_r;        // 0x10
    float bound_u;        // 0x14
    float bound_d;        // 0x18
};

static_assert(sizeof(MexCSSIcon) == 28, "MexCSSIcon size must be 28 bytes");
static_assert(offsetof(MexCSSIcon, ft_hudindex) == 0x00, "offset error");
static_assert(offsetof(MexCSSIcon, char_kind) == 0x01, "offset error");
static_assert(offsetof(MexCSSIcon, state) == 0x02, "offset error");
static_assert(offsetof(MexCSSIcon, anim_timer) == 0x03, "offset error");
static_assert(offsetof(MexCSSIcon, joint_id_vs) == 0x04, "offset error");
static_assert(offsetof(MexCSSIcon, joint_id_1p) == 0x05, "offset error");
static_assert(offsetof(MexCSSIcon, sfx) == 0x08, "offset error");
static_assert(offsetof(MexCSSIcon, bound_l) == 0x0C, "offset error");
static_assert(offsetof(MexCSSIcon, bound_r) == 0x10, "offset error");
static_assert(offsetof(MexCSSIcon, bound_u) == 0x14, "offset error");
static_assert(offsetof(MexCSSIcon, bound_d) == 0x18, "offset error");

struct VanillaIconInfo {
    uint8_t ft_hudindex;
    uint8_t joint_id;
};

static const std::unordered_map<int, VanillaIconInfo> s_vanilla_icons = {
    {22, {0, 0x04}},   // Dr. Mario (ICONHUD_DRMARIO, ICONJOINT_DRMARIO)
    {8, {1, 0x10}},    // Mario (ICONHUD_MARIO, ICONJOINT_MARIO)
    {7, {2, 0x11}},    // Luigi (ICONHUD_LUIGI, ICONJOINT_LUIGI)
    {5, {3, 0x12}},    // Bowser (ICONHUD_KOOPA, ICONJOINT_KOOPA)
    {12, {4, 0x13}},   // Peach (ICONHUD_PEACH, ICONJOINT_PEACH)
    {17, {5, 0x14}},   // Yoshi (ICONHUD_YOSHI, ICONJOINT_YOSHI)
    {1, {6, 0x15}},    // DK (ICONHUD_DONKEY, ICONJOINT_DONKEY)
    {0, {7, 0x16}},    // Captain Falcon (ICONHUD_CAPTAIN, ICONJOINT_CAPTAIN)
    {25, {8, 0x06}},   // Ganondorf (ICONHUD_GANON, ICONJOINT_GANON)
    {20, {9, 0x08}},   // Falco (ICONHUD_FALCO, ICONJOINT_FALCO)
    {2, {10, 0x17}},   // Fox (ICONHUD_FOX, ICONJOINT_FOX)
    {11, {11, 0x18}},  // Ness (ICONHUD_NESS, ICONJOINT_NESS)
    {14, {12, 0x19}},  // Ice Climbers (ICONHUD_POPONANA, ICONJOINT_POPONANA)
    {4, {13, 0x1A}},   // Kirby (ICONHUD_KIRBY, ICONJOINT_KIRBY)
    {16, {14, 0x1B}},  // Samus (ICONHUD_SAMUS, ICONJOINT_SAMUS)
    {18, {15, 0x1C}},  // Zelda (ICONHUD_ZELDA, ICONJOINT_ZELDA)
    {6, {16, 0x1D}},   // Link (ICONHUD_LINK, ICONJOINT_LINK)
    {21, {17, 0x0A}},  // Young Link (ICONHUD_CLINK, ICONJOINT_CLINK)
    {24, {18, 0x0C}},  // Pichu (ICONHUD_PICHU, ICONJOINT_PICHU)
    {13, {19, 0x1E}},  // Pikachu (ICONHUD_PIKACHU, ICONJOINT_PIKACHU)
    {15, {20, 0x1F}},  // Jigglypuff (ICONHUD_PURIN, ICONJOINT_PURIN)
    {10, {21, 0x20}},  // Mewtwo (ICONHUD_MEWTWO, ICONJOINT_MEWTWO)
    {3, {22, 0x21}},   // Mr. Game & Watch (ICONHUD_GAMEWATCH, ICONJOINT_GAMEWATCH)
    {9, {23, 0x22}},   // Marth (ICONHUD_MARS, ICONJOINT_MARS)
    {23, {24, 0x0E}},  // Roy (ICONHUD_EMBLEM, ICONJOINT_EMBLEM)
};

}  // namespace

extern "C" {

bool pc_mex_is_active(void) {
    return pc::mex::MexRuntime::instance().is_active();
}

int pc_mex_get_css_icon_count(void) {
    auto& rt = pc::mex::MexRuntime::instance();
    if (!rt.is_active())
        return 25;
    int count = static_cast<int>(rt.get_css_slots().size());
    return count > 0 ? count : 25;
}

bool pc_mex_populate_css_icons(void* out_icons, int max_icons) {
    if (!out_icons || max_icons <= 0)
        return false;
    auto& rt = pc::mex::MexRuntime::instance();
    if (!rt.is_active())
        return false;
    const auto& slots = rt.get_css_slots();
    if (slots.empty())
        return false;

    auto* icons = static_cast<MexCSSIcon*>(out_icons);
    int count = std::min(max_icons, static_cast<int>(slots.size()));
    std::memset(out_icons, 0, max_icons * sizeof(MexCSSIcon));

    for (int i = 0; i < count; ++i) {
        const auto& s = slots[i];
        icons[i].char_kind = static_cast<uint8_t>(s.fighter_kind);
        icons[i].state = 2;  // ICONSTATE_UNLOCKED
        icons[i].anim_timer = 0;
        icons[i].sfx = s.sfx_id;

        auto v_it = s_vanilla_icons.find(s.fighter_kind);
        if (v_it != s_vanilla_icons.end()) {
            icons[i].ft_hudindex = v_it->second.ft_hudindex;
            icons[i].joint_id_vs = v_it->second.joint_id;
            icons[i].joint_id_1p = v_it->second.joint_id;
        } else {
            icons[i].ft_hudindex = 0;
            icons[i].joint_id_vs = 0;
            icons[i].joint_id_1p = 0;
        }

        float half_w = (s.collision_size_x * s.scale_x) * 0.5f;
        float half_h = (s.collision_size_y * s.scale_y) * 0.5f;
        float cx = s.x + s.collision_offset_x;
        float cy = s.y + s.collision_offset_y;

        icons[i].bound_l = cx - half_w;
        icons[i].bound_r = cx + half_w;
        icons[i].bound_u = cy + half_h;
        icons[i].bound_d = cy - half_h;
    }
    return true;
}

int pc_mex_get_fighter_count(void) {
    return static_cast<int>(pc::mex::MexRuntime::instance().get_fighter_count());
}

const char* pc_mex_get_fighter_name(int kind) {
    const auto* f = pc::mex::MexRuntime::instance().get_fighter_by_kind(kind);
    return f ? f->name.c_str() : "";
}

const char* pc_mex_get_fighter_filename(int kind) {
    const auto* f = pc::mex::MexRuntime::instance().get_fighter_by_kind(kind);
    return f ? f->filename.c_str() : "";
}

const char* pc_mex_get_fighter_data_symbol(int kind) {
    const auto* f = pc::mex::MexRuntime::instance().get_fighter_by_kind(kind);
    return f ? f->data_symbol.c_str() : "";
}

int pc_mex_get_fighter_costume_count(int kind) {
    const auto* f = pc::mex::MexRuntime::instance().get_fighter_by_kind(kind);
    return f ? static_cast<int>(f->costumes.size()) : 0;
}

const char* pc_mex_get_fighter_costume_file(int kind, int costume_idx) {
    const auto* f = pc::mex::MexRuntime::instance().get_fighter_by_kind(kind);
    if (f && !f->costumes.empty()) {
        if (costume_idx >= 0 && costume_idx < static_cast<int>(f->costumes.size())) {
            return f->costumes[costume_idx].c_str();
        }
        return f->costumes[0].c_str();
    }
    return "";
}

const char* pc_mex_get_fighter_rst_anim_file(int kind) {
    const auto* f = pc::mex::MexRuntime::instance().get_fighter_by_kind(kind);
    return f ? f->rst_anim_file.c_str() : "";
}

int pc_mex_get_fighter_announcer_call(int kind) {
    const auto* f = pc::mex::MexRuntime::instance().get_fighter_by_kind(kind);
    return f ? f->announcer_call : 0;
}

const char* pc_mex_get_fighter_anim_file(int kind) {
    const auto* f = pc::mex::MexRuntime::instance().get_fighter_by_kind(kind);
    return f ? f->anim_file.c_str() : "";
}

int pc_mex_get_fighter_anim_count(int kind) {
    const auto* f = pc::mex::MexRuntime::instance().get_fighter_by_kind(kind);
    return f ? f->anim_count : 0;
}

struct FighterPartsTable* pc_mex_get_fighter_parts_table(int kind) {
    return pc::mex::MexRuntime::instance().get_fighter_parts_table(kind);
}
}
