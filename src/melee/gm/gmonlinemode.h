#ifndef MELEE_GM_GMONLINEMODE_H
#define MELEE_GM_GMONLINEMODE_H

#include <melee/gm/types.h>

/* PC only: GM_ONLINE, the LAN lobby (GS_ONLINE_LOBBY) -> VS -> results loop
 * driven by src/pc/net_lan.h (docs/netcode-plan.md §8, §10). */

extern GameModeState gm_Mode_Online_States[];

void gm_Scene_OnlineLobby_OnFrame(void);
void gm_Scene_OnlineLobby_OnEnter(void*);

#endif
