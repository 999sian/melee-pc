/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Self-check for src/pc/net_sfx.c, the rollback-safe sound log, against a
 * fake engine: 0x60 slots, voice ids (generation << 7) | slot as
 * axdriver.c makes them, and one sound group per nonzero track whose next
 * start cuts the voice it held (synth.c HSD_SynthSFXPlayWithGroup). Each
 * case drives net_sfx the way net.c and axdriver.c do: begin a tick, start
 * and stop sounds in it, roll back by beginning an older frame again, and
 * catch up with net_sfx_rollback_done(). */
#include "pc/net_sfx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* not <assert.h>: the build may define NDEBUG */
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c);                                \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

/* ---- the fake engine -------------------------------------------------- */

#define VOICES 0x60
typedef struct {
    int32_t id;
    int32_t sound;
    int32_t track;
    bool live;
    uint8_t volume, pan;
    int16_t pitch;
} Voice;
static Voice s_v[VOICES];
static int32_t s_gen = 1;
static int s_next;
static int s_plays; /* engine starts, i.e. sounds actually heard */
static bool s_reject;

/* Every voice the engine stops while a tick runs, and the frame that tick
 * simulated: case_random_rollbacks marks the ones a rollback took back. */
typedef struct {
    int32_t frame, sound, track;
    bool replaced;
} Stop;
static Stop s_stops[1 << 14];
static int s_nstops;
static int32_t s_sim_frame = -1;

static void stopped(const Voice* x) {
    if (s_sim_frame >= 0) {
        CHECK(s_nstops < (int)(sizeof s_stops / sizeof s_stops[0]));
        s_stops[s_nstops++] = (Stop){s_sim_frame, x->sound, x->track, false};
    }
}

static void engine_reset(void) {
    memset(s_v, 0, sizeof s_v);
    s_next = 0;
    s_plays = 0;
    s_nstops = 0;
    s_reject = false;
}

static Voice* voice(int32_t id) {
    if (id < 0 || (id & 0x7F) >= VOICES) {
        return NULL;
    }
    Voice* x = &s_v[id & 0x7F];
    return x->id == id ? x : NULL;
}

static int32_t engine_start(int32_t sound, uint8_t volume, uint8_t pan, int32_t track) {
    if (track != 0) {
        for (int i = 0; i < VOICES; i++) {
            if (s_v[i].live && s_v[i].track == track) {
                s_v[i].live = false; /* the group's previous voice */
                stopped(&s_v[i]);
            }
        }
    }
    for (int n = 0; n < VOICES; n++) {
        int slot = (s_next + n) % VOICES;
        if (!s_v[slot].live) {
            s_next = slot + 1;
            s_v[slot] = (Voice){(s_gen++ << 7) | slot, sound, track, true, volume, pan, 0};
            return s_v[slot].id;
        }
    }
    return -1;
}

int32_t net_sfx_engine_play(
    int32_t sound, uint8_t volume, uint8_t pan, int32_t track, int32_t channel) {
    (void)channel;
    s_plays++;
    return s_reject ? -1 : engine_start(sound, volume, pan, track);
}

bool net_sfx_engine_keyoff(int32_t id) {
    Voice* x = voice(id);
    if (x == NULL || !x->live) {
        return false;
    }
    x->live = false;
    stopped(x);
    return true;
}

void net_sfx_engine_pitch(int32_t id, int16_t pitch) {
    Voice* x = voice(id);
    if (x != NULL && x->live) {
        x->pitch = pitch;
    }
}

static void keyoff_track(int32_t track, bool all);
void net_sfx_engine_cut(int32_t track) {
    keyoff_track(track, false);
}

/* ---- the game's side, as axdriver.c routes it -------------------------- */

static int32_t start(int32_t sound, int32_t track) {
    int32_t h = net_sfx_start(sound, 0xFE, 0x80, track, 7);
    CHECK(h != -1 && h > 0 && net_sfx_is_handle(h));
    return h;
}

static void keyoff(int32_t h) {
    int32_t v = net_sfx_keyoff(h);
    if (v != -1) {
        net_sfx_engine_keyoff(v);
    }
}

/* axdriver.c AXDriverKeyOffGated */
static void keyoff_track(int32_t track, bool all) {
    net_sfx_keyoff_track(track, all);
    for (int i = 0; i < VOICES; i++) {
        if (s_v[i].live && (all || s_v[i].track == track) && !net_sfx_shielded(s_v[i].id)) {
            s_v[i].live = false;
            stopped(&s_v[i]);
        }
    }
}

