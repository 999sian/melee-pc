#!/usr/bin/env python3
"""Exercise the production launcher entry point for Android disc intents."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent


class ShaderStartup(unittest.TestCase):
    def test_disc_intents_reach_android_shader_wait(self):
        source = (ROOT/'src/pc/launcher.cpp').read_text()
        entry = source[source.index('extern "C" int pc_launcher_run('):source.index('// In-game settings use')]
        harness = r'''
#include <cassert>
#include <filesystem>
#include <string>
struct SDL_Window {};
using AuroraSampler = int;
struct { std::string disc; float scale = 1; int filter_mode = 0; } prefs;
std::filesystem::path config_path;
bool valid = true, opened = true, shown = false, requested = false;
namespace launcher {
struct Info { bool supported; std::string message; };
Info inspect_disc(const char*) { return {valid, "Invalid disc"}; }
template<class... T> bool save_preferences(T&&...) { return true; }
}
bool aurora_dvd_open(const char*) { return opened; }
void aurora_dvd_close() {}
void pc_load_disc_fonts(const char*) {}
void ensure_rmlui_input_layer() {}
void SDL_Log(const char*, ...) {}
struct Document {} document;
struct Context { Document* LoadDocument(const std::string&) { return &document; } } context;
namespace aurora::rmlui {
Context* get_context() { return &context; }
void set_ui_scale(float) {}
}
std::filesystem::path pc_resources_path() { return "."; }
bool load_menu_fonts(const std::filesystem::path&) { return true; }
void aurora_set_resampler(int) {}
struct Launcher {
Launcher(SDL_Window*, Document*) { shown = true; }
int run(const std::string&, bool start_requested = false) {
    requested = start_requested;
    return 7; // The real loop handles pipeline draining before auto-start.
}
};
''' + entry + r'''
int main() {
    const char* uri = "content://disc/melee.ciso";
    assert(pc_launcher_run(uri, nullptr) == 7);
    assert(shown && requested && prefs.disc == uri);
    shown = requested = false;
    assert(pc_launcher_run(nullptr, nullptr) == 7);
    assert(shown && !requested); // Icon launch stays in the launcher.
    valid = false; shown = requested = false;
    assert(pc_launcher_run(uri, nullptr) == 7);
    assert(shown && !requested); // Invalid intent must not auto-start.
    valid = true; opened = false; shown = requested = false;
    assert(pc_launcher_run(uri, nullptr) == 7);
    assert(shown && !requested);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            cpp = Path(tmp)/'startup.cpp'; cpp.write_text(harness)
            for defines in (['-D__ANDROID__'], []):
                binary = Path(tmp)/'test'
                subprocess.run(['c++','-std=c++17',*defines,str(cpp),'-o',str(binary)],check=True)
                subprocess.run([str(binary)],check=True)


if __name__ == '__main__':
    unittest.main()
