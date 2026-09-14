#pragma once
#include <atomic>
#include <filesystem>
#include <string>

namespace launcher {
struct DiscInfo { bool supported = false; std::string message; };
enum class VerifyState { Verified, Mismatch, Canceled, Error };
struct Verification { VerifyState state = VerifyState::Error; std::string message; };
struct Preferences {
    std::string disc;
    bool vsync = true;
    bool fullscreen = false;
    float scale = 1.0f;
    float render_scale = 0.0f, volume = 1.0f;
    int msaa = 1, anisotropy = 16;
    int widescreen = 0;
    int filter_mode = 0;
    bool mute = false, fps = false;
};
DiscInfo inspect_disc(const std::string& path);
Verification verify_disc(const std::string& path, std::atomic_bool& cancel, std::atomic_uint& progress);
Preferences load_preferences(const std::filesystem::path& path);
bool save_preferences(const std::filesystem::path& path, const Preferences& prefs, std::string& error);
}
