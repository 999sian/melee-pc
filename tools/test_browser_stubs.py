#!/usr/bin/env python3
"""Link browser preferences and verify keyboard defaults and persistent paths."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from net_test_support import sdl_includes

ROOT = Path(__file__).resolve().parent.parent


class BrowserStubs(unittest.TestCase):
    def test_preferences_match_desktop_defaults(self):
        with tempfile.TemporaryDirectory(prefix='browser-stubs-') as directory:
            path = Path(directory)
            includes = ['-I' + str(ROOT / 'src'), *['-I' + str(p) for p in sdl_includes(ROOT)]]
            subprocess.run(['cc', *includes, '-c', str(ROOT / 'platforms/browser/pc_stubs.c'),
                            '-o', str(path / 'stubs.o')], check=True)
            (path / 'test.cpp').write_text('''
#include "pc/pc.h"
#include "pc/paths.h"
#include "pc/launcher_data.hpp"
#include <SDL3/SDL_stdinc.h>
#include <cassert>
#include <cstdlib>
#include <cstring>
extern "C" char* SDL_strdup(const char* text) { return strdup(text); }
extern "C" void SDL_free(void* pointer) { free(pointer); }
int main() {
    for (int slot = 0; slot < int(launcher::default_keys.size()); ++slot)
        assert(pc_get_key_binding(slot) == launcher::default_keys[slot]);
    assert(pc_get_key_binding(-1) == 0);
    assert(pc_get_key_binding(22) == 0);
    assert(!pc_is_portable());
    char* path = pc_get_pref_path();
    assert(path && strcmp(path, "/saves/") == 0);
    SDL_free(path);
}
''')
            subprocess.run(['c++', '-std=c++20', *includes, str(path / 'test.cpp'),
                            str(path / 'stubs.o'), '-o', str(path / 'test')], check=True)
            subprocess.run([str(path / 'test')], check=True)


if __name__ == '__main__':
    unittest.main()
