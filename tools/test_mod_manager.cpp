/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pc/mod_manager.hpp"
#include "pc/vfs/vfs.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

static void write_file(const std::filesystem::path& p, const std::string& content) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out << content;
}

int main() {
    std::filesystem::path tmp_root =
        std::filesystem::temp_directory_path() / "melee_pc_mod_mgr_test";
    std::error_code ec;
    std::filesystem::remove_all(tmp_root, ec);
    std::filesystem::create_directories(tmp_root);

    std::filesystem::path pref_dir = tmp_root / "pref";
    std::filesystem::path mods_dir = pref_dir / "mods";
    std::filesystem::create_directories(mods_dir);

    // 1. Create a Standard Mod (Neon Fox Skin) with mod.json
    std::filesystem::path fox_mod = mods_dir / "neon_fox";
    std::string mod_json_content = R"({
        "id": "neon_fox",
        "name": "Neon Fox Skin",
        "version": "1.2.0",
        "author": "Sian",
        "description": "High-visibility neon textures.",
        "geckoCodes": [
            { "name": "Custom Laser", "code": "040E0A20 3F800000" }
        ]
    })";
    write_file(fox_mod / "mod.json", mod_json_content);
    write_file(fox_mod / "files" / "PlFxNr.dat", "neon_fox_dat");

    // 2. Create an m-ex Mod (ACE-BUILD) with ace.mexproj & data/mex.json
    std::filesystem::path ace_mod = mods_dir / "ace_build";
    std::string mexproj_content = R"({
        "startingScene": 2,
        "build": {
            "name": "Akaneia Community Edition",
            "longName": "The Ace Build v1.0.1",
            "shortMaker": "Team Akaneia, TurboDX, Team ACE",
            "majorVersion": 1,
            "minorVersion": 0,
            "patchVersion": 1,
            "description": "Nintendo's all-stars ready for battle!"
        }
    })";
    write_file(ace_mod / "ace.mexproj", mexproj_content);
    write_file(
        ace_mod / "data" / "mex.json", R"({ "name": "gct", "source": "C2068B40 00000030" })");
    write_file(ace_mod / "files" / "PlWf.dat", "wolf_dat");
    write_file(ace_mod / "files" / "PlFxNr.dat", "ace_fox_dat");  // Conflict with neon_fox!

    // Initialize VFS and ModManager
    pc::vfs::init();
    auto& mgr = pc::mod::ModManager::instance();
    mgr.init(pref_dir);

    const auto& mods = mgr.get_mods();
    assert(mods.size() >= 2);

    // Check parsed data for neon_fox
    bool found_fox = false;
    bool found_ace = false;
    for (const auto& m : mods) {
        if (m.id == "neon_fox") {
            found_fox = true;
            assert(m.name == "Neon Fox Skin");
            assert(m.author == "Sian");
            assert(m.version == "1.2.0");
            assert(m.has_gecko);
        } else if (m.id == "ace_build") {
            found_ace = true;
            assert(m.name == "The Ace Build v1.0.1");
            assert(m.author == "Team Akaneia, TurboDX, Team ACE");
            assert(m.version == "1.0.1");
            assert(m.type == pc::mod::ModType::Mex);
            assert(m.has_gecko);
        }
    }
    assert(found_fox);
    assert(found_ace);

    // Conflict detection test: both define PlFxNr.dat!
    mgr.check_conflicts();
    bool has_conflict_reported = false;
    for (const auto& m : mgr.get_mods()) {
        if (!m.conflict_warnings.empty()) {
            has_conflict_reported = true;
            break;
        }
    }
    assert(has_conflict_reported);

    // VFS verification: PlWf.dat from ACE is resolvable!
    assert(pc::vfs::has_file("PlWf.dat"));
    auto wolf_bytes = pc::vfs::read_file("PlWf.dat");
    assert(std::string(wolf_bytes.begin(), wolf_bytes.end()) == "wolf_dat");

    // Priority test: elevate neon_fox higher than ace_build
    mgr.set_mod_priority("neon_fox", 100);
    mgr.set_mod_priority("ace_build", 50);
    auto fox_bytes = pc::vfs::read_file("PlFxNr.dat");
    assert(std::string(fox_bytes.begin(), fox_bytes.end()) == "neon_fox_dat");

    // Reordering test
    mgr.set_mod_priority("ace_build", 120);
    auto ace_fox_bytes = pc::vfs::read_file("PlFxNr.dat");
    assert(std::string(ace_fox_bytes.begin(), ace_fox_bytes.end()) == "ace_fox_dat");

    // Disable ace_build -> neon_fox should now resolve
    mgr.set_mod_enabled("ace_build", false);
    assert(!pc::vfs::has_file("PlWf.dat"));
    auto fallback_fox = pc::vfs::read_file("PlFxNr.dat");
    assert(std::string(fallback_fox.begin(), fallback_fox.end()) == "neon_fox_dat");

    // Config persistence test
    mgr.save_config();
    assert(std::filesystem::exists(pref_dir / "mods.cfg"));

    // Cleanup
    std::filesystem::remove_all(tmp_root, ec);
    std::printf("PASS: ModManager scanning, mexproj parsing, VFS priority routing, conflicts, and "
                "config\n");
    return 0;
}
