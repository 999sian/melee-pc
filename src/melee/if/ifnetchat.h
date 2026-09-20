#ifndef MELEE_IF_IFNETCHAT_H
#define MELEE_IF_IFNETCHAT_H
#include <Runtime/platform.h>
/* Main/presentation thread only, after scene initialization and chat poll.
 * eligible: online CSS/results/lobby. Auto-clears pointers on scene cleanup. */
void ifNetChat_Update(bool eligible);
void ifNetChat_Free(void);
#endif
