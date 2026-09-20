#!/usr/bin/env python3
"""Bounded scene-gate injection; no game launch or production edits.

Compile actual net.c via the existing resume harness's dependency stubs.
Extract the shipping scene gate and tick dispatch block verbatim, substituting
only the simulation body with a pad-consuming callback counter. This tests
dispatch timing, not simulation fidelity. Normal checks cover the held boundary, the first exit request, and an ordinary
advance. One mutated guard restores the known-positive boundary failure.

Run from any directory with Python 3 and cc. The reused fixture binds an
ephemeral loopback socket, so it needs the same network permission as
test_net_resume.c. Its lb_80019A30 stub returns false, causing an expected
"did not advance the sim" diagnostic; the explicit simulation counter below
is the timing observation. Production sources remain unchanged by mutation.
"""
from pathlib import Path
from net_test_support import sdl_includes
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
scene = (ROOT / "src/melee/gm/gmscene.c").read_text()
gate_start = scene.index("static bool scene_end_gate(")
gate_end = scene.index("\n#endif", gate_start)
dispatch_start = scene.index("            void (*frame_fn)(void)")
dispatch_end = scene.index("\n#else", dispatch_start)
gate = scene[gate_start:gate_end]
dispatch = scene[dispatch_start:dispatch_end]

source = r'''
#define main resume_harness_main
#include "HARNESS"
#undef main

static int s_scene_end_held;
GATE
static int callback_count, sim_count, request_exit;
static struct gm_80479D58_t scene_state;
static void count_frame(void) {
    callback_count++;
    if (request_exit) scene_state.unk_C = 1;
}
static bool gm_RunSimTick(void (*on_frame)(void), struct gm_80479D58_t* st) {
    PadLibData* p = &HSD_PadLibData;
    PADStatus* head = &p->queue->stat[p->qread * 4];
    for (int i = 0; i < 4; i++) HSD_PadMasterStatus[i].button = head[i].button;
    p->qread = (p->qread + 1) % p->qnum;
    p->qcount--;
    sim_count++;
    if (on_frame) on_frame();
    return st->unk_C != 0;
}
static void shipping_dispatch(void (*on_frame)(void)) {
    struct gm_80479D58_t* temp_r25 = &scene_state;
    for (int i = 0; i < 1; i++) {
DISPATCH
    }
}
static int run_case(int advance, int mode) {
    fight_setup();
    s_test_scene.scene_kind = GS_SSS;
    s_scene_last = GS_SSS;
    net.frame = 199;
    net.tick_frame = 198;
    net.advance_left = advance;
    net.sync_mode = SYNC_ON;
    s_remote_have = 210;
    s_remote_newest = 210;
    net.hs = HS_DONE;
    net.start_frame = 120;
    s_wrote = 201;
    callback_count = sim_count = 0;
    request_exit = mode == 1;
    memset(&scene_state, 0, sizeof scene_state);
    /* Both peers already asked at 180, agreeing exit boundary 200. */
    s_scene_exit_local = mode == 0 ? 180 : -1;
    SceneMsg peer = {htonl(0), htonl(180)};
    net_scene_rel(&peer, sizeof peer);
    s_scene_end_held = mode == 0;
    shipping_dispatch(count_frame);
    if (mode == 0) {
        assert(scene_state.unk_C == 1 && s_scene_seq == 1);
        assert(callback_count == 0);
        assert(net.frame == 200 + (EXPECT_OLD && advance));
    } else if (mode == 1) {
        assert(scene_state.unk_C == 0 && s_scene_end_held == 1);
        assert(callback_count == 1 + (EXPECT_OLD && advance));
        assert(s_scene_exit_local == 200 + (EXPECT_OLD && advance));
    } else {
        assert(callback_count == 2 && net.frame == 201);
    }
    assert(s_pad_slips == 0);
    printf("observed: variant=%s mode=%s pending_advance=%d boundary=%d old_scene_ticks=%d callbacks=%d\n",
           EXPECT_OLD ? "mutated" : "fixed",
           mode == 0 ? "held" : mode == 1 ? "first-request" : "running",
           advance, net.frame, sim_count, callback_count);
    int result = net.frame;
    pc_net_disconnect();
    return result;
}
static void run_ending_rollback(void) {
    fight_setup();
    net.hs = HS_DONE;
    net.start_frame = 120;
    net.advance_left = 1;
    s_remote_have = s_remote_newest = 210;
    HSD_PadLibData.qcount = 0; /* live tick 199 has already consumed its pad */
    s_head_frame = -1;
    assert(snapshot_take(snap_slot(198), 198));
    s_rb_frame = 198;
    sim_count = callback_count = 0;
    assert(pc_net_after_tick(true));
    assert(net.resim && net.tick_frame == 198 && s_restored == 198);
    gm_RunSimTick(NULL, &scene_state);
    assert(pc_net_after_tick(true));
    assert(net.resim && net.tick_frame == 199);
    gm_RunSimTick(NULL, &scene_state);
    assert(!pc_net_after_tick(true));
    assert(!net.resim && net.frame == 200 && net.advance_left == 1);
    assert(s_seed_have && s_seed_after_tick == *HSD_RandSeedPtr);
    assert(sim_count == 2 && callback_count == 0);
    printf("regression: ending scene finishes rollback 198..199, arms seed bookkeeping, defers advance\n");
    pc_net_disconnect();
}
int main(void) {
    int control = run_case(0, 0);
    int injected = run_case(1, 0);
    run_case(1, 1);
    run_case(1, 2);
    if (EXPECT_OLD) {
        assert(injected - control == 1);
        printf("known-positive: 1; exit_boundary_diff=%+d\n", injected - control);
    } else {
        assert(control == injected);
        printf("regression: held boundary and first request protected; ordinary advance retained\n");
        run_ending_rollback();
    }
    return 0;
}
'''.replace("HARNESS", str(ROOT / "tools/test_net_resume.c")).replace("GATE", gate).replace("DISPATCH", dispatch)

