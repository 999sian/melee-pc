/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mod_manager.hpp"
#include "vfs/vfs.hpp"
#include "mex/mex_runtime.hpp"
#include "json.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace pc::mod {

namespace {

std::string read_file_as_string(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
        return "";
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

ModManager& ModManager::instance() {
    static ModManager s_instance;
    return s_instance;
}

void ModManager::init(const std::filesystem::path& pref_dir) {
    m_search_dirs.clear();
    m_mods.clear();

    std::filesystem::path user_pref = pref_dir;
    if (user_pref.empty()) {
        char* p = SDL_GetPrefPath(nullptr, "melee-pc");
        if (p != nullptr) {
            user_pref = p;
            SDL_free(p);
        }
    }

    if (!user_pref.empty()) {
        m_primary_mods_dir = user_pref / "mods";
        std::error_code ec;
        if (!std::filesystem::exists(m_primary_mods_dir, ec)) {
            std::filesystem::create_directories(m_primary_mods_dir, ec);
        }
        m_search_dirs.push_back(m_primary_mods_dir);
        m_config_path = (user_pref / "mods.cfg").string();
    }

    const char* base = SDL_GetBasePath();
    if (base != nullptr) {
        std::filesystem::path base_mods = std::filesystem::path(base) / "mods";
        if (base_mods != m_primary_mods_dir) {
            m_search_dirs.push_back(base_mods);
        }
    }

    std::filesystem::path cwd_mods = "./mods";
    if (cwd_mods != m_primary_mods_dir) {
        m_search_dirs.push_back(cwd_mods);
    }

    scan_mods();
    load_config();
    apply_active_mods();
}

void ModManager::inspect_directory(const std::filesystem::path& dir) {
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec) || !std::filesystem::is_directory(dir, ec)) {
        return;
    }

    std::string folder_name = dir.filename().string();
    ModInfo info;
    info.id = folder_name;
    info.name = folder_name;
    info.root_path = dir;
    info.version = "1.0.0";
    info.author = "Unknown";
    info.description = "";
    info.type = ModType::Standard;
    info.enabled = true;
    info.priority = 50;

    // Check for banner
    if (std::filesystem::exists(dir / "banner.png", ec)) {
        info.banner_path = dir / "banner.png";
    } else if (std::filesystem::exists(dir / "assets" / "banner.png", ec)) {
        info.banner_path = dir / "assets" / "banner.png";
    }

    // 1. Check for mod.json
    std::filesystem::path mod_json = dir / "mod.json";
    if (std::filesystem::exists(mod_json, ec)) {
        std::string json_str = read_file_as_string(mod_json);
        auto root = pc::json::parse(json_str);
        if (root.is_object()) {
            if (root.has("id"))
                info.id = root["id"].as_string(info.id);
            if (root.has("name"))
                info.name = root["name"].as_string(info.name);
            if (root.has("version"))
                info.version = root["version"].as_string(info.version);
            if (root.has("author"))
                info.author = root["author"].as_string(info.author);
            if (root.has("description"))
                info.description = root["description"].as_string(info.description);

            if (root.has("geckoCodes") && root["geckoCodes"].is_array()) {
                info.has_gecko = true;
                std::string codes;
                for (const auto& item : root["geckoCodes"].as_array()) {
                    if (item.has("code")) {
                        codes += item["code"].as_string() + "\n";
                    }
                }
                info.gecko_source = codes;
            }
        }
    }

