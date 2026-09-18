#ifndef MELEE_IF_IFNET_H
#define MELEE_IF_IFNET_H

/* Netplay HUD text ("P1  delay 2  ping 34ms  rb 3"). No-ops when netplay
 * is not active. Created and freed with the match HUD (ifall.c). */
void ifNet_Create(void);
void ifNet_Free(void);

#endif