includes = [ROOT / "extern/aurora/include", ROOT / "src", ROOT / "src/sdk_include",
            *sdl_includes(ROOT)]
with tempfile.TemporaryDirectory(prefix="net_scene_timing_") as work:
    work = Path(work)
    harness = (ROOT / "tools/test_net_resume.c").read_text()
    # Extend only the reused transport stub; resume tests still own their file.
    resume_assert = "    assert(type == REL_RESUME);"
    assert harness.count(resume_assert) == 1
    harness = harness.replace(resume_assert,
                              "    if (type == REL_SCENE) {\n"
                              "        assert(len == (int)sizeof(SceneMsg));\n"
                              "        return true;\n"
                              "    }\n" + resume_assert)
    net_source = (ROOT / "src/pc/net.c").read_text()
    guard = "!scene_ending && net.advance_left > 0"
    assert net_source.count(guard) == 1, "production advance guard changed; review mutation"
    for mutated in (False, True):
        variant = "mutated" if mutated else "fixed"
        net_copy = work / f"net_{variant}.c"
        net_copy.write_text(net_source.replace(guard, "net.advance_left > 0") if mutated else net_source)
        harness_copy = work / f"harness_{variant}.c"
        harness_copy.write_text(harness.replace('"../src/pc/net.c"', f'"{net_copy}"')
                               .replace('"../src/pc/net_wire.c"', f'"{ROOT / "src/pc/net_wire.c"}"')
                               .replace('"../src/sysdolphin/baselib/rumble.c"',
                                        f'"{ROOT / "src/sysdolphin/baselib/rumble.c"}"'))
        c = work / f"scene_timing_{variant}.c"
        binary = work / f"scene_timing_{variant}"
        c.write_text(source.replace(str(ROOT / "tools/test_net_resume.c"), str(harness_copy)))
        subprocess.run(["cc", "-std=gnu11", "-DTARGET_PC=1", "-DMELEE_PC=1", "-DAURORA",
                        f"-DEXPECT_OLD={int(mutated)}", f"-I{ROOT / 'src/pc'}",
                        *[f"-I{p}" for p in includes], str(c), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=10)