static bool playing(int32_t h) {
    Voice* x = voice(net_sfx_resolve(h));
    return x != NULL && x->live;
}

static int32_t sound_of(int32_t h) {
    Voice* x = voice(net_sfx_resolve(h));
    return x != NULL ? x->sound : -1;
}

static NetSfxStats stats(void) {
    NetSfxStats s;
    net_sfx_stats(&s);
    return s;
}

static void fresh(void) {
    net_sfx_reset();
    engine_reset();
}

enum { A = 1001, B = 1002, C = 1003, D = 1004, T = 0x1E, U = 0x20 };

/* ---- cases ------------------------------------------------------------- */

/* A handle is (frame, call index) and nothing else: not the sound, not the
 * engine's answer. A start the engine refuses still gets one. */
static void case_handles(void) {
    CHECK(!net_sfx_is_handle(0x40000080)); /* nothing handed out yet: offline ids stay ids */
    fresh();
    net_sfx_begin(0, false);
    int32_t h0 = start(A, 0), h1 = start(B, 0);
    CHECK(h0 != h1);
    s_reject = true;
    net_sfx_begin(1, false);
    int32_t h2 = start(C, 0);
    CHECK(net_sfx_resolve(h2) == -1); /* no voice: every call on it is a no-op */
    CHECK(net_sfx_keyoff(h2) == -1);
    CHECK(net_sfx_set(h2, NET_SFX_VOLUME, 3) == -1);
    fresh(); /* the other peer, whose engine is in another state */
    for (int i = 0; i < 40; i++) {
        engine_start(D, 0, 0, 0);
    }
    net_sfx_begin(0, false);
    CHECK(start(C, 0) == h0 && start(A, 0) == h1);
    net_sfx_begin(1, false);
    CHECK(start(B, U) == h2);
    CHECK(net_sfx_resolve(0x12345) == -1);
    net_sfx_reset();
    CHECK(net_sfx_resolve(h0) == -1); /* a new session forgets the old one's handles */
}

/* A re-run with the same inputs (the resim audit): the same handles, no
 * sound starts again, nothing to settle at the end. */
static void case_same_rerun(void) {
    fresh();
    int32_t h[4][2];
    for (int f = 10; f < 14; f++) {
        net_sfx_begin(f, false);
        h[f - 10][0] = start(A + f, 0);
        h[f - 10][1] = start(B, T);
    }
    int plays = s_plays;
    for (int f = 11; f < 14; f++) {
        net_sfx_begin(f, true);
        CHECK(start(A + f, 0) == h[f - 10][0]);
        CHECK(start(B, T) == h[f - 10][1]);
    }
    net_sfx_rollback_done();
    NetSfxStats s = stats();
    CHECK(s_plays == plays);
    CHECK(s.deduped == 6 && s.late == 0 && s.killed == 0 && s.cancelled == 0);
    CHECK(playing(h[3][1]) && sound_of(h[1][0]) == A + 11);
}

/* The corrected timeline keeps B, loses A and gains C: B keeps its voice
 * under its new handle, A is keyed off, C starts late. */
static void case_changed_rerun(void) {
    fresh();
    net_sfx_begin(20, false);
    int32_t ha = start(A, 0), hb = start(B, 0);
    int32_t va = net_sfx_resolve(ha), vb = net_sfx_resolve(hb);
    net_sfx_begin(21, false);
    int plays = s_plays;
    net_sfx_begin(20, true);
    int32_t nb = start(B, 0), nc = start(C, 0);
    CHECK(nb == ha); /* call 0 of frame 20, whatever it starts */
    CHECK(net_sfx_resolve(nb) == vb);
    CHECK(net_sfx_resolve(nc) == -1); /* waits for the catch-up */
    net_sfx_begin(21, true);
    CHECK(voice(va)->live); /* nothing is settled before the re-run catches up */
    net_sfx_rollback_done();
    NetSfxStats s = stats();
    CHECK(s_plays == plays + 1);
    CHECK(s.deduped == 1 && s.late == 1 && s.killed == 1);
    CHECK(!voice(va)->live && voice(vb)->live);
    CHECK(playing(nc) && sound_of(nc) == C);
    CHECK(sound_of(nb) == B);
}

/* An orphan that already finished is not keyed off again, and a voice from
 * before the rollback is never an orphan. */
