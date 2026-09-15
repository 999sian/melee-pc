/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "version.hpp"
#include <charconv>
#include <tuple>

#ifndef MELEE_APP_VERSION
#define MELEE_APP_VERSION "v0.1.4-beta"
#endif

namespace pc {

const std::string& get_app_version() {
    static const std::string version = MELEE_APP_VERSION;
    return version;
}

SemVer SemVer::parse(std::string_view s) {
    SemVer v;
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);

    if (!s.empty() && (s.front() == 'v' || s.front() == 'V')) {
        s.remove_prefix(1);
    }

    if (s.empty()) return v;

    // Parse major
    size_t idx = 0;
    while (idx < s.size() && s[idx] >= '0' && s[idx] <= '9') ++idx;
    if (idx == 0) return v;
    std::from_chars(s.data(), s.data() + idx, v.major);
    s.remove_prefix(idx);

    // Minor
    if (!s.empty() && s.front() == '.') {
        s.remove_prefix(1);
        idx = 0;
        while (idx < s.size() && s[idx] >= '0' && s[idx] <= '9') ++idx;
        if (idx > 0) {
            std::from_chars(s.data(), s.data() + idx, v.minor);
            s.remove_prefix(idx);
        }
    }

    // Patch
    if (!s.empty() && s.front() == '.') {
        s.remove_prefix(1);
        idx = 0;
        while (idx < s.size() && s[idx] >= '0' && s[idx] <= '9') ++idx;
        if (idx > 0) {
            std::from_chars(s.data(), s.data() + idx, v.patch);
            s.remove_prefix(idx);
        }
    }

    v.valid = true;

    // Pre-release or build metadata (-beta, -rc1, etc.)
    if (!s.empty() && s.front() == '-') {
        s.remove_prefix(1);
        // Look for git describe suffix: -<commits>-g<hash>
        // e.g. "beta-2-gadaf84057" or "2-gadaf84057"
        auto git_g = s.rfind("-g");
        if (git_g != std::string_view::npos) {
            auto commit_start = s.rfind('-', git_g - 1);
            size_t c_start = (commit_start == std::string_view::npos) ? 0 : commit_start + 1;
            int count = 0;
            auto [ptr, ec] = std::from_chars(s.data() + c_start, s.data() + git_g, count);
            if (ec == std::errc()) {
                v.build_commits = count;
                s = s.substr(0, (commit_start == std::string_view::npos) ? 0 : commit_start);
            }
        }

        if (!s.empty()) {
            // Find any number at the end of prerelease, e.g. "beta.1", "beta2", "rc3"
            size_t num_pos = s.size();
            while (num_pos > 0 && s[num_pos - 1] >= '0' && s[num_pos - 1] <= '9') {
                --num_pos;
            }
            if (num_pos < s.size()) {
                std::from_chars(s.data() + num_pos, s.data() + s.size(), v.prerelease_num);
                auto pre = s.substr(0, num_pos);
                if (!pre.empty() && pre.back() == '.') pre.remove_suffix(1);
                v.prerelease = std::string(pre);
            } else {
                v.prerelease = std::string(s);
            }
        }
    }

    return v;
}

std::string SemVer::to_string() const {
    std::string res = std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
    if (!prerelease.empty()) {
        res += "-" + prerelease;
        if (prerelease_num > 0) res += "." + std::to_string(prerelease_num);
    }
    if (build_commits > 0) {
        res += "+" + std::to_string(build_commits);
    }
    return res;
}

bool SemVer::operator==(const SemVer& o) const {
    if (!valid || !o.valid) return false;
    return std::tie(major, minor, patch, prerelease, prerelease_num, build_commits) ==
           std::tie(o.major, o.minor, o.patch, o.prerelease, o.prerelease_num, o.build_commits);
}

bool SemVer::operator<(const SemVer& o) const {
    if (!valid || !o.valid) return false;
    if (major != o.major) return major < o.major;
    if (minor != o.minor) return minor < o.minor;
    if (patch != o.patch) return patch < o.patch;

    // Normal release > pre-release
    // (Empty prerelease is a stable release, which is greater than any prerelease)
    if (prerelease.empty() != o.prerelease.empty()) {
        return !prerelease.empty(); // If this has prerelease and other does not, this < other
    }

    if (prerelease != o.prerelease) {
        return prerelease < o.prerelease;
    }

    if (prerelease_num != o.prerelease_num) {
        return prerelease_num < o.prerelease_num;
    }

    return build_commits < o.build_commits;
}

bool is_update_available(std::string_view current_ver, std::string_view latest_ver) {
    auto cur = SemVer::parse(current_ver);
    auto lat = SemVer::parse(latest_ver);
    if (!cur.valid || !lat.valid) return false;
    return cur < lat;
}

} // namespace pc
