/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pc/mex/mex_runtime.hpp"
#include "pc/mex/mex_c_api.h"
#include "pc/gecko/gecko_engine.hpp"

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
        std::filesystem::temp_directory_path() / "melee_pc_mex_gecko_test";
    std::error_code ec;
    std::filesystem::remove_all(tmp_root, ec);
    std::filesystem::create_directories(tmp_root);

    // ==========================================
    // 1. Test MexRuntime
    // ==========================================
    std::filesystem::path proj_dir = tmp_root / "ace_proj";
    write_file(proj_dir / "ace.mexproj", R"({
        "startingScene": 2,
        "build": {
            "name": "Akaneia Community Edition",
            "longName": "The Ace Build v1.0.1",
            "shortMaker": "Team Akaneia, TurboDX, Team ACE",
            "majorVersion": 1,
            "minorVersion": 0,
            "patchVersion": 1
        }
    })");

    write_file(proj_dir / "data" / "fighters" / "045.json", R"({
        "name": "Sonic",
        "seriesID": 10,
        "soundBank": 85,
        "ssmBitfield1": 1024,
        "ssmBitfield2": 2048,
        "announcerCall": 510050,
        "victoryTheme": 22,
        "fighterMusic1": 60,
        "fighterMusic2": 60,
        "files": {
            "fighterDataPath": "PlSn.dat",
            "fighterDataSymbol": "ftDataSonic",
            "rstAnimFile": "GmRstMSn.dat"
        },
        "costumes": [
            { "file": { "fileName": "PlSnNr.dat" } },
            { "file": { "fileName": "PlSnRd.dat" } }
        ]
    })");

    write_file(proj_dir / "data" / "fighters" / "046.json", R"({
        "name": "Ninten",
        "seriesID": 8,
        "soundBank": 86,
        "ssmBitfield1": 0,
        "ssmBitfield2": 2097152,
        "announcerCall": 510054,
        "victoryTheme": 23,
        "fighterMusic1": 61,
        "fighterMusic2": 61
    })");

    write_file(proj_dir / "data" / "stages" / "315.json", R"({
        "name": "Target Test",
        "fileName": "GrTgt.dat",
        "musicID": 55
    })");

    write_file(proj_dir / "data" / "css.json", R"({
        "entries": [
            { "characterID": 45, "x": 10.5, "y": 20.0 },
            { "characterID": 46, "x": 15.5, "y": 20.0 }
        ]
    })");

    auto& mex = pc::mex::MexRuntime::instance();
    assert(mex.load_project(proj_dir));
    assert(mex.is_active());
    assert(mex.get_project_name() == "The Ace Build v1.0.1");
    assert(mex.get_project_version() == "1.0.1");
    assert(mex.get_fighter_count() == 2);

    const auto* sonic = mex.get_fighter_by_kind(45);
    assert(sonic != nullptr);
    assert(sonic->name == "Sonic");
    assert(sonic->series_id == 10);
    assert(sonic->sound_bank == 85);
    assert(sonic->ssm_bitfield1 == 1024);

    const auto* ninten = mex.get_fighter_by_kind(46);
    assert(ninten != nullptr);
    assert(ninten->name == "Ninten");
    assert(ninten->sound_bank == 86);

    const auto* stage = mex.get_stage_by_id(315);
    assert(stage != nullptr);
    assert(stage->name == "Target Test");
    assert(stage->filename == "GrTgt.dat");
    assert(stage->music_id == 55);

    assert(mex.get_css_slots().size() == 2);
    assert(mex.get_css_slots()[0].fighter_kind == 45);
    assert(mex.get_css_slots()[1].fighter_kind == 46);

    // C ABI check
    assert(pc_mex_is_active() == 1);
    assert(pc_mex_get_fighter_count() == 2);
    assert(std::string(pc_mex_get_fighter_name(45)) == "Sonic");
    assert(std::string(pc_mex_get_fighter_name(46)) == "Ninten");
    assert(std::string(pc_mex_get_fighter_filename(45)) == "PlSn.dat");
    assert(std::string(pc_mex_get_fighter_data_symbol(45)) == "ftDataSonic");
    assert(std::string(pc_mex_get_fighter_rst_anim_file(45)) == "GmRstMSn.dat");
    assert(pc_mex_get_fighter_announcer_call(45) == 510050);
    assert(pc_mex_get_fighter_costume_count(45) == 2);
    assert(std::string(pc_mex_get_fighter_costume_file(45, 0)) == "PlSnNr.dat");
    assert(std::string(pc_mex_get_fighter_costume_file(45, 1)) == "PlSnRd.dat");
    assert(pc_mex_get_css_icon_count() == 2);

    struct DummyCSSIcon {
        uint8_t ft_hudindex;
        uint8_t char_kind;
        uint8_t state;
        uint8_t anim_timer;
        uint8_t joint_id_vs;
        uint8_t joint_id_1p;
        uint8_t pad[2];
        int32_t sfx;
        float bound_l;
        float bound_r;
        float bound_u;
        float bound_d;
    } icons[64];
    assert(pc_mex_populate_css_icons(icons, 64));
    assert(icons[0].char_kind == 45);
    assert(icons[0].state == 2);
    assert(icons[1].char_kind == 46);
    assert(icons[1].state == 2);

    // ==========================================
    // 2. Test GeckoEngine
    // ==========================================
    auto& gecko = pc::gecko::GeckoEngine::instance();
    gecko.reset();

    std::string gecko_test_code = R"(
        $Test Code
        040E0A20 12345678
        020E0A24 0000ABCD
        000E0A26 000000EF
        C2068B40 00000001
        7C0802A6 90010004
    )";

    gecko.apply_code_string(gecko_test_code);

    // Check 04 32-bit poke
    assert(gecko.read32(0x800E0A20) == 0x12345678);

    // Check 02 16-bit poke
    assert(gecko.read16(0x800E0A24) == 0xABCD);

    // Check 00 8-bit poke
    assert(gecko.read8(0x800E0A26) == 0xEF);

    // Check C2 hook injection at 0x80068B40
    assert(gecko.read32(0x80068B40) == 0x7C0802A6);
    assert(gecko.read32(0x80068B44) == 0x90010004);

    assert(gecko.is_modified(0x800E0A20));
    assert(!gecko.is_modified(0x80000000));

    // Cleanup
    std::filesystem::remove_all(tmp_root, ec);
    std::printf("PASS: MexRuntime ingestion and GeckoEngine memory patching\n");
    return 0;
}