    // 2. Check for .mexproj (e.g. ace.mexproj in ACE-BUILD)
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec)
            break;
        if (entry.path().extension() == ".mexproj") {
            info.type = ModType::Mex;
            std::string proj_str = read_file_as_string(entry.path());
            auto proj = pc::json::parse(proj_str);
            if (proj.is_object() && proj.has("build")) {
                const auto& b = proj["build"];
                if (b.has("longName")) {
                    info.name = b["longName"].as_string(info.name);
                } else if (b.has("name")) {
                    info.name = b["name"].as_string(info.name);
                }
                if (b.has("shortMaker")) {
                    info.author = b["shortMaker"].as_string(info.author);
                }
                if (b.has("description")) {
                    info.description = b["description"].as_string(info.description);
                }
                int major = b["majorVersion"].as_int(1);
                int minor = b["minorVersion"].as_int(0);
                int patch = b["patchVersion"].as_int(0);
                info.version = std::to_string(major) + "." + std::to_string(minor) + "." +
                               std::to_string(patch);
            }
            break;
        }
    }

    // 3. Check for data/mex.json (Gecko patches for m-ex)
    std::filesystem::path mex_json = dir / "data" / "mex.json";
    if (std::filesystem::exists(mex_json, ec)) {
        info.type = ModType::Mex;
        std::string json_str = read_file_as_string(mex_json);
        auto mex_obj = pc::json::parse(json_str);
        if (mex_obj.is_object() && mex_obj.has("source")) {
            info.has_gecko = true;
            info.gecko_source = mex_obj["source"].as_string();
        }
    }

    // 4. Check for textures or music only
    if (std::filesystem::exists(dir / "textures", ec) &&
        !std::filesystem::exists(dir / "files", ec) && info.type == ModType::Standard)
    {
        info.type = ModType::Texture;
    }
    if (std::filesystem::exists(dir / "music", ec) && !std::filesystem::exists(dir / "files", ec) &&
        info.type == ModType::Standard)
    {
        info.type = ModType::Music;
    }

    // Prevent duplicate entries by ID
    for (auto& existing : m_mods) {
        if (existing.id == info.id) {
            return;
        }
    }

    m_mods.push_back(std::move(info));
}

void ModManager::scan_mods() {
    m_mods.clear();
    std::error_code ec;

    for (const auto& search_dir : m_search_dirs) {
        if (!std::filesystem::exists(search_dir, ec) ||
            !std::filesystem::is_directory(search_dir, ec))
        {
            continue;
        }

        for (const auto& entry : std::filesystem::directory_iterator(search_dir, ec)) {
            if (ec)
                break;
            if (entry.is_directory(ec)) {
                inspect_directory(entry.path());
            }
        }
    }

    // Sort by priority descending
    std::stable_sort(m_mods.begin(), m_mods.end(),
        [](const ModInfo& a, const ModInfo& b) { return a.priority > b.priority; });

    check_conflicts();
}

bool ModManager::set_mod_enabled(const std::string& id, bool enabled) {
    for (auto& m : m_mods) {
        if (m.id == id) {
            if (m.enabled != enabled) {
                m.enabled = enabled;
                apply_active_mods();
                save_config();
            }
            return true;
        }
    }
    return false;
}

bool ModManager::set_mod_priority(const std::string& id, int priority) {
    for (auto& m : m_mods) {
        if (m.id == id) {
            m.priority = priority;
            std::stable_sort(m_mods.begin(), m_mods.end(),
                [](const ModInfo& a, const ModInfo& b) { return a.priority > b.priority; });
            apply_active_mods();
            save_config();
            return true;
        }
    }
    return false;
}

void ModManager::move_mod_up(const std::string& id) {
    for (size_t i = 0; i < m_mods.size(); ++i) {
        if (m_mods[i].id == id && i > 0) {
            int higher_prio = m_mods[i - 1].priority;
            m_mods[i].priority = higher_prio + 1;
            std::stable_sort(m_mods.begin(), m_mods.end(),
                [](const ModInfo& a, const ModInfo& b) { return a.priority > b.priority; });
            apply_active_mods();
            save_config();
            return;
        }
    }
}

void ModManager::move_mod_down(const std::string& id) {
    for (size_t i = 0; i < m_mods.size(); ++i) {
        if (m_mods[i].id == id && i + 1 < m_mods.size()) {
            int lower_prio = m_mods[i + 1].priority;
            m_mods[i].priority = std::max(0, lower_prio - 1);
            std::stable_sort(m_mods.begin(), m_mods.end(),
                [](const ModInfo& a, const ModInfo& b) { return a.priority > b.priority; });
            apply_active_mods();
            save_config();
            return;
        }
    }
}

void ModManager::apply_active_mods() {
    bool has_mex = false;
    for (const auto& m : m_mods) {
        if (!m.enabled) {
            pc::vfs::unmount(m.id);
            pc::vfs::unmount(m.id + "_root");
            continue;
        }

        std::filesystem::path mount_root = m.root_path;
        std::error_code ec;
        if (std::filesystem::exists(mount_root / "files", ec)) {
            mount_root = mount_root / "files";
            pc::vfs::mount(m.id, mount_root, m.priority);
            pc::vfs::mount(m.id + "_root", m.root_path, std::max(0, m.priority - 1));
        } else {
            pc::vfs::mount(m.id, mount_root, m.priority);
        }

        if (m.type == ModType::Mex && !has_mex) {
            pc::mex::MexRuntime::instance().load_project(m.root_path);
            has_mex = true;
        }
    }
    if (!has_mex) {
        pc::mex::MexRuntime::instance().reset();
    }
    check_conflicts();
    pc::vfs::sync_dvd_overlays();
}

