/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Netplay state capture: whole-region snapshots for rollback, the per-frame
 * checksum the peers compare, the state ring dumped on a DESYNC, the
 * MELEE_NET_SYNCTEST self-check and MELEE_NET_RECORD/REPLAY. Game thread
 * only (net_internal.h). */
#include "compat.h"
#include "pc/net_internal.h"

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
 *                        actually simulated plus the frame checksum.
 * MELEE_NET_REPLAY=file  feeds those pads back in and reports the first
 *                        frame whose checksum differs: the determinism test
 *                        for M0 (docs/netcode-plan.md §5). Works solo or
 *                        together with netplay (record only). */
static FILE* s_rec;
static FILE* s_rep;
static bool s_rep_reported;

typedef struct FrameRecord {
    PADStatus pads[4];
    uint32_t ck;
} FrameRecord;

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
        if (s_rep && (fread(magic, 4, 1, s_rep) != 1 || memcmp(magic, "MRC1", 4) != 0 ||
                      fread(&seed, 4, 1, s_rep) != 1)) {
            fclose(s_rep);
            s_rep = NULL;
        }
        if (s_rep) {
            *HSD_RandSeedPtr = seed;
        }
        pc_log_line("net: %s %s", s_rep ? "replaying" : "cannot open", rep);
    }
}

bool record_active(void) {
    return s_rec != NULL || s_rep != NULL;
}

static FrameRecord s_rep_cur;

/* Load the next record's pads for this frame; false at end of file. */
static bool replay_load(PADStatus* head) {
    if (fread(&s_rep_cur, sizeof s_rep_cur, 1, s_rep) != 1) {
        return false;
    }
    memcpy(head, s_rep_cur.pads, sizeof s_rep_cur.pads);
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

/* After the checksum of net.frame: write the record / compare the replay. */
void record_frame(const PADStatus* head, uint32_t ck) {
    if (s_rec != NULL) {
        if (net.frame == 0) {
            fwrite("MRC1", 4, 1, s_rec);
            fwrite(HSD_RandSeedPtr, 4, 1, s_rec);
        }
        FrameRecord r;
        memcpy(r.pads, head, sizeof r.pads);
        r.ck = ck;
        fwrite(&r, sizeof r, 1, s_rec);
        fflush(s_rec); /* runs usually end by SIGTERM; keep every frame */
    }
    if (s_rep != NULL) {
        replay_compare(ck);
    }
}

/* ---- frame checksum --------------------------------------------------- */

uint32_t frame_checksum(const PADStatus* head) {
    /* Inputs as simulated, the RNG seed entering the frame, and each
     * fighter's position, facing, percent, stocks and action state (plan
     * §4), so a physics divergence is caught on the frame it happens. */
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
    }
    return ck;
}

/* What went into the checksum, one line, so two peers' logs can be diffed
 * by eye when a DESYNC is reported. */
#define STATE_RING 64
static char s_state_ring[STATE_RING][320];

/* Remember what went into this frame's checksum (overwritten when the
 * frame is re-simulated, so the last write is the confirmed timeline). */
void record_state(const PADStatus* head, int32_t frame) {
    char* buf = s_state_ring[frame & (STATE_RING - 1)];
    int n = snprintf(buf, sizeof s_state_ring[0], "f%d seed=%08x pads=%04x/%d,%d %04x/%d,%d", frame,
                     *HSD_RandSeedPtr, head[0].button, head[0].stickX, head[0].stickY,
                     head[1].button, head[1].stickX, head[1].stickY);
    for (int slot = 0; in_fight() && slot < 4 && n < (int) sizeof s_state_ring[0] - 80; slot++) {
        HSD_GObj* gobj = Player_GetEntity(slot);
        if (gobj == NULL || gobj->classifier != HSD_GOBJ_CLASS_FIGHTER) {
            continue;
        }
        const Fighter* fp = GET_FIGHTER(gobj);
        n += snprintf(buf + n, sizeof s_state_ring[0] - (size_t) n,
                      " p%d=(%.3f,%.3f) v(%.3f,%.3f) kb(%.3f,%.3f) f%.0f %.1f%% m%d s%d", slot,
                      (double) fp->cur_pos.x, (double) fp->cur_pos.y, (double) fp->self_vel.x,
                      (double) fp->self_vel.y, (double) fp->x8c_kb_vel.x,
                      (double) fp->x8c_kb_vel.y, (double) fp->facing_dir,
                      (double) fp->dmg.x1830_percent, fp->motion_id, Player_GetStocks(slot));
    }
}

