#ifndef MELEE_GM_1BF9_H
#define MELEE_GM_1BF9_H

#ifdef TARGET_PC
#include <dolphin/types.h>

/// MELEE_BOOT_SCENE: the GameModeKind to boot straight into, or GM_COUNT when
/// the knob is unset. Cached, so this is cheap to call from an on_load hook.
u8 pc_boot_scene(void);
#endif

#endif
