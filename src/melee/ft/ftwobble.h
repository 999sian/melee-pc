#ifndef MELEE_FT_FTWOBBLE_H
#define MELEE_FT_FTWOBBLE_H

#include <Runtime/platform.h>

#include <melee/ft/forward.h>

#ifdef TARGET_PC
/// Slippi Online's Disable Wobbling rule (see ftwobble.c).
void ftWobble_Reset(Fighter* fp);
bool ftWobble_Check(Fighter_GObj* gobj);
#endif

#endif