/* Dump the recorded states around `frame` (both peers do this on DESYNC, so
 * the two logs can be diffed line by line). */
void dump_states_around(int32_t frame) {
    for (int32_t f = frame - 3; f <= frame + 1; f++) {
        const char* s = s_state_ring[f & (STATE_RING - 1)];
        if (f >= 0 && strncmp(s, "f", 1) == 0) {
            pc_log_line("net: state %s", s);
        }
    }
}

/* ---- snapshot ---------------------------------------------------------
 * Whole-region copy of everything the simulation can touch:
 *   1. the decomp's statics, bracketed by src/pc/melee_state.ld (the sound
 *      machine's TUs are excluded there: the audio thread owns them);
 *   2. every OSAlloc heap's live extent except the audio heap;
 *   3. the RNG seed pointer (aurora-side static the game redirects).
 * ponytail: plain memcpy each time; dirty tracking only if the measured cost
 * breaks the rollback budget. */
/* src/pc/melee_state.ld brackets the decomp's statics, but only the ELF links
 * take it (CMakeLists.txt: Linux and Android). GNU ld for PE/COFF and ld64
 * have no INSERT AFTER, so on Windows and Apple the four symbols have no
 * definition: without them the link fails outright, and defining them as an
 * empty span would be worse — snapshot_take would happily copy the heaps and
 * silently omit every static, i.e. rollback into a desync. So they are
 * defined here as an empty span on those platforms and snapshot_take refuses,
 * which drops the session to lockstep for good (net.c snap_predicted raises
 * the barrier to INT32_MAX). Netplay works there, at lockstep latency.
 * ponytail: no rollback on Windows/Apple until the region is named another
 * way; the upgrade path is per-object section renaming with objcopy at
 * archive level, or a PE linker script if GNU ld accepts SECTIONS/INSERT for
 * that target. */
#ifdef MELEE_STATE_SECTIONS
extern char __melee_data_start[], __melee_data_end[];
extern char __melee_bss_start[], __melee_bss_end[];
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

/* Everything a snapshot covers, as it stands right now. */
static int regions_now(Region* r) {
    int n = 0;
    void* lo;
    size_t len;
    aurora_heap_descs(&lo, &len);
    r[n++] = (Region) { "heapdescs", lo, len };
    r[n++] = (Region) { "data", __melee_data_start, (size_t) (__melee_data_end - __melee_data_start) };
    r[n++] = (Region) { "bss", __melee_bss_start, (size_t) (__melee_bss_end - __melee_bss_start) };
    for (int h = 0; h < MAX_HEAPS; h++) {
        void* hi;
        if (h == HSD_Synth_804D6018 || !aurora_heap_extent(h, &lo, &hi)) {
            continue;
        }
        static char names[MAX_HEAPS][8];
        snprintf(names[h], sizeof names[h], "heap%d", h);
        r[n++] = (Region) { names[h], lo, (size_t) ((char*) hi - (char*) lo) };
    }
    return n;
}

/* Cost of every take/restore and the re-simulation load per present, for
 * the periodic report; the *_worst pair is never reset. */
static uint64_t s_take_ns, s_take_ns_max, s_restore_ns, s_restore_ns_max;
static uint64_t s_take_ns_worst, s_restore_ns_worst;
static unsigned s_takes, s_restores;
static int s_resim_n_max;                /* re-run ticks per present, worst */
static unsigned s_resim_splits;          /* rollbacks that spilled into the next present */

/* MELEE_NET_SIM_OOM_FRAME=n: the first take at frame >= n fails the way a
 * realloc failure does, to exercise the lockstep fallback. */
static int32_t s_oom_frame = -1;
static bool s_oom_fired;

/* False when the state region is unavailable or the buffer could not be
 * grown; the snapshot is then invalid. */
