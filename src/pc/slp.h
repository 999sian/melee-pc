/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Slippi-compatible replay recording (src/pc/slp.c). MELEE_SLP_DIR=<dir>
 * writes every VS match -- offline or netplay -- to
 * <dir>/Game_YYYYMMDDTHHMMSS.slp, readable by slippi-js, Slippi Launcher and
 * the tools built on them. Off unless the variable is set.
 *
 * Each tick is captured when it ends and staged by netplay frame; a frame
 * reaches the file only once no rollback can change it (offline, on the next
 * tick), so the file holds exactly one pass per frame. */
#ifndef PC_SLP_H
#define PC_SLP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct HSD_GObj;
struct StartMeleeData;

/* A VS or Sudden Death scene is being set up (gmvs.c fn_8016E730, where
 * Slippi's SendGameInfo hooks): opens a file if this match is recorded. */
void pc_slp_match_start(const struct StartMeleeData* data);
/* The match scene is leaving (gmvs.c gm_Scene_Vs_OnExit): every staged frame
 * is written, Game End included if it happened, and the file is closed. */
void pc_slp_match_end(void);
/* Around every simulation tick, fresh or re-run (gmscene.c gm_RunSimTick).
 * `proc_mask` is the GObj p_link mask the tick ran its procs under; a tick
 * whose mask held the fighters (pause, GAME! freeze) is no replay frame. */
void pc_slp_tick_begin(void);
void pc_slp_tick_end(uint64_t proc_mask);
/* A fighter's inputs for the tick are read (fighter.c
 * Fighter_Spaghetti_8006AD10, where Slippi's SendGamePreFrame hooks): the
 * pre-frame event's state and inputs are taken here. */
void pc_slp_pre_frame(struct HSD_GObj* gobj);

/* net_snapshot.c, for the recorder. The netplay frame number of the tick
 * running: a rollback's re-run frame, otherwise the newest fresh one (it
 * counts every tick offline too). */
int32_t pc_net_sim_frame(void);
/* The newest netplay frame no rollback can change (record_confirm);
 * INT32_MIN when no session is running. */
int32_t pc_net_confirmed_frame(void);

#ifdef __cplusplus
}
#endif

#endif
