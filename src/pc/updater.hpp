/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <string>
#include <vector>
#include <memory>
#include <functional>

namespace pc::updater {

enum class Status {
    Idle,
    Checking,
    UpdateAvailable,
    UpToDate,
    Downloading,
    Downloaded,
    Failed
};

struct Asset {
    std::string name;
    std::string download_url;
    size_t size = 0;
};

struct Release {
    std::string tag_name;
    std::string name;
    std::string html_url;
    std::string body;
    bool prerelease = false;
    std::string published_at;
    std::vector<Asset> assets;
};

struct UpdateState {
    Status status = Status::Idle;
    std::string message;
    Release latest_release;
    std::string target_asset_name;
    std::string target_asset_url;
    size_t download_total_bytes = 0;
    size_t download_current_bytes = 0;
    float download_progress = 0.0f; // 0.0 to 1.0
    std::string downloaded_path;
    bool restart_supported = false;
};

void check_for_updates_async(bool include_prereleases = true);
void start_download_async();
void cancel();
UpdateState get_state();

void open_release_in_browser();
void open_downloaded_location();
bool apply_update_and_restart(std::string& error);

} // namespace pc::updater
