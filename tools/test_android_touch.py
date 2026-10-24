#!/usr/bin/env python3
"""Exercise actual Android touch hooks with a JNI type-only host stub."""
from pathlib import Path
from net_test_support import sdl_includes
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="melee-android-touch-") as directory:
    temp = Path(directory)
    (temp / "jni.h").write_text("""
#define JNIEXPORT
#define JNICALL
#define JNI_TRUE 1
typedef void JNIEnv;
typedef void* jclass;
typedef int jint;
typedef unsigned char jboolean;
""")
    (temp / "check.c").write_text(r'''
#include <SDL3/SDL.h>
#include <assert.h>
#define __ANDROID__ 1
#include "src/pc/touch.c"
void PADSetVirtualStatus(u32 port, const PADStatus* st) { (void)port; (void)st; }
void PADClearVirtualStatus(u32 port) { (void)port; }
void pc_net_match_stop(void) {}
void pc_net_disconnect(void) {}
void pc_lan_stop(void) {}
int main(void) {
    PADStatus st;
    SDL_Event event = {0};
    event.type = SDL_EVENT_FINGER_DOWN;
    event.tfinger.x = 0.9f;
    event.tfinger.y = 0.8f;
    pc_touch_set_active(false);
    pc_touch_event(&event);
    assert(!pc_touch_get_status(&st));
    Java_dev_melee_TouchControls_nativeSetTouchPad(NULL, NULL,
        PAD_BUTTON_A, 40, -30, 0, 0, 0, 0);
    pc_touch_event(&event);
    assert(pc_touch_get_status(&st));
    assert(st.button == PAD_BUTTON_A && st.stickX == 40 && st.stickY == -30);
    Java_dev_melee_TouchControls_nativeSetTouchActive(NULL, NULL, 0);
    pc_touch_event(&event);
    assert(!pc_touch_get_status(&st));
    return 0;
}
''')
    subprocess.run(["cc", "-DMELEE_PC=1", "-DTARGET_PC=1", "-I", str(temp),
                    "-I", str(root), "-I", str(root / "extern/aurora/include"),
                    "-I", str(root / "src"), "-I", str(root / "src/sdk_include"),
                    *[arg for path in sdl_includes(root) for arg in ("-I", str(path))],
                    str(temp / "check.c"), "-pthread", "-o", str(temp / "check")], check=True)
    subprocess.run([str(temp / "check")], check=True)
