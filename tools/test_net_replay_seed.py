#!/usr/bin/env python3
"""Exercise shipping record/replay code and inject a missing seed restore.

No game is launched: only the production record/replay section is compiled,
with the real PADStatus definition and logging/net-state stubs.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest
import struct

import net_test
import net_determinism

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src/pc/net_snapshot.c"

PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <dolphin/pad.h>
static uint32_t seed_value;
static uint32_t* HSD_RandSeedPtr = &seed_value;
static struct { bool active; int frame; } net;
/* The recorder stages a frame per ring slot so a rollback can replace it
 * before it is written; mirror net_internal.h's ring size here. */
#define RING 64
/* net.c owns the agreed scene-exit frame; this harness has no session, so
 * every record it writes is outside a hand-off. */
static int32_t net_scene_exit_at(void) { return -1; }
static char messages[4096];
static void pc_log_line(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(messages + strlen(messages), sizeof messages - strlen(messages), fmt, args);
    va_end(args);
}
'''
MAIN = r'''
int main(int argc, char** argv) {
    assert(argc == 2 && sizeof(PADStatus) == 16);
    PADStatus pads[4] = {0};
    pads[0].button = 0x1234;
    setenv("MELEE_NET_RECORD", argv[1], 1);
    record_open();
    seed_value = 7;
    replay_feed(pads);
    /* The agreed seed can be assigned after replay_feed but before checksum. */
    seed_value = 11;
    record_frame(pads, 0x11111111, net.frame);
    net.frame = 1;
    seed_value = 23;
    replay_feed(pads);
    record_frame(pads, 0x22222222, net.frame);
    fclose(s_rec);
    s_rec = NULL;
    unsetenv("MELEE_NET_RECORD");
    FILE* f = fopen(argv[1], "rb");
    unsigned char bytes[160]; /* 8-byte header + two 76-byte records */
    assert(fread(bytes, 1, sizeof bytes, f) == sizeof bytes);
    assert(fgetc(f) == EOF && memcmp(bytes, "MRC3", 4) == 0);
    uint32_t word;
    int32_t at;
    memcpy(&word, bytes + 8 + 64, 4); assert(word == 0x11111111);
    memcpy(&word, bytes + 8 + 68, 4); assert(word == 11);
    memcpy(&at, bytes + 8 + 72, 4); assert(at == -1); /* no hand-off pending */
    memcpy(&word, bytes + 8 + 76 + 68, 4); assert(word == 23);
    fclose(f);
    setenv("MELEE_NET_REPLAY", argv[1], 1);
    record_open();
    assert(s_rep != NULL);
    seed_value = 0xdeadbeef; /* Inject out-of-tick draws before the first tick. */
    net.frame = 0;
    memset(pads, 0, sizeof pads);
    replay_feed(pads);
    assert(seed_value == 11 && pads[0].button == 0x1234);
    record_frame(pads, 0x11111111, net.frame);
    assert(!s_rep_reported);
    seed_value = 0xcafebabe; /* A different load-time seed before the next tick. */
    net.frame = 1;
    replay_feed(pads);
    assert(seed_value == 23);
    record_frame(pads, 0x22222222, net.frame);
    assert(!s_rep_reported);
    record_frame(pads, 0x22222223, net.frame); /* Seed restoration must not hide bad state. */
    assert(s_rep_reported && strstr(messages, "REPLAY DIVERGED at frame 1"));
    replay_feed(pads);
    assert(s_rep == NULL);
    f = fopen(argv[1], "r+b");
    assert(fwrite("MRC1", 4, 1, f) == 1);
    fclose(f);
    record_open();
    assert(s_rep == NULL); /* Game rejects the old seedless format. */
    return 0;
}
'''


class ReplaySeedTest(unittest.TestCase):
    def compile_and_run(self, section):
        with tempfile.TemporaryDirectory() as work:
            source = Path(work) / "replay.c"
            source.write_text(PRELUDE + section + MAIN)
            exe = Path(work) / "replay"
            subprocess.run(["cc", "-std=gnu11", "-DTARGET_PC=1", "-I",
                            str(ROOT / "extern/aurora/include"), str(source), "-o", str(exe)],
                           check=True, capture_output=True)
            return subprocess.run([str(exe), str(Path(work) / "test.rec")],
                                  capture_output=True, text=True)

    def test_seed_round_trip_and_injected_missing_restore(self):
        source = SOURCE.read_text()
        section = source[source.index("static FILE* s_rec;"):
                         source.index("/* ---- frame checksum")]
        good = self.compile_and_run(section)
        self.assertEqual(good.returncode, 0, good.stderr)
        restore = "*HSD_RandSeedPtr = s_rep_cur.seed;"
        self.assertEqual(section.count(restore), 1)
        broken = self.compile_and_run(section.replace(restore, "/* injected missing restore */"))
        self.assertNotEqual(broken.returncode, 0)
        self.assertIn("seed_value == 11", broken.stderr)

    def test_checksum_readers_preserve_old_captures_and_read_new_seed(self):
        for magic, stride in ((b"MRC1", 68), (b"MRC2", 72), (b"MRC3", 76)):
            with self.subTest(magic=magic), tempfile.TemporaryDirectory() as work:
                header = magic + struct.pack("<I", 7)
                def row(ck, stride=stride):
                    tail = b""
                    if stride >= 72:
                        tail += struct.pack("<I", 0xdeadbeef)  # tick-start seed
                    if stride >= 76:
                        tail += struct.pack("<i", -1)  # agreed scene exit
                    return bytes(64) + struct.pack("<I", ck) + tail
                path = Path(work) / "test.rec"
                path.write_bytes(header + row(99) + header + row(11) + row(22))
                expected = [struct.pack("<I", 11), struct.pack("<I", 22)]
                self.assertEqual(net_test.record_cks(path), expected)
                self.assertEqual(net_test.record_cks(path, tail=1), expected[-1:])
                self.assertEqual(net_test.record_cks(path, tail=10), expected)
        data = b"MRC3" + struct.pack("<I", 7) + bytes(64) + struct.pack("<IIi", 11, 23, -1)
        self.assertEqual(net_determinism.rec_frames(data), 1)
        self.assertEqual(net_determinism.rec_ck(data, 0), 11)


if __name__ == "__main__":
    unittest.main()
