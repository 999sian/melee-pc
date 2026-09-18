#ifndef MELEE_FT_CHARA_FTPOPO_TYPES_H
#define MELEE_FT_CHARA_FTPOPO_TYPES_H

#include <Runtime/platform.h>

#include <melee/it/forward.h>
#include <melee/ft/kinds/ftCommon/types.h>

struct ftPopo_FighterVars {
    /* 0x222C */ Item_GObj* x222C;
    /* 0x2230:0 */ u8 x2230_b0 : 1;
    /* 0x2231 */ u8 filler_x2231[3];
    /* 0x2234 */ u32 x2234;
    /* 0x2238 */ Item_GObj* x2238;
    /* 0x223C */ u32 x223C;
    /* 0x2240 */ Vec x2240;
    /* 0x224C */ u32 x224C;
    /* 0x2250 */ float x2250;
};

typedef struct DISC_STRUCT ftIceClimberAttributes {
    float x0;
    float x4;
    float x8;
    float xC;
    float x10;
    float x14;
    float x18;
    int x1C;
    float x20;
    float x24;
    float x28;
    float x2C;
    float x30;
    float x34;
    float x38;
    float x3C;
    float x40;
    float x44;
    float x48;
    float x4C_gravity;
    float x50_gravity;
    float x54_terminal_vel;
    float x58_terminal_vel;
    float x5C;
    float x60;
    float x64;
    int x68;
    float x6C;
    float x70;
    float x74;
    float x78;
    float x7C;
    float x80;
    float x84;
    float x88;
    float x8C;
    float x90;
    float x94;
    float x98;
    float x9C;
    float xA0;
    float xA4;
    float xA8;
    float xAC;
    float xB0;
    float xB4;
    float xB8;
    float xBC;
    float xC0;
    float xC4;
    float xC8;
    u8 _CC[0xD0 - 0xCC];
    float xD0;
    u8 _D4[0x12C - 0xD4];
    float x12C;
    float x130;
    float x134;
    float x138;
    float x13C;
    float x140;
    float x144;
    float x148;
    float x14C;
    u8 _150[0x15C - 0x150];
} ftIceClimberAttributes;
ASSERT_SIZE(ftIceClimberAttributes, 0x15C);
DISC_ASSERT_SIZE(ftIceClimberAttributes, 0x15C);

union ftPp_MotionVars {
    /* Squall Hammer's view. On GameCube this was 32 bytes (8 words) with
     * an effect/GObj pointer at gp+08. On 64-bit LP64, an 8-byte host
     * pointer at +08 pushes xC..x1C by 4 bytes (xC to host +10, x1C to +20),
     * corrupting sibling union views (e.g. capturedamage, damage, etc).
     *
     * Keep gp+08 as a plain 4-byte slot and relocate the pointer past x1C
     * (host +20), which puts xC, x10, x14, x18, x1C back at their GameCube
     * offsets (gp+0C, gp+10, gp+14, gp+18, gp+1C).
     *
     * // ponytail: relocate pointer past scalar block to preserve GameCube layout */
    struct ftPp_SpecialSVars {
        /* +00 gp+00 */ float x0;
        /* +04 gp+04 */ int x4;
        /* +08 gp+08 */ u8 pad_x8[4];
        /* +0C gp+0C */ int xC;
        /* +10 gp+10 */ int x10;
        /* +14 gp+14 */ int x14;
        /* +18 gp+18 */ int x18;
        /* +1C gp+1C */ float x1C;
        /* +20 relocated from gp+08 */
        struct ftPp_SpecialSVars_x8_t {
            int x0;
            HSD_GObj* x4;
        }* x8;
    } specials;
    struct {
        /* fp+2340 */ int x0;
    } unk_80123954;
    struct {
        /* fp+2340 */ int x0;
        /* fp+2344:0 */ u8 x4_b0 : 1;
    } speciallw;
};

STATIC_ASSERT(offsetof(struct ftPp_SpecialSVars, x0) == 0x00);
STATIC_ASSERT(offsetof(struct ftPp_SpecialSVars, x4) == 0x04);
STATIC_ASSERT(offsetof(struct ftPp_SpecialSVars, xC) == 0x0C);
STATIC_ASSERT(offsetof(struct ftPp_SpecialSVars, x10) == 0x10);
STATIC_ASSERT(offsetof(struct ftPp_SpecialSVars, x14) == 0x14);
STATIC_ASSERT(offsetof(struct ftPp_SpecialSVars, x18) == 0x18);
STATIC_ASSERT(offsetof(struct ftPp_SpecialSVars, x1C) == 0x1C);
STATIC_ASSERT(offsetof(struct ftPp_SpecialSVars, x8) == 0x20);
STATIC_ASSERT(offsetof(struct ftPp_SpecialSVars, x10) ==
              offsetof(union ftCommon_MotionVars, fighterthrow.self_vel_y));
STATIC_ASSERT(offsetof(struct ftPp_SpecialSVars, x14) ==
              offsetof(union ftCommon_MotionVars, fighterthrow.self_vel_x));
STATIC_ASSERT(offsetof(struct ftPp_SpecialSVars, x18) ==
              offsetof(union ftCommon_MotionVars, capturedamage.x18));

#endif
