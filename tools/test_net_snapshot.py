#!/usr/bin/env python3
"""Focused regressions for rollback snapshot restore and sync-test filtering."""
from pathlib import Path
from net_test_support import sdl_includes
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src/pc/net_snapshot.c"


class NetSnapshotTest(unittest.TestCase):
    def compile_and_run(self, body):
        with tempfile.TemporaryDirectory(prefix="net-snapshot-") as work:
            source = Path(work) / "test.c"
            source.write_text(body)
            exe = Path(work) / "test"
            subprocess.run(["cc", "-std=gnu11", str(source), "-o", str(exe)],
                           check=True, capture_output=True)
            return subprocess.run([str(exe)], capture_output=True, text=True)

    def test_restore_applies_saved_seed_value_after_pointer_changes(self):
        source = SOURCE.read_text()
        restore = source[source.index("void snapshot_restore("):
                         source.index("/* Re-simulation load", source.index("void snapshot_restore("))]
        body = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
typedef struct { void* ptr; size_t len; } Region;
typedef struct {
    unsigned char* buf;
    Region regions[2];
    int nregions;
    uint32_t* seed_ptr;
    uint32_t seed_val;
} Snapshot;
static uint32_t seed_a, seed_b;
static uint32_t* HSD_RandSeedPtr = &seed_b;
static uint64_t s_restore_ns, s_restores, s_restore_ns_max, s_restore_ns_worst;
static uint64_t SDL_GetTicksNS(void) { static uint64_t t; return ++t; }
static bool OSDisableInterrupts(void) { return true; }
static void OSRestoreInterrupts(bool enabled) { assert(enabled); }
''' + restore + r'''
int main(void) {
    unsigned char live[4] = {9, 9, 9, 9};
    unsigned char saved[4] = {1, 2, 3, 4};
    seed_a = 0x11111111;
    seed_b = 0x22222222;
    Snapshot snap = {
        .buf = saved,
        .regions = {{live, sizeof live}},
        .nregions = 1,
        .seed_ptr = &seed_a,
        .seed_val = 0xabcdef01,
    };
    snapshot_restore(&snap);
    assert(memcmp(live, saved, sizeof live) == 0);
    assert(HSD_RandSeedPtr == &seed_a);
    assert(seed_a == 0xabcdef01);
    assert(seed_b == 0x22222222);
    return 0;
}
'''
        result = self.compile_and_run(body)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_synctest_ignores_only_live_pad_queue_bookkeeping(self):
        source = SOURCE.read_text()
        start = source.index("typedef struct SynctestIgnoredSpan")
        end = source.index("/* Hash of the same regions", start)
        helpers = source[start:end]
        body = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
typedef struct { unsigned char bytes[17]; } HSD_PadStatus;
typedef struct { unsigned char bytes[8]; } RumbleInfo;
typedef struct { unsigned char bytes[64]; } HSD_PadData;
typedef struct {
    unsigned char qnum, qread, qwrite, qcount, qtype;
    HSD_PadData* queue;
    unsigned char config[19];
    RumbleInfo rumble_info;
} PadLibData;
static HSD_PadStatus HSD_PadMasterStatus[4];
static HSD_PadStatus HSD_PadGameStatus[4];
static HSD_PadStatus HSD_PadCopyStatus[4];
static PadLibData HSD_PadLibData;
''' + helpers + r'''
int main(void) {
    unsigned char saved[sizeof HSD_PadMasterStatus];
    memset(saved, 0, sizeof saved);
    memset(HSD_PadMasterStatus, 0, sizeof HSD_PadMasterStatus);
    HSD_PadMasterStatus[2].bytes[4] = 1;
    assert(synctest_memcmp(HSD_PadMasterStatus, saved, sizeof saved) != 0);

    HSD_PadData queue[2] = {0};
    unsigned char queue_saved[sizeof queue] = {0};
    HSD_PadLibData.qnum = 2;
    HSD_PadLibData.queue = queue;
    HSD_PadLibData.qread = 1;
    assert(synctest_memcmp(&HSD_PadLibData.qread, "\0", 1) == 0);
    queue[1].bytes[3] = 1;
    assert(synctest_memcmp(queue, queue_saved, sizeof queue) == 0);

    unsigned char rumble_saved[sizeof HSD_PadLibData.rumble_info] = {0};
    HSD_PadLibData.rumble_info.bytes[2] = 1;
    assert(synctest_memcmp(&HSD_PadLibData.rumble_info, rumble_saved,
                          sizeof rumble_saved) != 0);

    unsigned char gameplay[16] = {0};
    unsigned char expected[16] = {0};
    gameplay[7] = 1;
    assert(synctest_memcmp(gameplay, expected, sizeof gameplay) != 0);
    return 0;
}
'''
        result = self.compile_and_run(body)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_resimulation_advances_rumble_state_without_touching_motor(self):
        harness = (ROOT / "tools/test_net_resume.c").read_text()
        harness = harness.replace("int main(int argc, char** argv) {", "int resume_main(int argc, char** argv) {")
        old_motor = """void PADControlMotor(u32 chan, u32 cmd) {
    (void)chan;
    (void)cmd;
}"""
        new_motor = """static int motor_calls;
void PADControlMotor(u32 chan, u32 cmd) {
    (void)chan;
    (void)cmd;
    motor_calls++;
}"""
        self.assertEqual(harness.count(old_motor), 1)
        harness = harness.replace(old_motor, new_motor)
        harness = harness.replace('"../src/pc/net.c"', f'"{ROOT / "src/pc/net.c"}"')
        harness = harness.replace('"../src/pc/net_wire.c"', f'"{ROOT / "src/pc/net_wire.c"}"')
        harness = harness.replace('"../src/sysdolphin/baselib/rumble.c"',
                                  f'"{ROOT / "src/sysdolphin/baselib/rumble.c"}"')
        main = r'''
int main(void) {
    memset(&HSD_PadLibData, 0, sizeof HSD_PadLibData);
    memset(HSD_Rumble_804C22E0, 0, sizeof HSD_Rumble_804C22E0);
    net.resim = false;
    HSD_PadRumbleInterpret(); /* establish physical neutral state */
    motor_calls = 0;
    HSD_Rumble_804C22E0[0].direct_status = 1;
    net.resim = true;
    HSD_PadRumbleInterpret();
    assert(motor_calls == 0);
    assert(HSD_Rumble_804C22E0[0].status == 1);
    assert(HSD_Rumble_804C22E0[0].last_status == 1);
    net.resim = false;
    HSD_PadRumbleInterpret(); /* same logical state still must reach hardware */
    assert(motor_calls == 1);
    net.resim = true;
    HSD_Rumble_804C22E0[0].direct_status = 0;
    HSD_PadRumbleInterpret();
    assert(motor_calls == 1);
    net.resim = false;
    HSD_PadRumbleInterpret(); /* corrected stop is emitted exactly once */
    assert(motor_calls == 2);
    HSD_PadRumbleInterpret();
    assert(motor_calls == 2);
    assert(HSD_Rumble_804C22E0[0].last_status == 0);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="net-rumble-") as work:
            source = Path(work) / "test.c"
            source.write_text(harness + main)
            exe = Path(work) / "test"
            includes = [ROOT / "extern/aurora/include", ROOT / "src", ROOT / "src/sdk_include",
                        ROOT / "extern/monocypher", *sdl_includes(ROOT)]
            subprocess.run(["cc", "-std=gnu11", "-DTARGET_PC=1", "-DMELEE_PC=1", "-DAURORA",
                            f"-I{ROOT / 'src/pc'}", *[f"-I{p}" for p in includes], str(source),
                            str(ROOT / "extern/monocypher/monocypher.c"),  # net_wire.c keys the MAC
                            "-o", str(exe)], check=True, capture_output=True)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_reserve_pages_in_one_slot_per_call_and_keeps_contents(self):
        source = SOURCE.read_text()
        start = source.index("void snaps_reserve(void) {")
        reserve = source[start:source.index("/* Session end", start)]
        body = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define SNAPS 8
#define MAX_REGIONS 12
typedef struct { const char* name; void* ptr; size_t len; } Region;
typedef struct { int32_t frame; uint8_t* buf; size_t cap, used, faulted; } Snapshot;
static Snapshot s_snaps[SNAPS];
static size_t live = 1000; /* bytes a take would copy right now */
static int regions_now(Region* r) {
    r[0] = (Region){"data", NULL, live / 2};
    r[1] = (Region){"heap0", NULL, live - live / 2};
    return 2;
}
''' + reserve + r'''
int main(void) {
    /* A slot already holding a snapshot must keep it through the growth. */
    s_snaps[3].buf = malloc(100);
    memset(s_snaps[3].buf, 0x5a, 100);
    s_snaps[3].cap = s_snaps[3].used = s_snaps[3].faulted = 100;
    s_snaps[3].frame = 42;
    for (int call = 1; call <= SNAPS; call++) {
        snaps_reserve();
        int done = 0;
        for (int i = 0; i < SNAPS; i++) {
            done += s_snaps[i].faulted == live;
        }
        assert(done == call); /* exactly one more slot per call */
    }
    for (int i = 0; i < SNAPS; i++) {
        assert(s_snaps[i].cap >= live && s_snaps[i].cap == live * 3 / 2);
    }
    for (int i = 0; i < 100; i++) {
        assert(s_snaps[3].buf[i] == 0x5a);
    }
    assert(s_snaps[3].frame == 42 && s_snaps[3].used == 100);
    /* Sized: a no-op, however often it runs. */
    uint8_t* was[SNAPS];
    for (int i = 0; i < SNAPS; i++) {
        was[i] = s_snaps[i].buf;
        memset(s_snaps[i].buf, 0x77, live);
    }
    for (int k = 0; k < 20; k++) {
        snaps_reserve();
    }
    for (int i = 0; i < SNAPS; i++) {
        assert(s_snaps[i].buf == was[i] && s_snaps[i].buf[live - 1] == 0x77);
    }
    /* Growth inside the headroom writes only past what was paged in. */
    live = 1200;
    snaps_reserve();
    assert(s_snaps[0].faulted == 1200 && s_snaps[0].buf == was[0]);
    assert(s_snaps[0].buf[999] == 0x77 && s_snaps[0].buf[1000] == 0);
    assert(s_snaps[1].faulted == 1000);
    return 0;
}
'''
        result = self.compile_and_run(body)
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
