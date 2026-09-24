/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_NET_SFX_H
#define PC_NET_SFX_H
/* Sound effects under rollback: Slippi's PreventDuplicateSounds,
 * KILL_ROLLBACK_SOUNDS and NoDestroyVoice (asm/Online/Core), rebuilt around
 * one change -- the simulation never sees an audio-engine voice id.
 *
 * Handles. In a netplay tick a sound start returns a virtual handle made of
 * the frame and of how many starts the tick made before this one. Both peers
 * and every re-run of a frame hand the game the same value, and it is never
 * -1: the engine's -1 ("no free voice") was each machine's own pool
 * occupancy, and the game branches on it. The engine's voice id for a handle
 * is kept here, and every later call that takes one -- key-off, pan, volume,
 * pitch, "still playing" -- resolves it through net_sfx_resolve(). A handle
 * whose voice is gone resolves to an id the engine no longer knows, which it
 * ignores.
 *
 * Log. Each of the last NET_SFX_FRAMES frames keeps the starts of the
 * timeline that stands ("cur", by call index) and, while a rollback re-runs
 * the frame, the voices of the timeline being replaced ("old"). A re-run
 * start takes the first old voice of the same sound on the same track (the
 * n-th start of a sound in a frame matches the n-th) and plays nothing. One
 * with no match is new and waits. When the re-run has caught up with the
 * present (net_sfx_rollback_done) the old voices nobody took are keyed off,
 * and the waiting starts play, late by the rollback's depth, unless the
 * corrected timeline has already stopped them.
 *
 * Key-offs. A re-run's key-off of a track, or of everything, already happened
 * once on the replaced timeline, and the engine has moved on since. A voice
 * that timeline started after that point would be cut before the re-run
 * reaches the start that keeps it (Slippi's NoDestroyVoice case: a fighter
 * clears its track, then starts its sound on it). Such voices are shielded;
 * see net_sfx_shielded(). A key-off by handle needs no shield, because a
 * handle only reaches voices that the standing timeline owns.
 *
 * Game thread only. None of this is in a snapshot, and that is the point:
 * this state is what survives the rollback. */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NET_SFX_FRAMES 16    /* frames logged; a power of two deeper than any rollback (SNAPS) */
#define NET_SFX_PER_FRAME 32 /* starts logged per frame */

/* net_sfx_set: which parameter a call changes. */
enum { NET_SFX_PAN, NET_SFX_VOLUME, NET_SFX_PITCH, NET_SFX_OTHER };

typedef struct NetSfxStats {
    unsigned played;    /* starts on a frame's first simulation */
    unsigned deduped;   /* re-run starts that took their logged voice and played nothing */
    unsigned late;      /* new on the corrected timeline: started when the rollback finished */
    unsigned killed;    /* still playing but gone from the corrected timeline: keyed off */
    unsigned cancelled; /* new on the corrected timeline and already stopped there: never played */
    unsigned revived;   /* cancelled, until a later rollback re-ran the frame that stopped them */
    unsigned shielded;  /* voices a re-run's track key-off left for the start that keeps them */
    unsigned overflow;  /* starts past NET_SFX_PER_FRAME (a re-run adds none of those) */
    unsigned outside;   /* starts between ticks: a handle, but nothing to log */
    unsigned private_starts; /* audio-private starts (net_sfx_private) */
} NetSfxStats;

/* Session start: forget every frame and handle. */
void net_sfx_reset(void);
/* A tick of `frame` begins; `resim` when it re-runs a frame. A re-run of a
 * frame no newer than the previous tick's is a rollback starting there. */
void net_sfx_begin(int32_t frame, bool resim);
/* That tick is over. A start after this and before the next begin comes from
 * code no rollback re-runs: it gets a handle and plays, and is not logged. */
void net_sfx_end(void);
/* The re-run has caught up with the present: key off what the corrected
 * timeline lost, start what it gained. No-op unless a rollback happened. */
void net_sfx_rollback_done(void);

/* A sound start in a session; returns its handle, never -1. */
int32_t net_sfx_start(int32_t sound, uint8_t volume, uint8_t pan, int32_t track, int32_t channel);
/* True for a value this module handed out as a handle (and never before the
 * first one, so offline voice ids are never taken for handles). */
bool net_sfx_is_handle(int32_t handle);
/* Audio-private starts: lbaudio_ax.c's per-tick sound update decides from
 * its own statics, which no snapshot holds, whether to start the star and
 * hammer loops and the queued sounds. Those starts must not take a place in
 * the tick's call order (they would shift every handle after them by an
 * amount that differs between peers and between re-runs), and "is it still
 * playing" about one is asked by audio-only code, so the engine may answer
 * it. They play when asked, re-run or not, like the offline game. */
void net_sfx_private(bool on);
bool net_sfx_is_private(int32_t handle);
/* The engine voice behind a handle, or -1 (still waiting, or never played). */
int32_t net_sfx_resolve(int32_t handle);
/* Key-off by handle: cancels a start still waiting, else returns the voice
 * for the engine to key off (-1: nothing to do). */
int32_t net_sfx_keyoff(int32_t handle);
/* Pan/volume/pitch by handle: a waiting start keeps the value for when it
 * plays; else returns the voice for the engine to apply it to (-1: none). */
int32_t net_sfx_set(int32_t handle, int what, int32_t value);
/* A key-off of `track` (of every track when `all`) inside a tick: on a re-run
 * it cancels the waiting starts it reaches. The engine then keys off each of
 * its voices on the track that net_sfx_shielded() does not claim. */
void net_sfx_keyoff_track(int32_t track, bool all);
/* True when a re-run's track key-off must spare `voice`: the replaced
 * timeline started it at this frame or later, and nothing has taken it yet. */
bool net_sfx_shielded(int32_t voice);
void net_sfx_stats(NetSfxStats* out);

/* The engine side: axdriver.c in the game, a fake in tools/test_net_sfx.c.
 * A voice id carries its engine slot in its low 7 bits. */
int32_t net_sfx_engine_play(int32_t sound, uint8_t volume, uint8_t pan, int32_t track,
    int32_t channel);                      /* voice id, or -1 */
bool net_sfx_engine_keyoff(int32_t voice); /* true when it was still playing */
/* A start's group cut, made by a re-run's start that waits: key off the
 * track's voices that net_sfx_shielded() does not claim. */
void net_sfx_engine_cut(int32_t track);
void net_sfx_engine_pitch(int32_t voice, int16_t pitch);

#ifdef __cplusplus
}
#endif

#endif
