/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <atomic>
#include <array>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_gamepad.h>
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
inline constexpr std::array<int, 22> default_keys = {SDL_SCANCODE_X, SDL_SCANCODE_Z, SDL_SCANCODE_C,
    SDL_SCANCODE_V, SDL_SCANCODE_RETURN, SDL_SCANCODE_TAB, SDL_SCANCODE_Q, SDL_SCANCODE_E,
    SDL_SCANCODE_T, SDL_SCANCODE_G, SDL_SCANCODE_F, SDL_SCANCODE_H, SDL_SCANCODE_RIGHT,
    SDL_SCANCODE_LEFT, SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_L, SDL_SCANCODE_J,
    SDL_SCANCODE_I, SDL_SCANCODE_K, SDL_SCANCODE_Q, SDL_SCANCODE_E};
struct Preferences {
    std::array<int, 22> keys = default_keys;
    int menu_button = SDL_GAMEPAD_BUTTON_BACK;
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
    bool mute = false, fps = false, reverb = true;
    bool check_updates = true;
    bool custom_textures = true;
    bool unlock_all = false;
    int hud_mode = 0;
    bool frozen_stadium = false;
    bool free_camera = false;
    bool ucf = false;
    bool cstick_1p = false;
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
