#!/usr/bin/env python3
"""A fresh tick polls current hardware; replay/advance reuse the sent sample."""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
s = (ROOT / 'src/pc/net.c').read_text()
a = s.index('static void capture_local_sample(')
b = s.index('/* A fresh frame:', a)
source = r'''
#include <assert.h>
#include <stdbool.h>
typedef struct { int button; } PADStatus;
static PADStatus s_raw_last;
static int polls;
static void PADRead(PADStatus* pads) {
    ++polls;
    for (int i=0;i<4;++i) pads[i].button=100+i;
}
''' + s[a:b] + r'''
int main(void) {
    s_raw_last.button=7;
    capture_local_sample(false);
    assert(polls==0 && s_raw_last.button==7);
    capture_local_sample(true);
    assert(polls==1 && s_raw_last.button==100);
    capture_local_sample(false);
    assert(polls==1 && s_raw_last.button==100);
}
'''
with tempfile.TemporaryDirectory() as d:
    p=Path(d)/'test.c'; p.write_text(source)
    subprocess.run(['cc','-std=c11','-Wall','-Werror',str(p),'-o',d+'/test'],check=True)
    subprocess.run([d+'/test'],check=True)
print('PASS: late fresh input and preserved replay/advance sample')
