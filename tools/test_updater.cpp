/* SPDX-License-Identifier: GPL-3.0-or-later */
// Include the implementation to exercise private state without network or a browser.
#include "../src/pc/updater.cpp"
#include <cassert>
#include <iostream>

static std::string opened_url;
extern "C" bool SDL_OpenURL(const char* url) {
    opened_url = url;
    return true;
}

int main(int argc, char**) {
    using namespace pc::updater;
    if (argc > 1) {
        g_updater_state.status = Status::UpdateAvailable;
        g_updater_state.latest_release.html_url = "https://example.com/release";
        start_download_async();
        assert(opened_url == "https://example.com/release");
        assert(get_state().status == Status::UpdateAvailable);
        std::cout << "PASS: asset-free release opens browser without deadlocking\n";
        return 0;
    }
    std::string url = "stale URL";
    size_t size = 123;
    assert(select_best_asset({}, url, size).empty());
    assert(url.empty() && size == 0);
#if defined(__linux__) && defined(__x86_64__)
    std::vector<Asset> incompatible{{"Melee-aarch64.AppImage", "arm", 1}};
    assert(select_best_asset(incompatible, url, size).empty());
#endif
    const std::vector<Asset> releases{
        {"Melee-aarch64.AppImage", "linux-arm", 10},
        {"Melee-Windows-arm64.zip", "windows-arm", 20},
        {"Melee-macOS-arm64.zip", "mac-arm", 30},
        {"Melee-x86_64.AppImage", "linux-x86", 40},
        {"Melee-Windows-x86_64.zip", "windows-x86", 50},
        {"Melee-macOS-x86_64.zip", "mac-x86", 60},
        {"Melee-Android-arm64.apk", "android", 70},
        {"Melee-iOS-arm64.ipa", "ios", 80},
        {"melee-linux-x86_64.tar.gz", "tar", 90},
    };
    struct Case {
        const char* platform;
        const char* arch;
        const char* url;
        size_t size;
    };
    for (const auto& c : std::vector<Case>{{"Linux", "arm64", "linux-arm", 10},
             {"Windows", "arm64", "windows-arm", 20}, {"macOS", "arm64", "mac-arm", 30},
             {"Linux", "x86_64", "linux-x86", 40}, {"Windows", "x86_64", "windows-x86", 50},
             {"macOS", "x86_64", "mac-x86", 60}, {"Android", "arm64", "android", 70},
             {"iOS", "arm64", "ios", 80}})
    {
        assert(!select_best_asset(releases, url, size, c.platform, c.arch).empty());
        assert(url == c.url && size == c.size);
    }
    assert(select_best_asset(releases, url, size, "Linux", "riscv64").empty());
    assert(url.empty() && size == 0);
    assert(select_best_asset(
        {{"Melee-x86_64.AppImage.sha256", "checksum", 1}}, url, size, "Linux", "x86_64")
            .empty());
    assert(select_best_asset({{"melee-linux-x86_64.tar.gz", "tar", 90}}, url, size, "Linux",
               "x86_64") == "melee-linux-x86_64.tar.gz");
    assert(select_best_asset({{"Melee-x86_64.AppImage", "", 40}}, url, size, "Linux", "x86_64")
            .empty());
    assert(pc::is_update_available(pc::get_app_version(), "v99.0.0"));
    assert(!pc::is_update_available(pc::get_app_version(), pc::get_app_version()));
    std::cout << "PASS: asset selection rejects incompatible releases\n";
}