bool snapshot_take(Snapshot* s, int32_t frame) {
    if (snapshot_state_region_missing() != NULL) {
        s->frame = -1;
        return false;
    }
    static bool env_read;
    if (!env_read) {
        env_read = true;
        const char* e = getenv("MELEE_NET_SIM_OOM_FRAME");
        s_oom_frame = e != NULL ? (int32_t) atol(e) : -1;
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
        if (now[i].ptr != s->regions[i].ptr || (i < 3 && now[i].len != s->regions[i].len)) {
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

/* Re-simulation load of one present (net.c pc_net_after_tick). */
void resim_note(int ticks, bool split) {
    if (ticks > s_resim_n_max) {
        s_resim_n_max = ticks;
    }
    if (split) {
        s_resim_splits++;
    }
}

/* One line of what a snapshot holds, for the "cannot roll back" log. */
const char* snapshot_describe(const Snapshot* s, char* buf, size_t n) {
    size_t heap_bytes = 0;
    for (int i = 3; i < s->nregions; i++) {
        heap_bytes += s->regions[i].len;
    }
    snprintf(buf, n, "frame %d scene %d barrier %d seed %08x %d heaps %.2f MB of %.2f MB", s->frame,
             s->scene, s->barrier, s->seed_val, s->nregions > 3 ? s->nregions - 3 : 0,
             heap_bytes / 1048576.0, s->used / 1048576.0);
    return buf;
}

/* The snapshot fields of the 600-frame report (also used by the sync test);
 * the max pair is per window, the worst pair per process. */
void snap_stats_report(void) {
    pc_log_line("net:   snapshot take %.2f ms (max %.2f, n %u), restore %.2f ms (max %.2f, n %u), "
                "worst ever %.2f/%.2f, resim/present max %d, split %u",
                s_takes ? s_take_ns / 1e6 / s_takes : 0.0, s_take_ns_max / 1e6, s_takes,
                s_restores ? s_restore_ns / 1e6 / s_restores : 0.0, s_restore_ns_max / 1e6,
                s_restores, s_take_ns_worst / 1e6, s_restore_ns_worst / 1e6, s_resim_n_max,
                s_resim_splits);
    s_take_ns = s_take_ns_max = s_restore_ns = s_restore_ns_max = 0;
    s_takes = s_restores = 0;
    s_resim_n_max = 0;
}

/* Hash of the same regions a snapshot covers, taken fresh from memory. */
static uint64_t state_hash(void) {
    Region r[MAX_REGIONS];
    int n = regions_now(r);
    XXH3_state_t* st = XXH3_createState();
    XXH3_64bits_reset(st);
    for (int i = 0; i < n; i++) {
        XXH3_64bits_update(st, r[i].ptr, r[i].len);
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
        s_tally[s_tally_n++] = (DiffTally) { addr, region, 1 };
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
        pc_log_line("net:   %-6s %p x%u", s_tally[best].region, s_tally[best].addr,
                    s_tally[best].count);
        s_tally[best].count = 0; /* consumed; keeps the table for identity */
    }
}

static void snapshot_diff(const Snapshot* s) {
    const uint8_t* p = s->buf;
    for (int i = 0; i < s->nregions; i++) {
        const Region* r = &s->regions[i];
        for (size_t off = 0; off < r->len; off += 64) {
            size_t n = r->len - off < 64 ? r->len - off : 64;
            if (memcmp((uint8_t*) r->ptr + off, p + off, n) != 0) {
                tally_add(r->name, (uint8_t*) r->ptr + off);
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
static Snapshot s_snap;       /* state before the tick */
static Snapshot s_after1;     /* state after the first run of the tick */
static uint64_t s_hash_first;
static int s_retick;          /* 0 normal, 1 first tick done, 2 retick done */
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
                    s_snap.frame, s_sync_fail, s_sync_skipped, s_snap.used / 1048576.0,
                    s_snap.nregions - 3, heap_bytes / 1048576.0);
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

/* Session end: the buffers go back (a snapshot is a few MB each) and no
 * old snapshot may match a frame of the next session, which restarts at 0. */
static void snapshot_free(Snapshot* s) {
    free(s->buf);
    s->buf = NULL;
    s->cap = s->used = 0;
    s->frame = -1;
}

void snaps_free(void) {
    for (int i = 0; i < SNAPS; i++) {
        snapshot_free(&s_snaps[i]);
    }
    snapshot_free(&s_snap);
    snapshot_free(&s_after1);
}
