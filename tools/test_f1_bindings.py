#!/usr/bin/env python3
"""Exercise F1 binding capture against controlled native input sources."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent

def run_cpp(body):
    with tempfile.TemporaryDirectory(prefix='pad-regression-') as d:
        src = Path(d) / 'test.cpp'
        src.write_text('#include <algorithm>\n#include <cassert>\n#include <cstdint>\n#include <cstdlib>\nusing Sint16=int16_t; using s8=int8_t; using u32=uint32_t;\n' + body)
        subprocess.run(['c++', '-std=c++20', str(src), '-o', d + '/test'], check=True)
        subprocess.run([d + '/test'], check=True)

class F1Bindings(unittest.TestCase):
    def test_f1_buttons_accept_both_stick_directions(self):
        launcher = (ROOT / 'src/pc/launcher.cpp').read_text()
        body = launcher[launcher.index('    void poll_binding() {'):launcher.index('    void cancel_binding() {')]
        run_cpp('''
constexpr int PAD_BUTTON_COUNT=12;
struct { int menu_button=4; } prefs;
constexpr int AXIS_SIGN_POSITIVE=1, AXIS_SIGN_NEGATIVE=-1;
constexpr int SDL_GAMEPAD_AXIS_LEFT_TRIGGER=4, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER=5, SDL_GAMEPAD_BUTTON_INVALID=-1;
constexpr int PAD_TRIGGER_L=64, PAD_TRIGGER_R=32, PAD_AXIS_TRIGGER_L=8, PAD_AXIS_TRIGGER_R=9, SFX_CONFIRM=1;
using PADAxis=int;
struct PADSignedNativeAxis { int nativeAxis,sign; };
struct PADAxisMapping { PADSignedNativeAxis nativeAxis; int nativeButton,padAxis; };
struct PADButtonMapping { uint32_t nativeButton; int padButton; };
PADSignedNativeAxis pulled{2,1}; uint32_t captured=0;
int native_button=-1;
int PADGetNativeButtonPressed(int) { return native_button; }
PADSignedNativeAxis PADGetNativeAxisPulled(int) { return pulled; }
const PADAxisMapping* PADGetAxisMappings(int,uint32_t* count) { *count=0; return nullptr; }
const PADButtonMapping* PADGetButtonMappings(int,uint32_t* count) { static PADButtonMapping m{0,256}; *count=1; return &m; }
void PADSetAxisMapping(int,PADAxisMapping) {}
void PADSetButtonMapping(int,PADButtonMapping m) { captured=m.nativeButton; }
void PADSerializeMappings() {}
void lbAudioAx_80024030(int) {}
struct Menu { int binding=0,pad_port=0; bool binding_axis=false,binding_armed=true;
void label(const char*,const char*) {} void refresh_bindings() {} void saved() {}
''' + body + '''};
int main() {
  Menu menu; menu.poll_binding(); assert(captured==0x40000002u);
  pulled.sign=-1; menu.binding=0; menu.poll_binding(); assert(captured==0x60000002u);
  menu.binding=12; native_button=9; menu.poll_binding(); assert(prefs.menu_button==9);
  assert(menu.binding==-1);
}
''')

if __name__ == '__main__': unittest.main()
