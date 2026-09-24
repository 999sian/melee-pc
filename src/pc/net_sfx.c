/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Sound effects under rollback; pc/net_sfx.h has the design. */
#include "pc/net_sfx.h"

#include <string.h>

/* handle = TAG | frame (22 bits, wraps after 19 hours) << 8 | call index.
 * The call index's top two bits: 00/01 a tick's own start, 10 a start
 * between ticks (frame: the next tick's), 11 an audio-private start (frame
 * bits: a running count, not a frame). Bit 30 keeps every handle clear of
 * -1, of every negative value and of the engine's ids ((generation << 7) |
 * slot), whose generation would need 2^23 starts to reach it. */
#define TAG 0x40000000u
#define TAG_MASK 0xC0000000u
#define FRAME_BITS 0x3FFFFFu
#define CALL_SHIFT 8
#define CALL_BITS 0xFFu
#define OUTSIDE 0x80u
#define PRIVATE 0xC0u
#define OLD_MAX (2 * NET_SFX_PER_FRAME)
#define SLOTS 128 /* a voice id's low 7 bits */

enum { EMPTY, LIVE, WAITING, CANCELLED, OLD, TAKEN };

typedef struct {
    int32_t sound;
    int32_t voice; /* engine voice id, -1 none */
    int32_t track;
    int32_t channel;
    int32_t cancelled_at; /* CANCELLED: the frame whose simulation stopped it */
    int16_t pitch;
    uint8_t volume;
    uint8_t pan;
    uint8_t state;
    bool has_pitch;
} Entry;

typedef struct {
    int32_t frame; /* -1: unused */
    int ncur;
    int nold;
    bool blind;                   /* starts this log does not hold: a re-run adds none */
    Entry cur[NET_SFX_PER_FRAME]; /* the standing timeline, by call index */
    Entry old[OLD_MAX];           /* voices of a timeline being replaced, in its call order */
} Frame;

static Frame s_log[NET_SFX_FRAMES];
/* handle -> voice, one per engine slot, so a binding lives exactly as long as
 * the slot is not reused: a looping sound started minutes ago still
 * resolves, a finished one resolves to an id the engine has since dropped. */
static struct {
    int32_t handle; /* 0: none (no handle is 0) */
    int32_t voice;
} s_bind[SLOTS];
static int32_t s_frame = -1;  /* frame of the running tick */
static int32_t s_newest = -1; /* newest frame the log holds */
static int32_t s_last = -1;   /* frame of the previous tick */
static int s_call;            /* starts so far this tick */
static int s_out;             /* starts so far between the last tick and the next */
static bool s_in_tick;        /* between net_sfx_begin and net_sfx_end */
static bool s_private;        /* net_sfx_private: the caller's state is not the simulation's */
static uint32_t s_priv_seq;   /* audio-private starts so far */
static bool s_resim;          /* the running tick re-runs its frame */
static bool s_replaced;       /* a rollback replaced frames the catch-up has not settled */
static bool s_issued;         /* a handle was ever handed out */
static NetSfxStats s_stats;

static int32_t handle_of(int32_t frame, unsigned call) {
    return (int32_t)(TAG | ((uint32_t)frame & FRAME_BITS) << CALL_SHIFT | (call & CALL_BITS));
}

static Frame* frame_at(int32_t frame) {
    Frame* s = &s_log[frame & (NET_SFX_FRAMES - 1)];
    return frame >= 0 && s->frame == frame ? s : NULL;
}

/* The oldest frame the log can still hold. */
static int32_t oldest(void) {
    int32_t f = s_newest - NET_SFX_FRAMES + 1;
    return f > 0 ? f : 0;
}

static void bind(int32_t handle, int32_t voice) {
    for (int i = 0; i < SLOTS; i++) {
        if (s_bind[i].handle == handle) {
            s_bind[i].handle = 0;
        }
    }
    if (voice < 0) {
        return; /* no voice: the handle resolves to nothing */
    }
    s_bind[voice & (SLOTS - 1)].handle = handle;
    s_bind[voice & (SLOTS - 1)].voice = voice;
}

static void unbind(int32_t voice) {
    if (voice >= 0 && s_bind[voice & (SLOTS - 1)].voice == voice) {
        s_bind[voice & (SLOTS - 1)].handle = 0;
    }
}

/* The start a handle names, while it waits for the catch-up. */
static Entry* waiting(int32_t handle) {
    uint32_t bits = ((uint32_t)handle >> CALL_SHIFT) & FRAME_BITS;
    int call = (int)((uint32_t)handle & CALL_BITS);
    Frame* s = &s_log[bits & (NET_SFX_FRAMES - 1)];
    if (s->frame < 0 || ((uint32_t)s->frame & FRAME_BITS) != bits || call >= s->ncur) {
        return NULL;
    }
    return s->cur[call].state == WAITING ? &s->cur[call] : NULL;
}

static void kill(int32_t voice) {
    if (voice >= 0 && net_sfx_engine_keyoff(voice)) {
        s_stats.killed++;
    }
}

