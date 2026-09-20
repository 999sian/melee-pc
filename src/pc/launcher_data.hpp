/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <atomic>
#include <filesystem>
#include <string>

namespace launcher {
struct DiscInfo {
    bool supported = false;
    std::string message;
};
enum class VerifyState { Verified, Mismatch, Canceled, Error };
struct Verification {
    VerifyState state = VerifyState::Error;
    std::string message;
};
struct Preferences {
    std::string disc;
    std::string net_name = "PLAYER";
    std::string net_target;  // empty hosts our own connect code
    int net_delay = -1;      // auto; otherwise 0..4 frames
    int net_port = 0;        // operating-system allocated

    bool vsync = true;
    bool fullscreen = false;
    float scale = 1.0f;
#if defined(__ANDROID__)
    float render_scale = 1.0f, volume = 1.0f;
    int msaa = 1, anisotropy = 1;
#else
    float render_scale = 0.0f, volume = 1.0f;
    int msaa = 1, anisotropy = 16;
#endif
    int widescreen = 0;
    int filter_mode = 0;
    int backend = 0;
    bool mute = false, fps = false;
    bool check_updates = true;
    bool custom_textures = true;
    bool unlock_all = false;
    int hud_mode = 0;
    bool frozen_stadium = false;
    bool free_camera = false;
    bool ucf = false;
    float music_volume = 1.0f;
    float sfx_volume = 1.0f;
    uint64_t install_id = 0;  // random once per install (LAN host election); 0 = not yet
};
DiscInfo inspect_disc(const std::string& path);
Verification verify_disc(
    const std::string& path, std::atomic_bool& cancel, std::atomic_uint& progress);
Preferences load_preferences(const std::filesystem::path& path);
bool save_preferences(
    const std::filesystem::path& path, const Preferences& prefs, std::string& error);
}  // namespace launcher
