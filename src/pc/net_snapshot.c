/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Netplay state capture: whole-region snapshots for rollback, the per-frame
 * checksum the peers compare, the state ring dumped on a DESYNC, the
 * MELEE_NET_SYNCTEST self-check and MELEE_NET_RECORD/REPLAY. Game thread
 * only (net_internal.h). */
#include "compat.h"
#include "pc/net_internal.h"
#include "pc/slp.h"

#include <dolphin/ar.h>
#include <dolphin/dvd.h>
#include <dolphin/os.h>
#include <sysdolphin/baselib/random.h>
#include <sysdolphin/baselib/synth.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wscalar-storage-order" /* disc-struct unions in lb/types.h */
#include <melee/ft/fighter.h>
#include <melee/ft/inlines.h>
#include <melee/pl/player.h>
#pragma GCC diagnostic pop
#include <xxhash.h>

#include <SDL3/SDL_timer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- record / replay --------------------------------------------------
 * MELEE_NET_RECORD=file  writes the seed, then per frame the four PADStatus
 *                        actually simulated, checksum and tick-start seed.
 * MELEE_NET_REPLAY=file  feeds those pads back in and reports the first
 *                        frame whose checksum differs: the determinism test
 *                        for M0 (docs/netcode-plan.md §5). Works solo or
 *                        together with netplay (record only).
 * MELEE_NET_STATE_LOG=file
 *                        writes record_state()'s line plus an exact-bits
 *                        line for EVERY frame to `file`, not just into the
 *                        DESYNC ring. A checksum says two platforms differ;
 *                        these say which field of which fighter differs,
 *                        which is the only way to localise a cross-platform
 *                        divergence (two files, one diff).
 *                        Its own file, block-buffered and flushed every 8
 *                        frames, deliberately NOT pc_log_line: that flushes
 *                        stderr and the log file on every call, and two of
 *                        those per frame slowed the loop enough to change
 *                        which frame the pad alarm fires on - the instrument
 *                        moved what it was measuring (the same replay went
 *                        from "identical over 1376 frames" to "diverges at
 *                        1359"). Off by default. */
static FILE* s_rec;
static int32_t s_confirmed = INT32_MIN; /* newest final netplay frame, for slp.c */
static FILE* s_rep;
static bool s_rep_reported;
static FILE* s_state_log;

typedef struct FrameRecord {
    PADStatus pads[4];
    uint32_t ck;
    uint32_t seed;
    /* The scene this frame was simulated in (scene_kind(), -1 before any).
     *
     * A netplay scene does not end when its own code asks to: both peers
     * hold it to a frame they agree on (net.c pc_net_scene_hold). An earlier
     * cut of MRC3 stored that agreed FRAME, which cannot work: gmscene.c's
     * scene_end_gate() only consults the hold on the frame the scene ASKS to
     * end, and that frame is a property of how long the machine took to
     * load -- exactly what differs between a session and a replay of it, so
     * the replay asks on a frame the recording has nothing for and leaves.
     * Storing the scene instead removes the timing from the question: the
     * replay ends a scene when the RECORDING's scene changed, whenever its
     * own code happens to ask. */
    int32_t scene;
} FrameRecord;

static FrameRecord s_rep_next; /* one record of lookahead: see the scene hold */
static bool s_rep_next_ok;

void record_open(void) {
    const char* rec = getenv("MELEE_NET_RECORD");
    const char* rep = getenv("MELEE_NET_REPLAY");
    if (rec != NULL && rec[0] != '\0') {
        s_rec = fopen(rec, "wb");
        pc_log_line("net: %s %s", s_rec ? "recording to" : "cannot open", rec);
    }
    if (rep != NULL && rep[0] != '\0' && !net.active) {
        s_rep = fopen(rep, "rb");
        char magic[4];
        uint32_t seed;
        bool ok = s_rep && fread(magic, 4, 1, s_rep) == 1;
        if (ok && memcmp(magic, "MRC4", 4) == 0) {
            /* The same layout with the older checksum, which folded no
             * velocity: replayed now, every fight frame reads as a
             * divergence that is not one. */
            pc_log_line("net: %s is an MRC4 recording; its checksums predate protocol 9, "
                        "record it again",
                rep);
            ok = false;
        }
        ok = ok && memcmp(magic, "MRC5", 4) == 0 && fread(&seed, 4, 1, s_rep) == 1;
        if (s_rep && !ok) {
            fclose(s_rep);
            s_rep = NULL;
        }
        if (s_rep) {
            *HSD_RandSeedPtr = seed;
            s_rep_next_ok = fread(&s_rep_next, sizeof s_rep_next, 1, s_rep) == 1;
        }
        pc_log_line("net: %s %s", s_rep ? "replaying" : "cannot open", rep);
    }
    const char* sl = getenv("MELEE_NET_STATE_LOG");
    if (sl != NULL && sl[0] != '\0') {
        s_state_log = fopen(sl, "w");
        if (s_state_log != NULL) {
            static char buf[1 << 16];
            setvbuf(s_state_log, buf, _IOFBF, sizeof buf);
        }
        pc_log_line("net: %s state log %s", s_state_log ? "writing" : "cannot open", sl);
    }
}

bool record_active(void) {
    return s_rec != NULL || s_rep != NULL;
}

static FrameRecord s_rep_cur;

/* Load the next record's pads for this frame; false at end of file. The file
 * is read one record ahead so the scene hold can ask what the recording did
 * on the frame AFTER this one, which is how it knows a scene ended. */
static bool replay_load(PADStatus* head) {
    if (!s_rep_next_ok) {
        return false;
    }
    s_rep_cur = s_rep_next;
    s_rep_next_ok = fread(&s_rep_next, sizeof s_rep_next, 1, s_rep) == 1;
    memcpy(head, s_rep_cur.pads, sizeof s_rep_cur.pads);
    *HSD_RandSeedPtr = s_rep_cur.seed;
    return true;
}

static void replay_compare(uint32_t ck) {
    if (!s_rep_reported && s_rep_cur.ck != ck) {
        s_rep_reported = true;
        pc_log_line("net: REPLAY DIVERGED at frame %d (recorded %08x now %08x)", net.frame,
            s_rep_cur.ck, ck);
    }
}

/* Before the fresh tick of net.frame: the replay's pads into the head. */
void replay_feed(PADStatus* head) {
    if (s_rep != NULL && !replay_load(head)) {
        pc_log_line("net: replay finished at frame %d%s", net.frame,
            s_rep_reported ? "" : ", no divergence");
        fclose(s_rep);
        s_rep = NULL;
    }
}

/* Still at tick start, after the checksum and agreed-seed override but before
 * simulation: record the same seed the checksum used, not a prior tick's or
 * a pre-handshake seed. The intervening state/desync logging draws no RNG.
 *
 * During netplay a frame is simulated more than once: the first pass runs on
 * a PREDICTED remote pad and a rollback re-runs it on the real one. Writing
 * the first pass is what the file used to hold, and replaying it then
 * diverges at the first rollback -- which reads exactly like the
 * cross-platform determinism failure the recording exists to localise. So a
 * frame is staged here (a later re-run overwrites its slot) and only written
 * out once it can no longer be rolled back, which is what record_confirm()
 * below does. Offline there is no rollback and staging is a straight
 * write-through. */
static FrameRecord s_rec_ring[RING];
static int32_t s_rec_staged = -1;  /* newest frame staged */
static int32_t s_rec_written = -1; /* newest frame on disk */

static void record_write(const FrameRecord* r, int32_t f) {
    if (f == 0) {
        fwrite("MRC5", 4, 1, s_rec);
        fwrite(&r->seed, 4, 1, s_rec);
    }
    fwrite(r, sizeof *r, 1, s_rec);
    fflush(s_rec); /* runs usually end by SIGTERM; keep every frame */
}

void record_frame(const PADStatus* head, uint32_t ck, int32_t f) {
    if (s_rec != NULL) {
        FrameRecord* r = &s_rec_ring[f & (RING - 1)];
        memcpy(r->pads, head, sizeof r->pads);
        r->ck = ck;
        r->seed = *HSD_RandSeedPtr;
        r->scene = scene_kind();
        if (f > s_rec_staged) {
            s_rec_staged = f;
        }
        if (!net.active) {
            record_write(r, f);
            s_rec_written = f;
        }
    }
    if (s_rep != NULL) {
        replay_compare(ck);
    }
}

/* Frames up to and including `upto` are settled: write them out in order.
 * A frame that aged out of the ring before it was confirmed cannot be
 * recovered, so the file stops there rather than silently skipping it. */
void record_confirm(int32_t upto) {
    s_confirmed = net.active ? upto : INT32_MIN;
    if (s_rec == NULL || !net.active) {
        return;
    }
    if (upto > s_rec_staged) {
        upto = s_rec_staged;
    }
    for (int32_t f = s_rec_written + 1; f <= upto; f++) {
        if (f <= net.frame - RING) {
            return; /* its slot has been reused: the record ends here */
        }
        record_write(&s_rec_ring[f & (RING - 1)], f);
        s_rec_written = f;
    }
}

/* Offline replay of a netplay recording: end a scene where the RECORDING
 * ended it. The gate asks "may this scene end now?" on whatever frame this
 * run's code gets round to asking, which is not the frame the recorded
 * session asked on -- it depends on how long each machine took to load. So
 * the answer cannot come from a frame number. It comes from the record one
 * frame ahead: while the recording was still in the same scene on the next
 * frame, this scene has not ended yet.
 *
 * Logged once per hand-off. Between two menus the checksum covers only the
 * pads and the RNG, so a scene left early still hashes equal for a while and
 * the DIVERGED line lands frames later, on a different boundary than the one
 * that slipped; the log is what makes the real boundary visible. */
bool record_replay_scene_hold(int32_t frame) {
    if (s_rep == NULL || !s_rep_next_ok || s_rep_cur.scene < 0) {
        return false;
    }
    bool hold = s_rep_next.scene == s_rep_cur.scene;
    static int32_t logged = -1;
    if (!hold && frame != logged) {
        logged = frame;
        pc_log_line("net: replay ends scene %d at frame %d (recording moves to %d)",
            s_rep_cur.scene, frame, s_rep_next.scene);
    }
    return hold;
}

/* ---- frame checksum --------------------------------------------------- */

/* For the .slp recorder (slp.c), which runs inside the tick. The frame the
 * tick simulates: a rollback's re-run frame, otherwise the newest fresh one
 * (net.frame counts every tick offline too, where tick_frame is not kept). */
int32_t pc_net_sim_frame(void) {
    return net.resim && net.active ? net.tick_frame : net.frame - 1;
}

/* The newest netplay frame no rollback can change, as record_confirm last
 * heard it from fresh_tick; INT32_MIN offline. */
int32_t pc_net_confirmed_frame(void) {
    return s_confirmed;
}

uint32_t frame_checksum(const PADStatus* head) {
    /* Inputs as simulated, the RNG seed entering the frame, and each
     * fighter's position, facing, percent, stocks and action state (plan
     * §4), so a physics divergence is caught on the frame it happens. The
     * two velocities went in with protocol 9: a divergence that starts in a
     * velocity reaches the position only a frame or more later, and one
     * that is cancelled (a landing, a wall) never reaches it at all. */
    uint32_t ck = fnv1a(2166136261u, head, 4 * sizeof(PADStatus));
    ck = fnv1a(ck, HSD_RandSeedPtr, sizeof(u32));
    /* Player slots keep dangling entity pointers between scenes, so only
     * look while a fight is running and the entity really is a fighter. */
    for (int slot = 0; in_fight() && slot < 4; slot++) {
        HSD_GObj* gobj = Player_GetEntity(slot);
        if (gobj == NULL || gobj->classifier != HSD_GOBJ_CLASS_FIGHTER) {
            continue;
        }
        const Fighter* fp = GET_FIGHTER(gobj);
        s32 stocks = Player_GetStocks(slot);
        ck = fnv1a(ck, &fp->cur_pos, sizeof fp->cur_pos);
        ck = fnv1a(ck, &fp->facing_dir, sizeof fp->facing_dir);
        ck = fnv1a(ck, &fp->dmg.x1830_percent, sizeof fp->dmg.x1830_percent);
        ck = fnv1a(ck, &fp->motion_id, sizeof fp->motion_id);
        ck = fnv1a(ck, &stocks, sizeof stocks);
        ck = fnv1a(ck, &fp->self_vel, sizeof fp->self_vel);
        ck = fnv1a(ck, &fp->x8c_kb_vel, sizeof fp->x8c_kb_vel);
    }
    return ck;
}

/* MELEE_NET_RENDER_AUDIT=1: the render must not write what the simulation
 * reads. A rollback resim and a time-sync advance run ticks with no render
 * between them, so anything a draw callback writes into sim state is written
 * on one peer's timeline and not on the other's -- the screen-KO position
 * (ftDrawCommon_80080E18) was one. This takes frame_checksum's fighter
 * fields and the seed before the render and again after, and names the
 * frame whenever they differ. Pads are left out: the render does not read
 * them and a zeroed set keeps both sides comparable. Debug only; one line
 * per 60 hits so a callback that writes every frame cannot flood the log. */
void pc_net_render_audit(bool after) {
    static int on = -1;
    static uint32_t before;
    static unsigned hits;
    if (on < 0) {
        const char* e = getenv("MELEE_NET_RENDER_AUDIT");
        on = e != NULL && e[0] == '1';
    }
    if (!on) {
        return;
    }
    static const PADStatus none[4];
    uint32_t ck = frame_checksum(none);
    if (!after) {
        before = ck;
    } else if (ck != before && (hits++ % 60) == 0) {
        pc_log_line("net: render audit: sim state changed during render at frame %d (%u so far)",
            pc_net_frame(), hits);
    }
}

/* What went into the checksum, one line, so two peers' logs can be diffed
 * by eye when a DESYNC is reported. */
#define STATE_RING 64
static char s_state_ring[STATE_RING][320];

/* Raw bits of a float: the human line below rounds to three decimals, which
 * hides exactly the 1-ULP differences a codegen or libm divergence starts
 * as, and every one of those fields is folded into the checksum verbatim. */
static uint32_t f32bits(float f) {
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    return u;
}

/* Everything frame_checksum() folds, exactly, one line per frame. This is
 * what two platforms' logs are diffed on to name the first field that
 * differs (MELEE_NET_STATE_LOG). */
static void log_state_bits(int32_t frame) {
    char buf[1024]; /* four fighters of at most 152 characters each */
    int n = snprintf(buf, sizeof buf, "f%d seed=%08x", frame, *HSD_RandSeedPtr);
    for (int slot = 0; in_fight() && slot < 4 && n < (int)sizeof buf - 160; slot++) {
        HSD_GObj* gobj = Player_GetEntity(slot);
        if (gobj == NULL || gobj->classifier != HSD_GOBJ_CLASS_FIGHTER) {
            continue;
        }
        const Fighter* fp = GET_FIGHTER(gobj);
        n += snprintf(buf + n, sizeof buf - (size_t)n,
            " p%d pos=%08x/%08x/%08x dir=%08x pct=%08x mid=%d st=%d vel=%08x/%08x/%08x "
            "kb=%08x/%08x/%08x",
            slot, f32bits(fp->cur_pos.x), f32bits(fp->cur_pos.y), f32bits(fp->cur_pos.z),
            f32bits(fp->facing_dir), f32bits(fp->dmg.x1830_percent), fp->motion_id,
            Player_GetStocks(slot), f32bits(fp->self_vel.x), f32bits(fp->self_vel.y),
            f32bits(fp->self_vel.z), f32bits(fp->x8c_kb_vel.x), f32bits(fp->x8c_kb_vel.y),
            f32bits(fp->x8c_kb_vel.z));
    }
    fprintf(s_state_log, "net: bits %s\n", buf);
}

/* ---- periodic full-state hash -----------------------------------------
 * frame_checksum() is a tripwire, not a desync detector: it folds the four
 * pads, the seed and five fields per fighter, so two timelines can part in a
 * velocity, a hitlag timer, an item, a camera or any heap byte and keep
 * agreeing for many frames afterwards. The frame a mismatch is finally
 * reported on is then not the frame the divergence happened on, which is
 * what dump_states_around() exists to work backwards from by hand.
 *
 * So the other half is the whole of what a snapshot copies -- by
 * construction everything the simulation can touch -- hashed periodically.
 * state_hash() is already that walk and the sync test rests on it; it also
 * drops the spans rollback_to carries across a restore
 * (synctest_ignored_spans), which is required here for a second reason:
 * those bytes are the local controller's raw queue, so they differ between
 * peers on every frame by design and a hash folding them could never be
 * compared at all. The audio heap and its descriptor are outside the
 * regions for the same kind of reason (regions_now).
 *
 * It costs what a snapshot costs, because it is the same bytes read instead
 * of copied: XXH3 over 6 MB measures 0.40 ms here against 0.33 ms for the
 * memcpy, next to the ~0.6 ms a take already reports. That is 3% of a frame
 * on the frame it runs, so it cannot run per frame; once every
 * STATE_HASH_EVERY frames it amortises to ~0.02% and still names the
 * divergence within seconds of it. The value is the confirmed one:
 * record_state is called again for every re-run frame, so the last write
 * for a frame is the timeline that stood.
 *
 * Nothing of this goes on the wire; protocol 7 is fixed. net_state_hash()
 * is what a peer comparison would read once it is not. */
#define STATE_HASH_EVERY 180      /* three seconds, in the cadence AUDIT_EVERY sets */
static uint64_t state_hash(void); /* defined with the sync test: the snapshot regions */

static uint64_t s_full_hash;
static int32_t s_full_hash_frame = -1;
static uint64_t s_full_hash_ns, s_full_hash_ns_max;
static unsigned s_full_hashes;

/* The newest full-state hash and the frame whose entry state it covers
 * (frame -1 until one is taken). Both peers hash the same bytes in the same
 * order, so two values carrying the same frame are comparable. */
uint64_t net_state_hash(int32_t* frame) {
    if (frame != NULL) {
        *frame = s_full_hash_frame;
    }
    return s_full_hash;
}

static void state_hash_periodic(int32_t frame) {
    /* Never during a load: an I/O worker writes into a heap while the game
     * thread runs, so the read would be torn and the peers would differ over
     * nothing. That is the window rollback already refuses (rb_barrier). It
     * is a local property, so the two peers can skip different frames --
     * which is why the frame is published next to the hash rather than
     * assumed. */
    if ((frame % STATE_HASH_EVERY) != 0 || frame <= net.rb_barrier || !in_fight()) {
        return;
    }
    /* rb_barrier is not enough on its own: it is raised only for a request the
     * GAME thread issued (pc_net_note_io), and a worker-issued transfer never
     * went through it. snapshot_take() refuses on exactly this condition; the
     * hash must too, or it reads a heap a DVD/ARQ worker is writing and the
     * peers differ over nothing. OSDisableInterrupts below only serialises the
     * game thread and the audio callback. */
    if (aurora_dvd_inflight() > 0 || aurora_arq_inflight() > 0) {
        return;
    }
    uint64_t t0 = SDL_GetTicksNS();
    /* Bracketed like snapshot_take's copy, and for its reason: a pad alarm
     * delivered in the middle of the read would write rumble state the walk
     * has already passed, which is a torn hash and a mismatch over nothing. */
    bool intr = OSDisableInterrupts();
    s_full_hash = state_hash();
    OSRestoreInterrupts(intr);
    s_full_hash_frame = frame;
    uint64_t dt = SDL_GetTicksNS() - t0;
    s_full_hash_ns += dt;
    s_full_hashes++;
    if (dt > s_full_hash_ns_max) {
        s_full_hash_ns_max = dt;
    }
    /* Into the state log too when it is on: that file is what two platforms'
     * runs are diffed on, and this is the line that says the divergence is
     * already in memory even when every checksum still agrees. */
    if (s_state_log != NULL) {
        fprintf(s_state_log, "net: fullhash f%d %016llx\n", frame, (unsigned long long)s_full_hash);
    }
}

/* Remember what went into this frame's checksum (overwritten when the
 * frame is re-simulated, so the last write is the confirmed timeline), and
 * log it too under MELEE_NET_STATE_LOG. Called for every frame, not only
 * the netplay ones: a solo replay is how a platform divergence is chased. */
void record_state(const PADStatus* head, int32_t frame) {
    if (!net.active && !s_state_log) {
        return;
    }
    char* buf = s_state_ring[frame & (STATE_RING - 1)];
    int n = snprintf(buf, sizeof s_state_ring[0], "f%d seed=%08x pads=%04x/%d,%d %04x/%d,%d", frame,
        *HSD_RandSeedPtr, head[0].button, head[0].stickX, head[0].stickY, head[1].button,
        head[1].stickX, head[1].stickY);
    for (int slot = 0; in_fight() && slot < 4 && n < (int)sizeof s_state_ring[0] - 80; slot++) {
        HSD_GObj* gobj = Player_GetEntity(slot);
        if (gobj == NULL || gobj->classifier != HSD_GOBJ_CLASS_FIGHTER) {
            continue;
        }
        const Fighter* fp = GET_FIGHTER(gobj);
        n += snprintf(buf + n, sizeof s_state_ring[0] - (size_t)n,
            " p%d=(%.3f,%.3f) v(%.3f,%.3f) kb(%.3f,%.3f) f%.0f %.1f%% m%d s%d", slot,
            (double)fp->cur_pos.x, (double)fp->cur_pos.y, (double)fp->self_vel.x,
            (double)fp->self_vel.y, (double)fp->x8c_kb_vel.x, (double)fp->x8c_kb_vel.y,
            (double)fp->facing_dir, (double)fp->dmg.x1830_percent, fp->motion_id,
            Player_GetStocks(slot));
    }
    if (s_state_log != NULL) {
        fprintf(s_state_log, "net: state %s\n", buf);
        log_state_bits(frame);
        /* A harness SIGKILLs the run a moment after the divergence it was
         * waiting for, so the tail must already be on disc; every 8 frames
         * is one write syscall per 4 KB instead of four per frame. */
        if ((frame & 7) == 0) {
            fflush(s_state_log);
        }
    }
    state_hash_periodic(frame);
}

/* Dump the recorded states around `frame` (both peers do this on DESYNC, so
 * the two logs can be diffed line by line). */
void dump_states_around(int32_t frame) {
    /* Most of the ring: the checksum covers position but not velocity, so
     * the frame a desync is reported on is not the frame the two timelines
     * parted -- that one is found by diffing the two peers' dumps back
     * until the lines agree. */
    for (int32_t f = frame - 40; f <= frame + 1; f++) {
        const char* s = s_state_ring[f & (STATE_RING - 1)];
        if (f >= 0 && strncmp(s, "f", 1) == 0) {
            pc_log_line("net: state %s", s);
        }
    }
}

/* One frame's recorded state line, "" when that frame is not in the ring:
 * the audit (net.c) keeps a copy from before a re-run and compares it with
 * the line the re-run wrote, which names the field that moved instead of
 * leaving a raw memory block to resolve by hand. */
const char* state_line(int32_t frame) {
    const char* s = s_state_ring[frame & (STATE_RING - 1)];
    return s[0] == 'f' ? s : "";
}

/* ---- snapshot ---------------------------------------------------------
 * Whole-region copy of everything the simulation can touch:
 *   1. the decomp's statics, bracketed by src/pc/melee_state.ld (the sound
 *      machine's TUs are excluded there: the audio thread owns them);
 *   2. every OSAlloc heap's live extent except the audio heap;
 *   3. the RNG seed pointer (aurora-side static the game redirects).
 * ponytail: plain memcpy each time; dirty tracking only if the measured cost
 * breaks the rollback budget. */
/* ELF, PE and Mach-O builds all provide simulation-only static ranges.
 * Keep the missing-region guard for isolated fixtures/invalid images; normal
 * project builds require section support and validate their boundaries. */
#ifdef MELEE_STATE_SECTIONS
#include "melee_state.h"
#else
static char s_no_state_region;
#define __melee_data_start (&s_no_state_region)
#define __melee_data_end (&s_no_state_region)
#define __melee_bss_start (&s_no_state_region)
#define __melee_bss_end (&s_no_state_region)
#endif

/* Non-NULL when the platform cannot bracket the decomp's statics; the text is
 * what net.c logs when it drops the session to lockstep. */
const char* snapshot_state_region_missing(void) {
#ifdef MELEE_STATE_SECTIONS
    return __melee_data_end > __melee_data_start ? NULL : "empty .melee_data section";
#else
    return "this platform's linker cannot bracket the decomp's statics";
#endif
}

/* The first regions are fixed; the rest are one per live heap. Only the
 * fixed ones have a length a snapshot may compare, because a heap grows. */
#define FIXED_REGIONS 4

/* Everything a snapshot covers, as it stands right now. */
static int regions_now(Region* r) {
    int n = 0;
    void* lo;
    size_t len;
    aurora_heap_descs(&lo, &len);
    /* The audio heap's BODY is excluded below, because the audio thread
     * allocates in it while the game thread is inside a rollback. Its
     * descriptor has to be excluded for the same reason and was not: a
     * restore rewound the free and allocated list heads while the cell
     * headers they point at stayed as the audio thread left them, which
     * hands the next HSD_AudioMalloc a cell that is already live. It never
     * shows up as a desync -- the checksum does not cover the audio heap --
     * only as lost sound effects or a crash inside OSAlloc. So the array is
     * snapshotted as the two halves either side of that one descriptor. */
    void* alo;
    size_t alen;
    if (!aurora_heap_desc(HSD_Synth_804D6018, &alo, &alen)) {
        alo = (char*)lo + len; /* no audio heap: the second half is empty */
        alen = 0;
    }
    size_t below = (size_t)((char*)alo - (char*)lo);
    r[n++] = (Region){"heapdescs", lo, below};
    r[n++] = (Region){"heapdescs2", (char*)alo + alen, len - below - alen};
    r[n++] = (Region){"data", __melee_data_start, (size_t)(__melee_data_end - __melee_data_start)};
    r[n++] = (Region){"bss", __melee_bss_start, (size_t)(__melee_bss_end - __melee_bss_start)};
    for (int h = 0; h < MAX_HEAPS; h++) {
        void* hi;
        if (h == HSD_Synth_804D6018 || !aurora_heap_extent(h, &lo, &hi)) {
            continue;
        }
        static char names[MAX_HEAPS][8];
        snprintf(names[h], sizeof names[h], "heap%d", h);
        r[n++] = (Region){names[h], lo, (size_t)((char*)hi - (char*)lo)};
    }
    return n;
}

/* Cost of every take/restore and the re-simulation load per present, for
 * the periodic report; the *_worst pair is never reset. */
static uint64_t s_take_ns, s_take_ns_max, s_restore_ns, s_restore_ns_max;
static uint64_t s_take_ns_worst, s_restore_ns_worst;
static unsigned s_takes, s_restores;
static int s_resim_n_max;        /* re-run ticks per present, worst */
static unsigned s_take_inflight; /* takes refused over in-flight transfers */
static bool s_take_refused_io;   /* the last take was one of them */

/* MELEE_NET_SIM_OOM_FRAME=n: the first take at frame >= n fails the way a
 * realloc failure does, to exercise the lockstep fallback. */
static int32_t s_oom_frame = -1;
static bool s_oom_fired;

/* False when the state region is unavailable or the buffer could not be
 * grown; the snapshot is then invalid. */
bool snapshot_take(Snapshot* s, int32_t frame) {
    /* A transfer still in flight owns memory this copy is about to read, and
     * it completes on a DVD or ARQ worker thread with no regard for frames.
     * Restoring such a snapshot rewinds bytes the worker has since written,
     * or worse rewinds them while it is still writing. The tick boundary
     * drains transfers before each tick precisely so this is normally false
     * (net.c dvd_settle), but the drain gives up after five seconds and runs
     * on, and a request issued from a worker never went through it at all.
     * Refuse rather than take one that may not be restorable. This is not
     * a failure of the snapshot machinery: the caller runs just this frame
     * in lockstep and predicts again from the next (net.c snap_predicted). */
    s_take_refused_io = aurora_dvd_inflight() > 0 || aurora_arq_inflight() > 0;
    if (s_take_refused_io) {
        s_take_inflight++;
        s->frame = -1;
        return false;
    }
    if (snapshot_state_region_missing() != NULL) {
        s->frame = -1;
        return false;
    }
    static bool env_read;
    if (!env_read) {
        env_read = true;
        const char* e = getenv("MELEE_NET_SIM_OOM_FRAME");
        s_oom_frame = e != NULL ? (int32_t)atol(e) : -1;
    }
    if (s_oom_frame >= 0 && !s_oom_fired && frame >= s_oom_frame) {
        s_oom_fired = true;
        s->frame = -1;
        return false;
    }
    uint64_t t0 = SDL_GetTicksNS();
    bool intr = OSDisableInterrupts();
    s->frame = -1;
    s->nregions = regions_now(s->regions);
    size_t need = 0;
    for (int i = 0; i < s->nregions; i++) {
        need += s->regions[i].len;
    }
    if (need > s->cap) {
        size_t cap = need * 3 / 2;
        uint8_t* buf = realloc(s->buf, cap);
        if (buf == NULL) {
            OSRestoreInterrupts(intr);
            return false;
        }
        s->buf = buf;
        s->cap = cap;
    }
    s->used = 0;
    for (int i = 0; i < s->nregions; i++) {
        memcpy(s->buf + s->used, s->regions[i].ptr, s->regions[i].len);
        s->used += s->regions[i].len;
    }
    if (s->used > s->faulted) {
        s->faulted = s->used;
    }
    s->seed_ptr = HSD_RandSeedPtr;
    s->seed_val = *HSD_RandSeedPtr;
    s->barrier = net.rb_barrier;
    s->scene = scene_kind();
    s->frame = frame;
    OSRestoreInterrupts(intr);
    uint64_t dt = SDL_GetTicksNS() - t0;
    s_take_ns += dt;
    s_takes++;
    if (dt > s_take_ns_max) {
        s_take_ns_max = dt;
    }
    if (dt > s_take_ns_worst) {
        s_take_ns_worst = dt;
    }
    return true;
}

bool snapshot_refused_io(void) {
    return s_take_refused_io;
}

/* Why a snapshot cannot be restored right now, NULL when it can: the scene
 * must be the one it was taken in and every region must still start where
 * it did (a heap's live extent moves with allocation, so only its base is
 * compared; the restore rewrites the free-list heads along with the cells,
 * so bytes past the old extent are dead space by construction). */
const char* snapshot_unusable(const Snapshot* s) {
    if (s->scene != scene_kind()) {
        return "scene changed";
    }
    Region now[MAX_REGIONS];
    int n = regions_now(now);
    if (n != s->nregions) {
        return "heap set changed";
    }
    for (int i = 0; i < n; i++) {
        if (now[i].ptr != s->regions[i].ptr ||
            (i < FIXED_REGIONS && now[i].len != s->regions[i].len))
        {
            return "region moved";
        }
    }
    return NULL;
}

void snapshot_restore(const Snapshot* s) {
    uint64_t t0 = SDL_GetTicksNS();
    bool intr = OSDisableInterrupts();
    const uint8_t* p = s->buf;
    for (int i = 0; i < s->nregions; i++) {
        memcpy(s->regions[i].ptr, p, s->regions[i].len);
        p += s->regions[i].len;
    }
    HSD_RandSeedPtr = s->seed_ptr;
    *HSD_RandSeedPtr = s->seed_val;
    OSRestoreInterrupts(intr);
    uint64_t dt = SDL_GetTicksNS() - t0;
    s_restore_ns += dt;
    s_restores++;
    if (dt > s_restore_ns_max) {
        s_restore_ns_max = dt;
    }
    if (dt > s_restore_ns_worst) {
        s_restore_ns_worst = dt;
    }
}

/* Re-simulation load of one present (net.c pc_net_after_tick): how many
 * re-run ticks the deepest rollback in that present cost. This is the one
 * number that shows a rollback turning into a dropped frame, so it has to
 * be fed from the re-run loop rather than left at its initial value. */
void resim_note(int ticks) {
    if (ticks > s_resim_n_max) {
        s_resim_n_max = ticks;
    }
}

/* One line of what a snapshot holds, for the "cannot roll back" log. */
const char* snapshot_describe(const Snapshot* s, char* buf, size_t n) {
    size_t heap_bytes = 0;
    for (int i = FIXED_REGIONS; i < s->nregions; i++) {
        heap_bytes += s->regions[i].len;
    }
    snprintf(buf, n, "frame %d scene %d barrier %d seed %08x %d heaps %.2f MB of %.2f MB", s->frame,
        s->scene, s->barrier, s->seed_val,
        s->nregions > FIXED_REGIONS ? s->nregions - FIXED_REGIONS : 0, heap_bytes / 1048576.0,
        s->used / 1048576.0);
    return buf;
}

/* The snapshot fields of the 600-frame report (also used by the sync test);
 * the max pair is per window, the worst pair per process. The full-state
 * hash is the newest one, not a window figure: it is the value a peer would
 * be compared against, so it outlives the window its timings belong to. */
void snap_stats_report(void) {
    pc_log_line("net:   snapshot take %.2f ms (max %.2f, n %u), restore %.2f ms (max %.2f, n %u), "
                "worst ever %.2f/%.2f, resim/present max %d, takes refused (io) %u, "
                "full state hash %016llx at frame %d (%.2f ms, max %.2f, n %u)",
        s_takes ? s_take_ns / 1e6 / s_takes : 0.0, s_take_ns_max / 1e6, s_takes,
        s_restores ? s_restore_ns / 1e6 / s_restores : 0.0, s_restore_ns_max / 1e6, s_restores,
        s_take_ns_worst / 1e6, s_restore_ns_worst / 1e6, s_resim_n_max, s_take_inflight,
        (unsigned long long)s_full_hash, s_full_hash_frame,
        s_full_hashes ? s_full_hash_ns / 1e6 / s_full_hashes : 0.0, s_full_hash_ns_max / 1e6,
        s_full_hashes);
    s_take_ns = s_take_ns_max = s_restore_ns = s_restore_ns_max = 0;
    s_takes = s_restores = 0;
    s_resim_n_max = 0;
    s_take_inflight = 0;
    s_full_hash_ns = s_full_hash_ns_max = 0;
    s_full_hashes = 0;
}

/* rollback_to carries the live PadLibData bookkeeping and raw queue across a
 * restore, but rewinds rumble ownership from the snapshot. Ignore exactly
 * those carried bytes; controller-derived statuses and rumble remain part of
 * the deterministic state checked by the sync test. */
typedef struct SynctestIgnoredSpan {
    const void* ptr;
    size_t len;
} SynctestIgnoredSpan;

static int synctest_ignored_spans(SynctestIgnoredSpan spans[3]) {
    size_t rumble = offsetof(PadLibData, rumble_info);
    spans[0] = (SynctestIgnoredSpan){&HSD_PadLibData, rumble};
    size_t after_rumble = rumble + sizeof HSD_PadLibData.rumble_info;
    spans[1] = (SynctestIgnoredSpan){
        (uint8_t*)&HSD_PadLibData + after_rumble, sizeof HSD_PadLibData - after_rumble};
    int qn = HSD_PadLibData.qnum > 8 ? 8 : HSD_PadLibData.qnum;
    spans[2] = (SynctestIgnoredSpan){HSD_PadLibData.queue,
        HSD_PadLibData.queue != NULL ? (size_t)qn * sizeof *HSD_PadLibData.queue : 0};
    return 3;
}

static bool synctest_ignored_byte(const void* ptr) {
    uintptr_t p = (uintptr_t)ptr;
    SynctestIgnoredSpan spans[3];
    int nspans = synctest_ignored_spans(spans);
    for (int i = 0; i < nspans; i++) {
        uintptr_t lo = (uintptr_t)spans[i].ptr;
        if (p >= lo && p - lo < spans[i].len) {
            return true;
        }
    }
    return false;
}

static int synctest_memcmp(const void* current, const void* saved, size_t len) {
    const uint8_t* a = current;
    const uint8_t* b = saved;
    for (size_t i = 0; i < len; i++) {
        if (!synctest_ignored_byte(a + i) && a[i] != b[i]) {
            return a[i] < b[i] ? -1 : 1;
        }
    }
    return 0;
}

/* Hash of the same regions a snapshot covers, taken fresh from memory. */
static uint64_t state_hash(void) {
    Region r[MAX_REGIONS];
    int n = regions_now(r);
    SynctestIgnoredSpan ignored[3];
    int nignored = synctest_ignored_spans(ignored);
    XXH3_state_t* st = XXH3_createState();
    if (st == NULL) {
        /* Allocation failure is a handled condition in this module
         * (snapshot_take fails soft on realloc); the callers only compare the
         * value, so a constant just makes this frame's hash non-informative. */
        return 0;
    }
    XXH3_64bits_reset(st);
    for (int i = 0; i < n; i++) {
        uintptr_t cursor = (uintptr_t)r[i].ptr;
        uintptr_t end = cursor + r[i].len;
        while (cursor < end) {
            uintptr_t next = end;
            uintptr_t skip_end = cursor;
            for (int j = 0; j < nignored; j++) {
                uintptr_t lo = (uintptr_t)ignored[j].ptr;
                uintptr_t hi = lo + ignored[j].len;
                if (cursor >= lo && cursor < hi) {
                    if (hi > skip_end) {
                        skip_end = hi;
                    }
                } else if (lo > cursor && lo < next) {
                    next = lo;
                }
            }
            if (skip_end > cursor) {
                cursor = skip_end < end ? skip_end : end;
            } else {
                XXH3_64bits_update(st, (const void*)cursor, next - cursor);
                cursor = next;
            }
        }
    }
    XXH3_64bits_update(st, HSD_RandSeedPtr, sizeof(u32));
    uint64_t h = XXH3_64bits_digest(st);
    XXH3_freeState(st);
    return h;
}

/* Tally of differing 64-byte chunks across every mismatch, keyed by address
 * (statics keep their address; heap chunks are keyed by address too, which
 * is stable within a scene). Dumped with the periodic report. */
typedef struct DiffTally {
    const void* addr;
    const char* region;
    unsigned count;
} DiffTally;
#define TALLY_MAX 256
static DiffTally s_tally[TALLY_MAX];
static int s_tally_n;

static void tally_add(const char* region, const void* addr) {
    for (int i = 0; i < s_tally_n; i++) {
        if (s_tally[i].addr == addr) {
            s_tally[i].count++;
            return;
        }
    }
    if (s_tally_n < TALLY_MAX) {
        s_tally[s_tally_n++] = (DiffTally){addr, region, 1};
    }
}

static void tally_report(void) {
    for (int pass = 0; pass < 8 && s_tally_n > 0; pass++) {
        int best = -1;
        for (int i = 0; i < s_tally_n; i++) {
            if (s_tally[i].count > 0 && (best < 0 || s_tally[i].count > s_tally[best].count)) {
                best = i;
            }
        }
        if (best < 0) {
            break;
        }
        pc_log_line(
            "net:   %-6s %p x%u", s_tally[best].region, s_tally[best].addr, s_tally[best].count);
        s_tally[best].count = 0; /* consumed; keeps the table for identity */
    }
}

static void snapshot_diff(const Snapshot* s) {
    const uint8_t* p = s->buf;
    for (int i = 0; i < s->nregions; i++) {
        const Region* r = &s->regions[i];
        for (size_t off = 0; off < r->len; off += 64) {
            size_t n = r->len - off < 64 ? r->len - off : 64;
            if (synctest_memcmp((uint8_t*)r->ptr + off, p + off, n) != 0) {
                tally_add(r->name, (uint8_t*)r->ptr + off);
            }
        }
        p += r->len;
    }
}

/* ---- sync test --------------------------------------------------------
 * MELEE_NET_SYNCTEST=1: every tick is run twice, snapshot -> tick -> hash ->
 * restore -> tick again -> hash, and the two hashes must match. Proves that
 * the snapshot covers all state the tick depends on and that a tick is a
 * pure function of (state, inputs), which is what rollback needs. */
static Snapshot s_snap;   /* state before the tick */
static Snapshot s_after1; /* state after the first run of the tick */
static uint64_t s_hash_first;
static int s_retick; /* 0 normal, 1 first tick done, 2 retick done */
static unsigned s_sync_fail, s_sync_skipped;

void synctest_before_tick(void) {
    snapshot_take(&s_snap, net.frame); /* frame stays -1 on failure: the retick is skipped */
}

bool synctest_after_tick(void) {
    if (s_retick == 0) {
        if (s_snap.frame < 0 || s_snap.frame <= net.rb_barrier || snapshot_unusable(&s_snap)) {
            /* The tick issued a disc read or one may still be completing on
             * a worker thread (IO_QUIET frames of barrier); re-running would
             * issue it twice / lose the completion. Rollback never spans a
             * load either. */
            s_sync_skipped++;
            return false;
        }
        s_hash_first = state_hash();
        snapshot_take(&s_after1, s_snap.frame);
        snapshot_restore(&s_snap);
        s_retick = 1;
        net.resim = true;
        return true;
    }
    net.resim = false;
    s_retick = 0;
    uint64_t second = state_hash();
    if (second != s_hash_first) {
        s_sync_fail++;
        snapshot_diff(&s_after1);
    }
    if ((s_snap.frame % 600) == 0 && s_snap.frame > 0) {
        size_t heap_bytes = 0;
        for (int i = 3; i < s_snap.nregions; i++) {
            heap_bytes += s_snap.regions[i].len;
        }
        pc_log_line("net: synctest frame %d, %u mismatches, %u skipped (I/O), snapshot %.2f MB "
                    "(%d heaps %.2f MB)",
            s_snap.frame, s_sync_fail, s_sync_skipped, s_snap.used / 1048576.0, s_snap.nregions - 3,
            heap_bytes / 1048576.0);
        snap_stats_report();
        tally_report();
    }
    return false;
}

/* ---- rollback ring ----------------------------------------------------
 * One snapshot per predicted frame (Slippi cadence), taken right before the
 * tick that consumes the prediction (net.c snap_predicted / rollback_to). */
static Snapshot s_snaps[SNAPS];

Snapshot* snap_slot(int32_t f) {
    return &s_snaps[f & (SNAPS - 1)];
}

/* Behind the barrier, on the lockstep frames just before a fight can
 * predict (net.c fresh_tick): grow a slot to what a take would need now, with
 * the headroom snapshot_take grows by, and write the bytes a take will copy
 * so they are paged in. Otherwise the first takes are where the allocator
 * runs and where ~6 MB of fresh pages per slot fault in, eight slots in a
 * row, on exactly the frames rollback has just been armed for. Slippi's
 * savestates allocate their region buffers once, up front; GGRS preallocates
 * its state cells.
 *
 * One slot per call: all eight at once is ~50 MB written in one frame, a
 * hitch of its own. The fight's entry barrier gives ten lockstep frames, and
 * a slot not reached by then just grows in snapshot_take as before. Once the
 * heaps stop growing every call is a no-op: nothing is allocated or written
 * unless the need outgrew what that slot has already paged in. A slot keeps
 * its contents (realloc carries them, and only bytes past everything it ever
 * held are written), and a failed allocation leaves snapshot_take its own
 * growth path and lockstep fallback. */
void snaps_reserve(void) {
    Region r[MAX_REGIONS];
    int n = regions_now(r);
    size_t need = 0;
    for (int i = 0; i < n; i++) {
        need += r[i].len;
    }
    for (int i = 0; i < SNAPS; i++) {
        Snapshot* s = &s_snaps[i];
        if (need <= s->faulted) {
            continue;
        }
        if (need > s->cap) {
            size_t cap = need * 3 / 2;
            uint8_t* buf = realloc(s->buf, cap);
            if (buf == NULL) {
                return;
            }
            s->buf = buf;
            s->cap = cap;
        }
        /* faulted >= used always (snapshot_take raises it), so this writes
         * past every byte the slot holds. */
        memset(s->buf + s->faulted, 0, need - s->faulted);
        s->faulted = need;
        return; /* one slot per frame */
    }
}

/* Session end: the buffers go back (a snapshot is a few MB each) and no
 * old snapshot may match a frame of the next session, which restarts at 0. */
static void snapshot_free(Snapshot* s) {
    free(s->buf);
    s->buf = NULL;
    s->cap = s->used = s->faulted = 0;
    s->frame = -1;
}

void snaps_free(void) {
    for (int i = 0; i < SNAPS; i++) {
        snapshot_free(&s_snaps[i]);
    }
    snapshot_free(&s_snap);
    snapshot_free(&s_after1);
}
