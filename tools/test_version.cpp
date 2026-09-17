/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pc/version.hpp"
#include <cassert>
#include <iostream>

int main() {
    using namespace pc;

    auto v1 = SemVer::parse("v0.1.4-beta");
    assert(v1.valid);
    assert(v1.major == 0 && v1.minor == 1 && v1.patch == 4);
    assert(v1.prerelease == "beta");

    auto v2 = SemVer::parse("0.1.4");
    assert(v2.valid);
    assert(v2.major == 0 && v2.minor == 1 && v2.patch == 4);
    assert(v2.prerelease.empty());

    // Pre-release is strictly less than full release of same version
    assert(v1 < v2);
    assert(is_update_available("v0.1.4-beta", "v0.1.4"));

    auto v3 = SemVer::parse("v0.1.5-beta");
    assert(v1 < v3);
    assert(is_update_available("v0.1.4-beta", "v0.1.5-beta"));

    auto v4 = SemVer::parse("v0.1.6-beta");
    assert(v3 < v4);
    assert(is_update_available("v0.1.5-beta", "v0.1.6-beta"));

    auto v5 = SemVer::parse("v0.1.7-beta");
    assert(v4 < v5);
    assert(is_update_available("v0.1.6-beta", "v0.1.7-beta"));

    // Older release
    assert(!is_update_available("v0.1.4-beta", "v0.1.3"));
    assert(!is_update_available("v0.1.4-beta", "v0.1.0-beta"));

    // Same release
    assert(!is_update_available("v0.1.4-beta", "v0.1.4-beta"));

    // Git describe style
    auto v_git = SemVer::parse("v0.1.4-beta-2-gadaf84057");
    assert(v_git.valid);
    assert(v_git.major == 0 && v_git.minor == 1 && v_git.patch == 4);
    assert(v_git.build_commits == 2);
    assert(v1 < v_git);

    std::cout << "PASS: SemVer parsing and update checking\n";
    return 0;
}