void ModManager::check_conflicts() {
    std::unordered_map<std::string, std::string> seen_files;
    for (auto& m : m_mods) {
        m.conflict_warnings.clear();
    }

    std::error_code ec;
    for (auto& m : m_mods) {
        if (!m.enabled)
            continue;

        std::filesystem::path files_dir = m.root_path;
        if (std::filesystem::exists(m.root_path / "files", ec)) {
            files_dir = m.root_path / "files";
        }

        if (!std::filesystem::exists(files_dir, ec))
            continue;

        for (const auto& entry : std::filesystem::recursive_directory_iterator(files_dir, ec)) {
            if (ec)
                break;
            if (entry.is_regular_file(ec)) {
                std::string rel = std::filesystem::relative(entry.path(), files_dir, ec).string();
                for (char& c : rel)
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

                auto it = seen_files.find(rel);
                if (it != seen_files.end()) {
                    std::string warn = "Overrides " + rel + " from " + it->second;
                    m.conflict_warnings.push_back(warn);
                } else {
                    seen_files[rel] = m.name;
                }
            }
        }
    }
}

size_t ModManager::get_active_count() const {
    size_t count = 0;
    for (const auto& m : m_mods) {
        if (m.enabled)
            count++;
    }
    return count;
}

void ModManager::save_config() {
    if (m_config_path.empty())
        return;
    std::ofstream out(m_config_path);
    if (!out.is_open())
        return;

    for (const auto& m : m_mods) {
        out << m.id << "=" << (m.enabled ? "1" : "0") << "," << m.priority << "\n";
    }
}

void ModManager::load_config() {
    if (m_config_path.empty())
        return;
    std::ifstream in(m_config_path);
    if (!in.is_open())
        return;

    std::string line;
    while (std::getline(in, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        std::string id = line.substr(0, eq);
        std::string rest = line.substr(eq + 1);

        bool enabled = true;
        int priority = 50;

        size_t comma = rest.find(',');
        if (comma != std::string::npos) {
            enabled = (rest.substr(0, comma) == "1");
            try {
                priority = std::stoi(rest.substr(comma + 1));
            } catch (...) {
            }
        } else {
            enabled = (rest == "1");
        }

        for (auto& m : m_mods) {
            if (m.id == id) {
                m.enabled = enabled;
                m.priority = priority;
                break;
            }
        }
    }

    std::stable_sort(m_mods.begin(), m_mods.end(),
        [](const ModInfo& a, const ModInfo& b) { return a.priority > b.priority; });
}

}  // namespace pc::mod

extern "C" {

void pc_mod_manager_init(const char* pref_path) {
    pc::mod::ModManager::instance().init(
        pref_path ? std::filesystem::path(pref_path) : std::filesystem::path());
}

void pc_mod_manager_apply(void) {
    pc::mod::ModManager::instance().apply_active_mods();
}

int pc_mod_manager_get_count(void) {
    return static_cast<int>(pc::mod::ModManager::instance().get_mods().size());
}

const char* pc_mod_manager_get_name(int index) {
    const auto& mods = pc::mod::ModManager::instance().get_mods();
    if (index >= 0 && static_cast<size_t>(index) < mods.size()) {
        return mods[index].name.c_str();
    }
    return "";
}

int pc_mod_manager_is_enabled(int index) {
    const auto& mods = pc::mod::ModManager::instance().get_mods();
    if (index >= 0 && static_cast<size_t>(index) < mods.size()) {
        return mods[index].enabled ? 1 : 0;
    }
    return 0;
}

void pc_mod_manager_set_enabled(int index, int enabled) {
    const auto& mods = pc::mod::ModManager::instance().get_mods();
    if (index >= 0 && static_cast<size_t>(index) < mods.size()) {
        pc::mod::ModManager::instance().set_mod_enabled(mods[index].id, enabled != 0);
    }
}
}
