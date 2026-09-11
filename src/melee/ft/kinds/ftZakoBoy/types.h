#ifndef MELEE_FT_CHARA_FTZAKOBOY_TYPES_H
#define MELEE_FT_CHARA_FTZAKOBOY_TYPES_H

#include <Runtime/platform.h>

#include <melee/ft/forward.h>

struct ftZakoBoy_FighterVars {
    char filler0[FIGHTERVARS_SIZE];
};

typedef struct DISC_STRUCT _ftZakoboyAttributes {
    s32 x0;
} ftZakoboyAttributes;
DISC_ASSERT_SIZE(ftZakoboyAttributes, 0x4);

#endif