/* A rollback to `from`: every frame from there on is about to be simulated
 * again, so what stands there becomes the timeline being replaced. Its
 * handles are unbound, because the re-run hands the same values to whatever
 * it starts at those call indices. Starts still waiting never played and
 * are dropped; the re-run makes them again if it still has them. An older
 * start that a frame from here on stopped before it was heard waits again:
 * the frame that stopped it is not decided any more. */
static void replace_from(int32_t from) {
    if (from < oldest()) {
        from = oldest();
    }
    for (int32_t fr = oldest(); fr < from; fr++) {
        Frame* s = frame_at(fr);
        for (int i = 0; s != NULL && i < s->ncur; i++) {
            if (s->cur[i].state == CANCELLED && s->cur[i].cancelled_at >= from) {
                s->cur[i].state = WAITING;
                s_stats.revived++;
            }
        }
    }
    for (int32_t fr = from; fr <= s_newest; fr++) {
        Frame* s = frame_at(fr);
        if (s == NULL) {
            continue;
        }
        Entry old[OLD_MAX];
        int n = 0;
        for (int i = 0; i < s->ncur; i++) { /* the timeline that stood, first */
            Entry* e = &s->cur[i];
            if (e->state != LIVE) {
                continue;
            }
            unbind(e->voice);
            if (n < OLD_MAX) {
                old[n] = *e;
                old[n++].state = OLD;
            } else {
                kill(e->voice);
                s_stats.overflow++;
            }
        }
        for (int i = 0; i < s->nold; i++) { /* then what an unfinished re-run left untaken */
            if (s->old[i].state != OLD) {
                continue;
            }
            if (n < OLD_MAX) {
                old[n++] = s->old[i];
            } else {
                kill(s->old[i].voice);
                s_stats.overflow++;
            }
        }
        memcpy(s->old, old, (size_t)n * sizeof *old);
        s->nold = n;
        s->ncur = 0;
    }
    s_replaced = true;
}

static void cancel(Entry* e) {
    e->state = CANCELLED;
    e->cancelled_at = s_frame;
    s_stats.cancelled++;
}

/* Waiting starts on `track` (every one when `all`) that something later on
 * the re-run's timeline has already silenced. */
static void cancel_waiting(int32_t track, bool all) {
    for (int32_t fr = oldest(); fr <= s_newest; fr++) {
        Frame* s = frame_at(fr);
        for (int i = 0; s != NULL && i < s->ncur; i++) {
            Entry* e = &s->cur[i];
            if (e->state == WAITING && (all || e->track == track)) {
                cancel(e);
            }
        }
    }
}

void net_sfx_reset(void) {
    memset(s_log, 0, sizeof s_log);
    for (int i = 0; i < NET_SFX_FRAMES; i++) {
        s_log[i].frame = -1;
    }
    memset(s_bind, 0, sizeof s_bind);
    s_frame = s_newest = s_last = -1;
    s_call = s_out = 0;
    s_in_tick = s_resim = s_replaced = s_private = false;
    memset(&s_stats, 0, sizeof s_stats);
}

void net_sfx_begin(int32_t frame, bool resim) {
    if (!resim && s_replaced) {
        net_sfx_rollback_done(); /* a re-run that never caught up: settle it now */
    }
    s_call = s_out = 0;
    s_in_tick = true;
    s_frame = frame;
    s_resim = resim;
    if (resim && frame <= s_last) {
        replace_from(frame);
    }
    s_last = frame;
    if (frame > s_newest) {
        /* A frame the log has not seen; the slot's older frame ages out. A
         * re-run cannot reach one, but if it did, what it played is unknown. */
        Frame* s = &s_log[frame & (NET_SFX_FRAMES - 1)];
        s->frame = frame;
        s->ncur = s->nold = 0;
        s->blind = resim;
        s_newest = frame;
    }
}

void net_sfx_end(void) {
    s_in_tick = false;
    s_out = 0;
}

void net_sfx_rollback_done(void) {
    s_resim = false;
    if (!s_replaced) {
        return;
    }
    s_replaced = false;
    /* What the corrected timeline no longer has, first: a late start below
     * may share a sound group with one of these. The engine answers false
     * for a voice that has already finished, and that is not counted. */
    for (int32_t fr = oldest(); fr <= s_newest; fr++) {
        Frame* s = frame_at(fr);
        for (int i = 0; s != NULL && i < s->nold; i++) {
            if (s->old[i].state == OLD) {
                kill(s->old[i].voice);
            }
        }
        if (s != NULL) {
            s->nold = 0;
        }
    }
    /* Then what it gained, in timeline order. */
    for (int32_t fr = oldest(); fr <= s_newest; fr++) {
        Frame* s = frame_at(fr);
        for (int i = 0; s != NULL && i < s->ncur; i++) {
            Entry* e = &s->cur[i];
            if (e->state == WAITING) {
                e->voice = net_sfx_engine_play(e->sound, e->volume, e->pan, e->track, e->channel);
                if (e->has_pitch && e->voice >= 0) {
                    net_sfx_engine_pitch(e->voice, e->pitch);
                }
                e->state = LIVE;
                bind(handle_of(fr, i), e->voice);
                s_stats.late++;
            }
        }
    }
}