static void case_finished_orphan(void) {
    fresh();
    net_sfx_begin(29, false);
    int32_t hk = start(A, 0);
    net_sfx_begin(30, false);
    int32_t hd = start(D, 0);
    net_sfx_engine_keyoff(net_sfx_resolve(hd)); /* D ran out on its own */
    net_sfx_begin(30, true);
    net_sfx_rollback_done();
    CHECK(stats().killed == 0);
    CHECK(playing(hk));
}

/* Slippi's NoDestroyVoice: a fighter clears its track, then starts its sound
 * on it. Re-run, the clear must not cut the voice the replaced timeline
 * started right after it, or the dedup hands back a dead voice. A voice
 * from before the key-off is cut as it should be. */
static void case_track_keyoff_rerun(void) {
    fresh();
    net_sfx_begin(39, false);
    int32_t hq = start(A, T); /* on the track since before the rollback */
    net_sfx_begin(40, false);
    keyoff_track(T, false);
    CHECK(!playing(hq));
    int32_t hs = start(B, T);
    int32_t vs = net_sfx_resolve(hs);
    net_sfx_begin(41, false);
    int32_t hl = start(C, U); /* the replaced timeline's future, on another track */
    int32_t vl = net_sfx_resolve(hl);
    net_sfx_begin(40, true);
    CHECK(net_sfx_resolve(hs) == -1); /* the re-run hands (40, 0) out again */
    keyoff_track(T, false);
    CHECK(voice(vs)->live);
    keyoff_track(U, false); /* the corrected timeline clears U before C was ever started */
    CHECK(voice(vl)->live);
    CHECK(start(B, T) == hs && net_sfx_resolve(hs) == vs && playing(hs));
    net_sfx_begin(41, true);
    net_sfx_rollback_done();
    NetSfxStats s = stats();
    CHECK(s.shielded == 2 && s.deduped == 1 && s.killed == 1 && s.late == 0);
    CHECK(playing(hs) && !voice(vl)->live);

    /* Here the corrected timeline clears a track the replaced one left alone:
     * the voice on it predates the rollback and is cut. */
    fresh();
    net_sfx_begin(50, false);
    int32_t hv = start(A, T);
    net_sfx_begin(51, false);
    net_sfx_begin(51, true);
    keyoff_track(T, false);
    CHECK(!playing(hv));
    net_sfx_rollback_done();
    CHECK(stats().shielded == 0);
}

/* A start new on the corrected timeline that the same timeline stops before
 * the catch-up is never heard: by handle, by track, by key-off-all, or cut
 * by a later start on its sound group. */
static void case_cancelled(void) {
    fresh();
    net_sfx_begin(60, false);
    net_sfx_begin(61, false);
    int32_t hm = start(B, T);
    net_sfx_begin(62, false);
    net_sfx_begin(63, false);
    int plays = s_plays;
    net_sfx_begin(60, true);
    int32_t h1 = start(A, 0);
    int32_t h2 = start(C, T);
    int32_t h3 = start(D, U);
    int32_t h4 = start(D, 0);
    keyoff(h1);
    net_sfx_begin(61, true);
    CHECK(start(B, T) == hm); /* cuts C's group before C was heard */
    keyoff_track(U, false);
    net_sfx_begin(62, true);
    keyoff_track(0, true);
    net_sfx_begin(63, true);
    net_sfx_rollback_done();
    NetSfxStats s = stats();
    CHECK(s_plays == plays);
    CHECK(s.cancelled == 4 && s.late == 0 && s.deduped == 1);
    CHECK(net_sfx_resolve(h1) == -1 && net_sfx_resolve(h2) == -1 && net_sfx_resolve(h3) == -1 &&
          net_sfx_resolve(h4) == -1);
    CHECK(!playing(hm)); /* the corrected timeline's key-off-all at 62 reached it */
}

/* Late starts play in timeline order, so the sound group ends up holding
 * the corrected timeline's last voice. */
static void case_late_order(void) {
    fresh();
    net_sfx_begin(70, false);
    int32_t he = start(A, T);
    net_sfx_begin(71, false);
    net_sfx_begin(70, true);
    CHECK(start(A, T) == he);
    net_sfx_begin(71, true);
    int32_t hf = start(B, T);
    net_sfx_rollback_done();
    CHECK(!playing(he) && playing(hf));
}

/* A second rollback before the first caught up: the timeline that stood
 * before either is taken back whole. */
