#!/usr/bin/env python3
"""Exercise the production native hint lifecycle with an optional Android API."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent


class AndroidPerformance(unittest.TestCase):
    def test_optional_hint_session_lifecycle(self):
        header = ROOT / 'extern/aurora/lib/android_performance.hpp'
        self.assertTrue(header.exists(), 'native performance-hint trial is missing')
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp / 'android').mkdir()
            (tmp / 'android/performance_hint.h').write_text(r'''
#pragma once
#include <cstdint>
#include <cstddef>
struct APerformanceHintManager {};
struct APerformanceHintSession {};
APerformanceHintManager* APerformanceHint_getManager();
APerformanceHintSession* APerformanceHint_createSession(APerformanceHintManager*, const int32_t*, size_t, int64_t);
int APerformanceHint_reportActualWorkDuration(APerformanceHintSession*, int64_t);
int APerformanceHint_updateTargetWorkDuration(APerformanceHintSession*, int64_t);
void APerformanceHint_closeSession(APerformanceHintSession*);
''')
            (tmp / 'android/log.h').write_text('''
#define ANDROID_LOG_INFO 4
inline int __android_log_print(int, const char*, const char*, ...) { return 0; }
''')
            source = r'''
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <unistd.h>
#include <android/performance_hint.h>
static bool symbols = true, supported = true;
static int created, reported, closed, updated, error;
APerformanceHintManager manager;
APerformanceHintSession session;
APerformanceHintManager* APerformanceHint_getManager() { return &manager; }
APerformanceHintSession* APerformanceHint_createSession(APerformanceHintManager*, const int32_t* tids, size_t n, int64_t target) {
    assert(n == 1 && tids[0] == gettid() && target > 0);
    ++created;
    return supported ? &session : nullptr;
}
int APerformanceHint_reportActualWorkDuration(APerformanceHintSession*, int64_t duration) {
    assert(duration > 0); ++reported; return error;
}
int APerformanceHint_updateTargetWorkDuration(APerformanceHintSession*, int64_t duration) {
    assert(duration > 0); ++updated; return error;
}
void APerformanceHint_closeSession(APerformanceHintSession*) { ++closed; }
extern "C" void* dlsym(void*, const char* name) noexcept {
    if (!symbols) return nullptr;
#define SYM(x) if (strcmp(name, #x) == 0) return reinterpret_cast<void*>(&x)
    SYM(APerformanceHint_getManager);
    SYM(APerformanceHint_createSession);
    SYM(APerformanceHint_reportActualWorkDuration);
    SYM(APerformanceHint_updateTargetWorkDuration);
    SYM(APerformanceHint_closeSession);
    return nullptr;
}
''' + f'\n#include "{header}"\n' + r'''
int main(int argc, char**) {
    if (argc > 1) symbols = false;
    {
        aurora::PerformanceHints hints;
        hints.report(100, 200);
        assert(created == 0); // opt-in only
    }
    setenv("MELEE_ANDROID_PERF_HINTS", "1", 1);
    {
        aurora::PerformanceHints hints;
        hints.report(0, 200);
        hints.report(100, 0);
        hints.report(UINT64_MAX, 200);
        assert(created == 0);
        hints.report(100, 200);
        if (!symbols) { assert(created == 0); return 0; }
        assert(created == 1 && reported == 1);
        hints.report(120, 200);
        assert(created == 1 && reported == 2 && updated == 0);
        hints.report(120, 220);
        assert(updated == 1);
        hints.reset();
        assert(closed == 1);
        hints.report(100, 200);
        assert(created == 2);
        error = 32; // EPIPE: close and stop retrying each frame
        hints.report(100, 200);
        assert(closed == 2);
        hints.report(100, 200);
        assert(created == 2);
        hints.reset();
        error = 0;
        supported = false;
        hints.report(100, 200);
        hints.report(100, 200);
        assert(created == 3);
        hints.reset();
        supported = true;
        hints.report(100, 200);
        assert(created == 4);
    }
    assert(closed == 3); // thread exit releases the last session
}
'''
            (tmp / 'test.cpp').write_text(source)
            subprocess.run(['c++', '-std=c++20', '-D__ANDROID__', '-I', str(tmp),
                            str(tmp / 'test.cpp'), '-o', str(tmp / 'test')], check=True)
            subprocess.run([str(tmp / 'test')], check=True)
            subprocess.run([str(tmp / 'test'), 'missing-symbols'], check=True)


if __name__ == '__main__':
    unittest.main()
