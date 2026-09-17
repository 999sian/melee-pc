/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pc/updater.hpp"
#include "pc/version.hpp"
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

int main() {
    using namespace pc;

    std::cout << "Testing Melee-PC Updater...\n";
    std::cout << "Current App Version: " << get_app_version() << "\n";

    // Test async check
    updater::check_for_updates_async(true);

    // Wait up to 5 seconds for network check
    for (int i = 0; i < 50; ++i) {
        auto state = updater::get_state();
        if (state.status != updater::Status::Checking) {
            std::cout << "Check finished with status " << static_cast<int>(state.status) << ": "
                      << state.message << "\n";
            if (!state.latest_release.tag_name.empty()) {
                std::cout << "Latest release found: " << state.latest_release.tag_name << " ("
                          << state.latest_release.name << ")\n";
                std::cout << "Target asset: " << state.target_asset_name
                          << " (size: " << state.download_total_bytes << " bytes)\n";
            }
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    auto final_state = updater::get_state();
    assert(final_state.status == updater::Status::UpToDate ||
           final_state.status == updater::Status::UpdateAvailable ||
           final_state.status == updater::Status::Failed);

    // Test cancellation
    updater::check_for_updates_async(true);
    updater::cancel();
    auto canceled_state = updater::get_state();
    assert(canceled_state.status != updater::Status::Checking);

    // Test version comparisons
    assert(is_update_available(get_app_version(), "v0.1.7-beta"));
    assert(is_update_available(get_app_version(), "v0.2.0"));
    assert(!is_update_available(get_app_version(), "v0.1.6-beta"));
    assert(!is_update_available(get_app_version(), "v0.1.5-beta"));
    assert(!is_update_available(get_app_version(), "v0.1.4-beta"));
    assert(!is_update_available(get_app_version(), "v0.1.0-beta"));

    std::cout << "PASS: Updater async check, cancellation, and state transitions\n";
    return 0;
}