static void case_nested(void) {
    fresh();
    net_sfx_begin(80, false);
    int32_t ha = start(A, 0);
    int32_t va = net_sfx_resolve(ha);
    net_sfx_begin(81, false);
    int32_t hb = start(B, T);
    int32_t vb = net_sfx_resolve(hb);
    net_sfx_begin(82, false);
    int plays = s_plays;
    net_sfx_begin(80, true);
    CHECK(start(C, 0) == ha); /* mispredicted: C waits, A is untaken */
    net_sfx_begin(81, true);
    net_sfx_begin(80, true); /* the next packet corrects it again */
    CHECK(start(A, 0) == ha);
    net_sfx_begin(81, true);
    CHECK(start(B, T) == hb);
    net_sfx_begin(82, true);
    net_sfx_rollback_done();
    NetSfxStats s = stats();
    CHECK(s_plays == plays && s.late == 0 && s.killed == 0);
    CHECK(net_sfx_resolve(ha) == va && net_sfx_resolve(hb) == vb);
    CHECK(voice(va)->live && voice(vb)->live);
}

/* Pan, volume and pitch set on a waiting start reach it when it plays. */
static void case_waiting_params(void) {
    fresh();
    net_sfx_begin(90, false);
    net_sfx_begin(90, true);
    int32_t h = start(A, 0);
    CHECK(net_sfx_set(h, NET_SFX_VOLUME, 77) == -1);
    CHECK(net_sfx_set(h, NET_SFX_PAN, 11) == -1);
    CHECK(net_sfx_set(h, NET_SFX_PITCH, -300) == -1);
    net_sfx_rollback_done();
    Voice* x = voice(net_sfx_resolve(h));
    CHECK(x != NULL && x->live && x->volume == 77 && x->pan == 11 && x->pitch == -300);
    CHECK(net_sfx_set(h, NET_SFX_PAN, 5) == x->id); /* now the engine's to apply */
}

/* The corrected timeline stops a looping voice both timelines started: the
 * key-off by handle reaches it during the re-run. */
static void case_handle_keyoff_rerun(void) {
    fresh();
    net_sfx_begin(100, false);
    int32_t hl = start(A, 0);
    net_sfx_begin(101, false);
    net_sfx_begin(100, true);
    CHECK(start(A, 0) == hl);
    net_sfx_begin(101, true);
    keyoff(hl);
    CHECK(!playing(hl));
    net_sfx_rollback_done();
    CHECK(stats().killed == 0 && stats().deduped == 1);
}

/* A handle whose voice finished and whose engine slot went to another sound
 * started outside the log is a no-op: the engine no longer knows the id. */
static void case_stale_handle(void) {
    fresh();
    net_sfx_begin(110, false);
    int32_t h = start(A, 0);
    int32_t v = net_sfx_resolve(h);
    net_sfx_engine_keyoff(v);
    s_next = v & 0x7F;
    int32_t other = engine_start(B, 0, 0, 0);
    CHECK((other & 0x7F) == (v & 0x7F) && other != v);
    keyoff(h);
    CHECK(voice(other)->live);
}

/* A start between ticks (scene setup) gets a handle that does not depend on
 * the engine, that no tick's start shares, and that a rollback leaves alone:
 * no re-run makes that call again. A re-run's track key-off outside a tick
 * shields nothing. */
static void case_outside(void) {
    fresh();
    net_sfx_begin(120, false);
    int32_t hin = start(A, 0);
    net_sfx_end();
    int32_t o1 = start(B, T), o2 = start(C, 0);
    CHECK(o1 != o2 && o1 != hin && playing(o1) && playing(o2));
    net_sfx_begin(121, false);
    int32_t h121 = start(D, 0);
    CHECK(h121 != o1 && h121 != o2);
    net_sfx_end();
    net_sfx_begin(121, true); /* a rollback over the frame after the setup */
    CHECK(start(D, 0) == h121);
    net_sfx_end();
    net_sfx_rollback_done();
    CHECK(playing(o1) && playing(o2) && stats().killed == 0 && stats().outside == 2);
    int32_t first = o1;
    fresh(); /* the other peer: same calls, other engine state */
    s_reject = true;
    net_sfx_begin(120, false);
    start(A, 0);
    net_sfx_end();
    CHECK(start(B, T) == first);
}

/* Audio-private starts (lbaudio_ax.c's own loop update) take no place in the
 * tick's call order, play on a re-run like the offline game, and are never
 * settled by a catch-up. */
