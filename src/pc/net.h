/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_NET_H
#define PC_NET_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Netplay prototype: delay-based input lockstep between two instances over
 * UDP (src/pc/net.c). Enabled by MELEE_NET=<peer host:port>; see net.c for
 * the other knobs. Rollback is not implemented yet, this is the substrate
 * (input sync, frame index, desync checksum) it will sit on. */
void pc_net_init(void);
bool pc_net_active(void);

/* Called once per simulation tick before the pad queue head is consumed.
 * Replaces the head sample's four ports with the synced inputs for this
 * frame, blocking until the remote input arrives. */
void pc_net_sync(void);

/* Called after each tick. Returns true when the tick must be run again
 * (rollback re-simulation or the MELEE_NET_SYNCTEST self-check). */
bool pc_net_after_tick(void);

/* True while re-simulating: sound/music/rumble starts must be suppressed. */
bool pc_net_resim(void);

/* Called whenever the game issues a disc request: a tick that did I/O can
 * never be re-simulated (completions land on worker threads). */
void pc_net_note_io(void);

#ifdef __cplusplus
}
#endif

#endif
