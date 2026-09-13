#ifndef MELEE_FT_CHARA_FTPIKACHU_TYPES_H
#define MELEE_FT_CHARA_FTPIKACHU_TYPES_H

#include <Runtime/platform.h>

#include <melee/ft/forward.h>
#include <melee/it/forward.h>

#include <dolphin/mtx.h>
#include <melee/ft/kinds/ftCommon/types.h>

struct ftPikachu_FighterVars {
    char filler0[FIGHTERVARS_SIZE];
};

typedef struct DISC_STRUCT _ftPikachuAttributes {
    DiscVec2 specialn_spawn_offset;
    DiscVec2 specialairn_spawn_offset;
    float specialairn_landing_lag;
    ItemKind specialn_itkind;
    ItemKind specialairn_itkind;
    float x1C;
    float x20;
    float x24;
    float x28;
    float x2C;
    float x30;
    float specials_start_friction;
    float specials_start_gravity;
    float x3C;
    float x40;
    float x44;
    float x48;
    float x4C;
    float x50;
    float x54;
    float x58;
    int x5C;
    s32 x60; // up b zip duration
    float x64;
    float x68; // up b angle offset 1
    DiscVec3 x6C_scale;
    float x78; // up b angle offset 2
    DiscVec3 x7C_scale;
    float x88;
    float x8C; // up b minimum stick magnitude

    float x90; // up b zip stick magnitude to velocity slope
    float x94; // up b zip stick magnitude to velocity intercept
    float x98; // second zip velocity decay
    float x9C;

    int xA0;
    float xA4;
    s32 xA8; // minimum stick angle difference between two up b zips
    float xAC;

    float xB0;
    float xB4;
    float xB8;
    float xBC;

    float xC0;
    float xC4;
    float xC8;
    float xCC;

    float xD0;
    s32 xD4;
    s32 xD8;
    u32 xDC;

    ftCollisionBoxDisc height_attributes;
} ftPikachuAttributes;
DISC_ASSERT_SIZE(ftPikachuAttributes, 0xF8);

union ftPikachu_MotionVars {
    /// @todo Proper state name.
    struct ftPikachu_State2Vars {
        s32 x0;
    } unk2;

    /// @todo Proper state name.
    struct ftPikachu_State3Vars {
        s32 x0;
    } unk3;

    struct ftPikachu_SpecialHiVars {
        int x0;
        s32 x4;
        s32 x8;
        int xC;
        Vec2 x10;
        s32 x18;
        Vec2 x1C;
        float x24;
    } specialhi;

    struct ftPikachu_SpecialLwVars {
        /* fp+2340 */ Item_GObj* x0;
        /* fp+2344 */ s32 x4; ///< thunder state: 0, 1 or 3
    } speciallw;
};

#endif