static void case_private(void) {
    fresh();
    net_sfx_begin(130, false);
    int32_t plain = start(A, 0);
    fresh();
    net_sfx_begin(130, false);
    net_sfx_private(true);
    int32_t p = net_sfx_start(D, 0xFE, 0x80, 5, 4);
    net_sfx_private(false);
    CHECK(net_sfx_is_private(p) && !net_sfx_is_private(plain) && playing(p));
    CHECK(start(A, 0) == plain); /* the simulation's handle did not move */
    net_sfx_end();
    net_sfx_begin(131, false);
    net_sfx_end();
    int plays = s_plays;
    net_sfx_begin(130, true);
    net_sfx_private(true);
    int32_t q = net_sfx_start(D, 0xFE, 0x80, 5, 4);
    net_sfx_private(false);
    CHECK(q != p && playing(q) && s_plays == plays + 1);
    CHECK(start(A, 0) == plain);
    net_sfx_end();
    net_sfx_begin(131, true);
    net_sfx_end();
    net_sfx_rollback_done();
    CHECK(playing(q) && stats().killed == 0 && stats().private_starts == 2);
}

/* Random schedules against a reference. Each round plays a timeline of
 * frames whose starts, key-offs and track clears depend on an input; the
 * peer's input arrives up to 7 frames late, frames past it guess the last
 * real one, and a wrong guess is rolled back -- half the time corrected
 * again before the re-run caught up. Whenever every simulated frame has its
 * real input, the voices playing must be a subset of what a run that never
 * guessed has playing, sound by sound: nothing the corrected timeline lacks
 * survives (no orphan), and nothing it has plays twice. And the reverse
 * holds with one exception: a sound the reference has and the run lacks
 * must be one that a simulation a rollback took back had stopped on that
 * track. A key-off or group cut already happened; a voice it stopped that
 * the corrected timeline keeps stays stopped (so it is in Slippi). A start
 * that such a simulation stopped before it was ever heard is no excuse: the
 * log plays it once the frame that stopped it is re-run. */
#define SIM_FRAMES 64
#define LAG 7
static uint32_t s_rng = 12345;
static uint32_t rnd(void) {
    s_rng = s_rng * 1103515245u + 12345u;
    return s_rng >> 16;
}

typedef struct {
    int32_t loop[4]; /* each "fighter"'s held handle (sim state) */
} Sim;

/* One frame of the simulated game: a pure function of (state, input). Every
 * sound is on a track, so the engine holds at most one voice per track. */
static void sim_frame(Sim* g, int frame, uint32_t in) {
    for (int k = 0; k < 4; k++) {
        uint32_t b = (in >> (k * 3)) & 7;
        int32_t track = 0x1E + k * 2;
        if (b == 1) {
            keyoff_track(track, false); /* NoDestroyVoice's shape: clear, then start */
            g->loop[k] = start(A + k, track);
        } else if (b == 2 && g->loop[k] != -1) {
            keyoff(g->loop[k]);
            g->loop[k] = -1;
        } else if (b == 3) {
            start(B + (int32_t)(frame % 3), 0x50 + k); /* a one-shot, on its own group */
        } else if (b == 4) {
            keyoff_track(track, false);
            g->loop[k] = -1;
        } else if (b == 5) {
            g->loop[k] = start(C + k, track); /* cuts the group's voice */
        }
    }
}

/* Live voices as sound * 1000 + track, one slot per track. */
typedef struct {
    int32_t by_track[0x60];
} Heard;

static Heard heard_now(void) {
    Heard h;
    memset(&h, 0, sizeof h);
    for (int i = 0; i < VOICES; i++) {
        if (s_v[i].live) {
            CHECK(s_v[i].track > 0 && s_v[i].track < 0x60);
            CHECK(h.by_track[s_v[i].track] == 0); /* the engine's group rule held */
            h.by_track[s_v[i].track] = s_v[i].sound;
        }
    }
    return h;
}

static void rerun(
    Sim* g, Sim* snap, uint32_t* used, const uint32_t* truth, int from, int to, int have) {
    *g = snap[from];
    for (int r = from; r <= to; r++) {
        snap[r] = *g;
        used[r] = r <= have ? truth[r] : truth[have];
        net_sfx_begin(r, true);
        s_sim_frame = r;
        sim_frame(g, r, used[r]);
        s_sim_frame = -1;
        net_sfx_end();
    }
}

