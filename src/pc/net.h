/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_NET_H
#define PC_NET_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Netplay prototype: rollback lockstep between two instances over UDP
 * (src/pc/net.c, docs/netcode-plan.md §4). Enabled by MELEE_NET=<peer
 * host:port>; see net.c for the other knobs. */
void pc_net_init(void);
bool pc_net_active(void);

/* Called once per simulation tick before the pad queue head is consumed.
 * Replaces the head sample's four ports with the synced inputs for this
 * frame, predicting the remote one when it has not arrived (stalling only
 * when the remote is more than the rollback window behind). */
void pc_net_sync(void);

/* Called after each tick. Returns true when the tick must be run again
 * (rollback re-simulation or the MELEE_NET_SYNCTEST self-check). */
bool pc_net_after_tick(void);

/* True while re-simulating: sound/music/rumble starts must be suppressed. */
bool pc_net_resim(void);

/* Called whenever the game issues a disc request: a tick that did I/O can
 * never be re-simulated (completions land on worker threads). */
void pc_net_note_io(void);

/* Live netplay numbers for the HUD. False when netplay is not active. */
bool pc_net_stats(int* ping_ms, int* delay_frames, unsigned* rollbacks);

#ifdef __cplusplus
}
#endif

#endif
