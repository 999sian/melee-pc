/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <aurora/aurora.h>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <vector>

namespace aurora {
AuroraConfig g_config{};
char g_gameName[4]{};
void log_internal(AuroraLogLevel, const char*, const char*, unsigned int) noexcept {}
}  // namespace aurora

extern "C" {
uintptr_t OSBaseAddress = 0;

static std::vector<const void*> s_test_ext_ptrs;

uint32_t pc_register_ext_ptr(const void* p) {
    if (!p)
        return 0;
    for (size_t i = 0; i < s_test_ext_ptrs.size(); ++i) {
        if (s_test_ext_ptrs[i] == p)
            return (uint32_t)(i + 1);
    }
    s_test_ext_ptrs.push_back(p);
    return (uint32_t)s_test_ext_ptrs.size();
}

void* pc_resolve_ext_ptr(uint32_t id) {
    if (id == 0 || id > s_test_ext_ptrs.size())
        return nullptr;
    return const_cast<void*>(s_test_ext_ptrs[id - 1]);
}

void pc_disc_ptr_overflow(const void* p, const char* file, int line) {
    fprintf(stderr, "%s:%d: pointer %p overflow\n", file, line, p);
    abort();
}
}
