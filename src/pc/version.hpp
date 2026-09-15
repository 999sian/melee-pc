/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <string>
#include <string_view>

namespace pc {

struct SemVer {
    int major = 0;
    int minor = 0;
    int patch = 0;
    std::string prerelease;
    int prerelease_num = 0;
    int build_commits = 0;
    bool valid = false;

    static SemVer parse(std::string_view s);
    std::string to_string() const;

    bool operator<(const SemVer& o) const;
    bool operator>(const SemVer& o) const { return o < *this; }
    bool operator<=(const SemVer& o) const { return !(o < *this); }
    bool operator>=(const SemVer& o) const { return !(*this < o); }
    bool operator==(const SemVer& o) const;
    bool operator!=(const SemVer& o) const { return !(*this == o); }
};

const std::string& get_app_version();
bool is_update_available(std::string_view current_ver, std::string_view latest_ver);

} // namespace pc
