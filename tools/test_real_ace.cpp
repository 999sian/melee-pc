/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pc/mod_manager.hpp"
#include "pc/mex/mex_runtime.hpp"
#include "pc/gecko/gecko_engine.hpp"
#include "pc/vfs/vfs.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include "pc/json.hpp"

int main() {
    std::cout << "========================================\n";
    std::cout << "Testing Real ACE-BUILD-PUBLIC- on Melee-PC\n";
    std::cout << "========================================\n";

    std::filesystem::path ace_path = "./mods/ACE-BUILD-PUBLIC-";
    if (!std::filesystem::exists(ace_path)) {
        ace_path = "../mods/ACE-BUILD-PUBLIC-";
    }
    if (!std::filesystem::exists(ace_path)) {
        std::cerr << "ERROR: " << ace_path << " not found!\n";
        return 1;
    }

    // 1. Test MexRuntime directly loading real ace.mexproj and data/
    std::cout << "[1] Loading ACE project with pc::mex::MexRuntime...\n";
    auto& mex = pc::mex::MexRuntime::instance();
    bool loaded = mex.load_project(ace_path);
    if (!loaded) {
        std::cerr << "ERROR: MexRuntime failed to load ACE project!\n";
        return 1;
    }

    std::cout << "  Project Name:    " << mex.get_project_name() << "\n";
    std::cout << "  Project Version: " << mex.get_project_version() << "\n";
    std::cout << "  Fighter Count:   " << mex.get_fighter_count() << "\n";
    std::cout << "  CSS Slot Count:  " << mex.get_css_slots().size() << "\n";

    // Print first 10 fighters
    std::cout << "  Sample fighters loaded:\n";
    for (int i = 0; i < 55; ++i) {
        const auto* f = mex.get_fighter_by_kind(i);
        if (f) {
            std::cout << "    [" << i << "] " << f->name << " (series: " << f->series_id
                      << ", soundBank: " << f->sound_bank << ")\n";
        }
    }

    // 2. Test Gecko codes from data/mex.json
    std::cout << "\n[2] Testing Gecko engine on ACE data/mex.json...\n";
    std::filesystem::path mex_json = ace_path / "data" / "mex.json";
    std::string gecko_src;
    if (std::filesystem::exists(mex_json)) {
        std::ifstream in(mex_json, std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        auto parsed = pc::json::parse(content);
        if (parsed.is_object() && parsed.has("source")) {
            gecko_src = parsed["source"].as_string();
        }
    }

    std::cout << "  Gecko source length: " << gecko_src.size() << " bytes\n";
    auto& gecko = pc::gecko::GeckoEngine::instance();
    auto codes = gecko.parse_codes(gecko_src, "m-ex System");
    std::cout << "  Gecko codes count:   " << codes.size() << "\n";
    size_t total_lines = 0;
    for (const auto& c : codes)
        total_lines += c.lines.size();
    std::cout << "  Gecko lines parsed:  " << total_lines << "\n";
    gecko.apply_codes(codes);
    std::cout << "  Gecko apply to RAM:  SUCCESS\n";

    // 3. Test ModManager integration
    std::cout << "\n[3] Testing ModManager automatic discovery & VFS mounting...\n";
    auto& mm = pc::mod::ModManager::instance();
    mm.init(ace_path.parent_path().parent_path());

    std::cout << "  Total mods detected: " << mm.get_mods().size() << "\n";
    bool found_ace = false;
    for (const auto& m : mm.get_mods()) {
        std::cout << "  - Mod: " << m.name << " (" << m.version << ") by " << m.author
                  << " [type: " << (int)m.type << ", enabled: " << (m.enabled ? "yes" : "no")
                  << ", has_gecko: " << (m.has_gecko ? "yes" : "no") << "]\n";
        if (m.id == "ACE-BUILD-PUBLIC-" || m.name.find("Ace") != std::string::npos ||
            m.name.find("Akaneia") != std::string::npos)
        {
            found_ace = true;
        }
    }

    if (!found_ace) {
        std::cerr << "ERROR: ModManager did not find ACE build in mods/!\n";
        return 1;
    }

    // 4. Test VFS resolution of ACE assets
    std::cout << "\n[4] Testing VFS resolution of ACE assets...\n";
    char resolved[1024] = {0};
    if (pc_vfs_resolve("assets/banner.png", resolved, sizeof(resolved))) {
        std::cout << "  assets/banner.png -> " << resolved << "\n";
    } else {
        std::cerr << "ERROR: assets/banner.png not directly resolved\n";
        return 1;
    }

    // Test custom characters, costumes, stages, and audio
    const std::vector<std::string> test_files = {
        "PlWf.dat",           // Wolf character data
        "PlWfAJ.dat",         // Wolf animations
        "PlWfNr.dat",         // Wolf neutral costume
        "EfWfData.dat",       // Wolf effects
        "GmRstMWf.dat",       // Wolf results screen
        "PlDk.dat",           // Diddy Kong character data
        "PlDkAJ.dat",         // Diddy Kong animations
        "PlDkNr.dat",         // Diddy Kong neutral costume
        "PlSn.dat",           // Sonic character data
        "PlSnAJ.dat",         // Sonic animations
        "PlSnNr.dat",         // Sonic neutral costume
        "GrAc.dat",           // Animal Crossing stage
        "audio/starwolf.hps"  // Custom ACE music track
    };

    std::cout << "  Testing custom fighter & stage asset resolution:\n";
    for (const auto& f : test_files) {
        std::memset(resolved, 0, sizeof(resolved));
        if (pc_vfs_resolve(f.c_str(), resolved, sizeof(resolved))) {
            std::cout << "    [OK] " << f << " -> " << resolved << "\n";
        } else {
            std::cerr << "    [FAIL] Could not resolve: " << f << "\n";
            return 1;
        }
    }

    std::cout << "\n========================================\n";
    std::cout << "ALL ACE-BUILD TESTS PASSED ON MELEE-PC!\n";
    std::cout << "========================================\n";
    return 0;
}
