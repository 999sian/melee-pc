/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pc/vfs/vfs.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

static void write_file(const std::filesystem::path& p, const std::string& content) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out << content;
}

int main() {
    std::filesystem::path tmp_root = std::filesystem::temp_directory_path() / "melee_pc_vfs_test";
    std::error_code ec;
    std::filesystem::remove_all(tmp_root, ec);
    std::filesystem::create_directories(tmp_root);

    std::filesystem::path base_dir = tmp_root / "base";
    std::filesystem::path mod1_dir = tmp_root / "mod1";
    std::filesystem::path mod2_dir = tmp_root / "mod2";

    // Setup files:
    // Base has PlFxNr.dat (content: "base_fox") and Stage.dat ("base_stage")
    write_file(base_dir / "files" / "PlFxNr.dat", "base_fox");
    write_file(base_dir / "files" / "Stage.dat", "base_stage");

    // Mod 1 (priority 50) overrides PlFxNr.dat ("mod1_fox") and adds NewFighter.dat ("mod1_new")
    write_file(mod1_dir / "PlFxNr.dat", "mod1_fox");
    write_file(mod1_dir / "NewFighter.dat", "mod1_new");

    // Mod 2 (priority 80) overrides PlFxNr.dat ("mod2_fox")
    write_file(mod2_dir / "files" / "plfxnr.dat", "mod2_fox");  // Lowercase test!

    pc::vfs::init(base_dir);
    assert(pc::vfs::has_file("PlFxNr.dat"));
    assert(pc::vfs::has_file("Stage.dat"));
    assert(!pc::vfs::has_file("NewFighter.dat"));

    auto base_read = pc::vfs::read_file("PlFxNr.dat");
    assert(std::string(base_read.begin(), base_read.end()) == "base_fox");

    // Mount Mod 1
    assert(pc::vfs::mount("mod1", mod1_dir, 50));
    assert(pc::vfs::has_file("NewFighter.dat"));
    auto mod1_read = pc::vfs::read_file("PlFxNr.dat");
    assert(std::string(mod1_read.begin(), mod1_read.end()) == "mod1_fox");

    // Mount Mod 2 (higher priority 80)
    assert(pc::vfs::mount("mod2", mod2_dir, 80));
    auto mod2_read = pc::vfs::read_file("PlFxNr.dat");
    assert(std::string(mod2_read.begin(), mod2_read.end()) == "mod2_fox");

    // Case-insensitivity test: query with uppercase, mixed case, lowercase
    assert(pc::vfs::has_file("PLFXNR.DAT"));
    assert(pc::vfs::has_file("plfxnr.dat"));
    assert(pc::vfs::has_file("files/PlFxNr.dat"));
    assert(pc::vfs::has_file("stage.dat"));

    // Disable Mod 2 -> Mod 1 should shine through
    assert(pc::vfs::set_mount_enabled("mod2", false));
    auto fallback1 = pc::vfs::read_file("PlFxNr.dat");
    assert(std::string(fallback1.begin(), fallback1.end()) == "mod1_fox");

    // Disable Mod 1 -> Base should shine through
    assert(pc::vfs::set_mount_enabled("mod1", false));
    auto fallback_base = pc::vfs::read_file("PlFxNr.dat");
    assert(std::string(fallback_base.begin(), fallback_base.end()) == "base_fox");

    // C ABI test
    char out_path[1024] = {};
    assert(pc_vfs_has_file("Stage.dat") == 1);
    assert(pc_vfs_resolve("Stage.dat", out_path, sizeof(out_path)) == 1);
    assert(std::string(out_path).find("Stage.dat") != std::string::npos);

    // Cleanup
    std::filesystem::remove_all(tmp_root, ec);
    std::printf("PASS: VFS priority mounting, case-insensitivity, override, and C ABI\n");
    return 0;
}
