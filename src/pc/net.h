/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_NET_H
#define PC_NET_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Netplay prototype: rollback lockstep between two instances over UDP
 * (src/pc/net.c, docs/netcode-plan.md §4). Enabled at boot by
 * MELEE_NET=<peer host:port> or at runtime by pc_net_connect()
 * (src/pc/net_lan.h); see net.c for the other knobs. */

/* Wire protocol version; a peer with another one is refused (both sides
 * report PEER_INCOMPATIBLE). Bump on any change to the packet layouts,
 * Rules or the handshake. */
#define PC_NET_PROTO_VERSION 4
void pc_net_init(void);
bool pc_net_active(void);
/* Controller port the local player drives (0 = P1/host, 1 = P2/guest). */
int pc_net_local_player(void);

/* Frame of the tick being simulated (-1 before the first); identical on both
 * peers, so a scene change scheduled for a given frame lands in sync. */
int32_t pc_net_frame(void);

/* RNG seed agreed for the session (pc_net_connect / match handshake). */
uint32_t pc_net_seed(void);

/* Match handshake progress: 0 idle, 1 pending, 2 done, 3 failed (15 s). */
int pc_net_handshake_state(void);

/* Match rules in force from the RULES handshake until disconnect: unlock-all
 * is on for both peers, frozen stadium is the host's setting. False when no
 * rules are in force (use the local prefs). */
bool pc_net_rules(bool* unlock_all, bool* frozen_stadium);

/* Called once per simulation tick before the pad queue head is consumed.
 * Replaces the head sample's four ports with the synced inputs for this
 * frame, predicting the remote one when it has not arrived (stalling only
 * when the remote is more than the rollback window behind). */
void pc_net_sync(void);

/* Called after each tick. Returns true when the tick must be run again
 * (rollback re-simulation or the MELEE_NET_SYNCTEST self-check). */
bool pc_net_after_tick(void);

/* Called by the frame boundary (src/pc/vi.c) after the pad alarm ran; the
 * returned ns are added to the next pacing wait. Time-sync skips are paid
 * here rather than by sleeping inside a tick. */
uint64_t pc_net_pace_adjust_ns(void);

/* True while re-simulating: sound/music/rumble starts must be suppressed. */
bool pc_net_resim(void);

/* Called whenever the game issues a disc request: a tick that did I/O can
 * never be re-simulated (completions land on worker threads). */
void pc_net_note_io(void);

/* Live netplay numbers for the HUD. False when netplay is not active. */
bool pc_net_stats(int* ping_ms, int* delay_frames, unsigned* rollbacks);

/* Link quality for the HUD/lobby: 0 stable, 1 warning (loss, jitter or
 * deep rollbacks in the last second), 2 stalling (a stall over 500 ms in the
 * last two seconds, or the peer announced it is leaving), 3 reconnecting
 * (the peer has been silent past the stall timeout and the session is being
 * resumed, net.c's MELEE_NET_RECONNECT_MS). A reader that only knows 0-2
 * must treat anything above 2 as at least as bad as 2. */
int pc_net_quality(void);

/* Why the last session ended (kept until the next connect): 0 still up /
 * never broke, 1 the peer left (BYE), 2 timeout, 3 desync, 4 incompatible
 * protocol version, 5 an interruption could not be resumed (the input gap
 * outran the rings, or the peer answered for another session or seed). */
enum { PC_NET_PEER_OK, PC_NET_PEER_LEFT, PC_NET_PEER_TIMEOUT, PC_NET_PEER_DESYNC,
       PC_NET_PEER_INCOMPATIBLE, PC_NET_PEER_RESUME };
int pc_net_peer_status(void);

#ifdef __cplusplus
}
#endif

#endif