static void case_random_rollbacks(void) {
    unsigned compared = 0, rollbacks = 0, equal = 0;
    NetSfxStats sum = {0};
    for (int round = 0; round < 400; round++) {
        uint32_t truth[SIM_FRAMES];
        for (int f = 0; f < SIM_FRAMES; f++) {
            truth[f] = rnd() % 4 == 0 ? truth[f > 0 ? f - 1 : 0] : rnd() & 0xFFFu;
        }
        /* The reference: no guesses. */
        fresh();
        Sim ref = {{-1, -1, -1, -1}};
        Heard want[SIM_FRAMES];
        for (int f = 0; f < SIM_FRAMES; f++) {
            net_sfx_begin(f, false);
            sim_frame(&ref, f, truth[f]);
            net_sfx_end();
            want[f] = heard_now();
        }
        /* The same game under rollback. */
        fresh();
        Sim snap[SIM_FRAMES];
        Sim g = {{-1, -1, -1, -1}};
        uint32_t used[SIM_FRAMES];
        int have = -1;
        for (int f = 0; f < SIM_FRAMES; f++) {
            snap[f] = g;
            used[f] = f <= have ? truth[f] : (have >= 0 ? truth[have] : 0);
            net_sfx_begin(f, false);
            s_sim_frame = f;
            sim_frame(&g, f, used[f]);
            s_sim_frame = -1;
            net_sfx_end();
            if (rnd() % 3 != 0 || f - have > LAG || f == SIM_FRAMES - 1) {
                have += 1 + (int)(rnd() % 3);
                if (have > f || f - have > LAG || f == SIM_FRAMES - 1) {
                    have = f - (f == SIM_FRAMES - 1 ? 0 : (int)(rnd() % 2));
                }
            }
            int first = -1;
            for (int k = 0; k <= have && first < 0; k++) {
                if (used[k] != truth[k]) {
                    first = k;
                }
            }
            if (first >= 0) {
                rollbacks++;
                for (int i = 0; i < s_nstops; i++) {
                    s_stops[i].replaced |= s_stops[i].frame >= first;
                }
                if (rnd() % 2 && first < f) {
                    /* A correction that lands mid re-run: the first pass had
                     * only part of the real inputs and stops short. */
                    int partial = first + (int)(rnd() % (unsigned)(have - first + 1));
                    rerun(&g, snap, used, truth, first, first + (f - first) / 2, partial);
                    for (int i = 0; i < s_nstops; i++) {
                        s_stops[i].replaced |= s_stops[i].frame >= first;
                    }
                }
                rerun(&g, snap, used, truth, first, f, have);
                net_sfx_rollback_done();
            }
            if (have == f) {
                Heard h = heard_now();
                for (int t = 0; t < 0x60; t++) {
                    CHECK(h.by_track[t] == 0 || h.by_track[t] == want[f].by_track[t]);
                    if (h.by_track[t] == 0 && want[f].by_track[t] != 0) {
                        bool excused = false;
                        for (int i = 0; i < s_nstops && !excused; i++) {
                            excused = s_stops[i].replaced && s_stops[i].track == t &&
                                      s_stops[i].sound == want[f].by_track[t];
                        }
                        CHECK(excused);
                    }
                }
                compared++;
                equal += memcmp(&h, &want[f], sizeof h) == 0;
            }
        }
        NetSfxStats s = stats();
        sum.deduped += s.deduped;
        sum.late += s.late;
        sum.killed += s.killed;
        sum.cancelled += s.cancelled;
        sum.shielded += s.shielded;
    }
    /* Every path ran, and most catch-ups end exactly where the reference is. */
    CHECK(compared > 1000 && rollbacks > 1000);
    CHECK(
        sum.deduped > 0 && sum.late > 0 && sum.killed > 0 && sum.cancelled > 0 && sum.shielded > 0);
    printf("net_sfx: %u rollbacks, %u catch-ups compared (%u exact), deduped %u late %u killed "
           "%u cancelled %u shielded %u\n",
        rollbacks, compared, equal, sum.deduped, sum.late, sum.killed, sum.cancelled, sum.shielded);
}

int main(void) {
    case_handles(); /* first: it checks what holds before any handle exists */
    case_same_rerun();
    case_changed_rerun();
    case_finished_orphan();
    case_track_keyoff_rerun();
    case_cancelled();
    case_late_order();
    case_nested();
    case_waiting_params();
    case_handle_keyoff_rerun();
    case_stale_handle();
    case_outside();
    case_private();
    case_random_rollbacks();
    puts("net_sfx: ok");
    return 0;
}
