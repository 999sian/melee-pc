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

    /* Thunder's own view. On GameCube this was
     * `{ Item_GObj* x0; s32 x4; }`, so gp+00 held the thunder gobj and gp+04
     * the state word. An 8-byte host pointer at +00 pushes x4 to host +08,
     * but x4 is written from OUTSIDE this state -- the thunder ITEM calls
     * ftPk_SpecialLw_SetState_Unk0 on its owner from
     * it_2725_Logic39_Destroyed, and its only guard
     * (ftPk_SpecialLw_CheckProperty) passes whenever the fighter is not
     * captured, i.e. also long after SpecialLw ended and `mv` belongs to
     * another view. Host +08 is where every sibling view keeps gp+08:
     * for a fighter riding a Barrel Cannon that is ftCo_BarrelVars::x8,
     * an Item_GObj*, whose low half the `= 3` then shredded ->
     * it_80295F38(barrel.x8) dereferenced 0x3 on the next frame.
     *
     * So keep gp+00 as a plain 4-byte slot and relocate the pointer to host
     * +08, which makes this view's host footprint identical to barrel's
     * (word, word, 8-byte Item_GObj*) and puts gp+04 back on host +04 like
     * all ~28 siblings. x0 is free to move because it is only ever touched
     * while this view owns `mv`: every reference lives in
     * ftpikachuspeciallw.c (Enter/SpawnEffect write it, 8012765C reads it).
     * Pichu shares this declaration through mv.pc. */
    struct ftPikachu_SpecialLwVars {
        /* +00 gp+00 */ u8 pad_x0[4];
        /* +04 gp+04 */ s32 x4; ///< thunder state: 0, 1 or 3
        /* +08 relocated from gp+00 */ Item_GObj* x0;
    } speciallw;
};

/* One GameCube offset, one host offset.
 *
 * speciallw.x4 is written across state boundaries, so it must land on the
 * same HOST byte as the gp+04 slot of whichever sibling view of
 * union Fighter_MotionVars is live. Pinned against ftPikachu_SpecialHiVars
 * (same union, and Quick Attack is a state SpecialLw is routinely left for)
 * and against ftCommon_MotionVars::barrel (the view whose gp+08 pointer the
 * stray write used to corrupt). Putting an 8-byte pointer back at +00 fails
 * here instead of faulting at the next dereference.
 *
 * x0 is pinned too: at host +08 it coincides exactly with barrel.x8, which
 * is also an 8-byte Item_GObj*, rather than half-overlapping it. */
STATIC_ASSERT(offsetof(struct ftPikachu_SpecialLwVars, x4) ==
              offsetof(struct ftPikachu_SpecialHiVars, x4));
STATIC_ASSERT(offsetof(struct ftPikachu_SpecialLwVars, x4) ==
              offsetof(union ftCommon_MotionVars, barrel.x4));
STATIC_ASSERT(offsetof(struct ftPikachu_SpecialLwVars, x0) ==
              offsetof(union ftCommon_MotionVars, barrel.x8));
STATIC_ASSERT(sizeof(((union ftCommon_MotionVars*) 0)->barrel.x8) ==
              sizeof(((struct ftPikachu_SpecialLwVars*) 0)->x0));

#endif
