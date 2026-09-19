#!/usr/bin/env python3
"""Exercise the production axis expression and PADRead's final merge/filter path."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
SOURCE = (ROOT / 'extern/aurora/lib/dolphin/pad/pad.cpp').read_text()

def run_cpp(body):
    with tempfile.TemporaryDirectory(prefix='pad-regression-') as d:
        src = Path(d) / 'test.cpp'
        src.write_text('#include <algorithm>\n#include <cassert>\n#include <cstdint>\n#include <cstdlib>\nusing Sint16=int16_t; using s8=int8_t; using u32=uint32_t;\n' + body)
        subprocess.run(['c++', '-std=c++20', str(src), '-o', d + '/test'], check=True)
        subprocess.run([d + '/test'], check=True)

class PadRegressions(unittest.TestCase):
    def test_direction_bindings(self):
        expressions = re.findall(r'auto (?:xl|yl|xr|yr) = static_cast<Sint16>\((.*)\);', SOURCE)
        self.assertEqual(len(expressions), 4)
        for expr, prefix in zip(expressions, ('xl', 'yl', 'xr', 'yr')):
            expr = expr.replace(prefix + 'Pos', 'pos').replace(prefix + 'Neg', 'neg')
            run_cpp('int axis(int pos,int neg) { return ' + expr + '; }\n' + '''
int main() {
  assert(axis(32767, 0) == 32767); // button to positive direction
  assert(axis(0, 32767) == -32767); // button to negative direction
  assert(axis(0, 0) == 0);
  assert(axis(32767, 32767) == 0); // simultaneous opposites cancel
  assert(axis(12345, -12345) == 12345); // default analog pair
  assert(axis(-12345, 12345) == -12345);
  assert(axis(-32768, 32767) == -32767);
}''')

    def test_virtual_hold_suppressed_until_release(self):
        functions = SOURCE[SOURCE.index('static void neutralize_status('):SOURCE.index('u32 PADRead(')]
        block = SOURCE[SOURCE.index('    if (g_blockPAD) {', SOURCE.index('u32 PADRead(')):SOURCE.index('\n  }\n  return rumbleSupport;', SOURCE.index('u32 PADRead('))]
        run_cpp('''
struct PADStatus { uint16_t button{}; uint32_t extButton{}; int8_t stickX{},stickY{},substickX{},substickY{}; uint8_t triggerLeft{},triggerRight{},analogA{},analogB{}; };
uint16_t g_suppressedButtons[4]{}; bool g_suppressLeftTrigger[4]{},g_suppressRightTrigger[4]{};
struct { int minTrigger=30; } ClampRegion;
bool g_blockPAD=false, g_virtualPadActive[4]{true}; PADStatus g_virtualPadStatus[4]{};
''' + functions + '\nvoid read(PADStatus* status,bool captureHeldInput) { int i=0;\n' + block + '''
}
int main() {
  PADStatus st[4]{};
  g_virtualPadStatus[0].button=0x100; g_virtualPadStatus[0].triggerLeft=100;
  read(st,true); assert(st[0].button==0 && st[0].triggerLeft==0);
  st[0]={}; read(st,false); assert(st[0].button==0 && st[0].triggerLeft==0);
  g_virtualPadStatus[0]={}; st[0]={}; read(st,false);
  g_virtualPadStatus[0].button=0x100; g_virtualPadStatus[0].triggerLeft=100;
  st[0]={}; read(st,false); assert(st[0].button==0x100 && st[0].triggerLeft==100);
  g_blockPAD=true; st[0]={}; read(st,false); assert(st[0].button==0 && st[0].triggerLeft==0);
}
''')

if __name__ == '__main__': unittest.main()