int32_t net_sfx_start(int32_t sound, uint8_t volume, uint8_t pan, int32_t track, int32_t channel) {
    s_issued = true;
    if (s_private) {
        /* Decided by audio-only state (lbaudio_ax.c's own statics, outside
         * every snapshot, that no rollback rewinds): never logged, never
         * sim-visible, and kept out of the tick's call count so it cannot
         * shift the handles the simulation does keep. */
        uint32_t n = s_priv_seq++;
        int32_t handle = handle_of((int32_t)(n >> 6), PRIVATE | (n & 0x3Fu));
        bind(handle, net_sfx_engine_play(sound, volume, pan, track, channel));
        s_stats.private_starts++;
        return handle;
    }
    if (!s_in_tick) {
        /* Between ticks (a scene's setup or teardown): no rollback re-runs
         * this code, so nothing to log, but the game may keep the answer.
         * The handle counts from the tick that comes next. */
        int32_t handle = handle_of(s_last + 1, OUTSIDE | (unsigned)(s_out++ & 0x3F));
        bind(handle, net_sfx_engine_play(sound, volume, pan, track, channel));
        s_stats.outside++;
        return handle;
    }
    int call = s_call++;
    int32_t handle = handle_of(s_frame, (unsigned)call & ~OUTSIDE);
    Frame* s = frame_at(s_frame);
    bool logged = s != NULL && call < NET_SFX_PER_FRAME;
    Entry e = {sound, -1, track, channel, -1, 0, volume, pan, LIVE, false};
    if (!s_resim) {
        e.voice = net_sfx_engine_play(sound, volume, pan, track, channel);
        bind(handle, e.voice);
        s_stats.played++;
        if (logged) {
            s->cur[call] = e;
            s->ncur = call + 1;
        } else {
            if (s != NULL) {
                s->blind = true;
            }
            s_stats.overflow++;
        }
        return handle;
    }
    /* A re-run. A start on a sound group (track != 0) cuts whatever the group
     * holds when it plays, so a start still waiting on the same group from
     * earlier in the re-run would be cut by this one before it is heard. */
    if (track != 0) {
        cancel_waiting(track, false);
    }
    Entry* o = NULL;
    for (int i = 0; s != NULL && i < s->nold; i++) {
        Entry* c = &s->old[i];
        if (c->state == OLD && c->sound == sound && c->track == track) {
            o = c;
            break;
        }
    }
    if (o != NULL) {
        o->state = TAKEN;
        e.voice = o->voice;
        bind(handle, e.voice);
        s_stats.deduped++;
    } else if (!logged || s->blind) {
        s_stats.overflow++; /* this frame played more than the log holds: add nothing */
    } else {
        e.state = WAITING;
        /* The start's group cut belongs to this point of the corrected
         * timeline, whether or not the start is still standing when the
         * catch-up plays it: what the group holds now is cut now. A taken
         * start needs none, since the start it took made the same cut. */
        if (track != 0) {
            net_sfx_engine_cut(track);
        }
    }
    if (logged) {
        s->cur[call] = e;
        s->ncur = call + 1;
    }
    return handle;
}

bool net_sfx_is_handle(int32_t handle) {
    return s_issued && ((uint32_t)handle & TAG_MASK) == TAG;
}

bool net_sfx_is_private(int32_t handle) {
    return net_sfx_is_handle(handle) && ((uint32_t)handle & PRIVATE) == PRIVATE;
}

void net_sfx_private(bool on) {
    s_private = on;
}

int32_t net_sfx_resolve(int32_t handle) {
    for (int i = 0; handle != 0 && i < SLOTS; i++) {
        if (s_bind[i].handle == handle) {
            return s_bind[i].voice;
        }
    }
    return -1;
}

int32_t net_sfx_keyoff(int32_t handle) {
    Entry* e = waiting(handle);
    if (e != NULL) {
        cancel(e);
        return -1;
    }
    return net_sfx_resolve(handle);
}

int32_t net_sfx_set(int32_t handle, int what, int32_t value) {
    Entry* e = waiting(handle);
    if (e == NULL) {
        return net_sfx_resolve(handle);
    }
    if (what == NET_SFX_PAN) {
        e->pan = (uint8_t)value;
    } else if (what == NET_SFX_VOLUME) {
        e->volume = (uint8_t)value;
    } else if (what == NET_SFX_PITCH) {
        e->pitch = (int16_t)value;
        e->has_pitch = true;
    }
    return -1;
}

void net_sfx_keyoff_track(int32_t track, bool all) {
    if (s_resim && s_in_tick) {
        cancel_waiting(track, all);
    }
}

bool net_sfx_shielded(int32_t voice) {
    if (!s_resim || !s_in_tick || voice < 0) {
        return false;
    }
    for (int32_t fr = s_frame; fr <= s_newest; fr++) {
        Frame* s = frame_at(fr);
        for (int i = 0; s != NULL && i < s->nold; i++) {
            if (s->old[i].state == OLD && s->old[i].voice == voice) {
                s_stats.shielded++;
                return true;
            }
        }
    }
    return false;
}

void net_sfx_stats(NetSfxStats* out) {
    *out = s_stats;
}
