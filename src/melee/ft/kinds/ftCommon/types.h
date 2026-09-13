#ifndef MELEE_FT_CHARA_FTCOMMON_TYPES_H
#define MELEE_FT_CHARA_FTCOMMON_TYPES_H

#include <Runtime/platform.h>

#include <melee/ft/forward.h>
#include <melee/ft/kinds/ftCommon/forward.h> // IWYU pragma: export
#include <melee/it/forward.h>
#include <melee/lb/forward.h>

#include <placeholder.h>

#include <dolphin/mtx.h>
#include <sysdolphin/baselib/controller.h>

struct ftCollisionBox {
    /*  +0 */ float top;
    /*  +4 */ float bottom;
    /*  +8 */ Vec2 left;
    /* +10 */ Vec2 right;
};

/// Disc-layout twin of ftCollisionBox, embedded in per-character attr blocks.
struct DISC_STRUCT ftCollisionBoxDisc {
    /*  +0 */ float top;
    /*  +4 */ float bottom;
    /*  +8 */ DiscVec2 left;
    /* +10 */ DiscVec2 right;
};
DISC_ASSERT_SIZE(struct ftCollisionBoxDisc, 0x18);

/// Copy a disc-layout height box into a native ftCollisionBox for the
/// ft_8008xxxx(ftCollisionBox*) helpers.
static inline void ftCollisionBox_FromDisc(ftCollisionBox* dst,
                                           const ftCollisionBoxDisc* src)
{
    dst->top = src->top;
    dst->bottom = src->bottom;
    dst->left.x = src->left.x;
    dst->left.y = src->left.y;
    dst->right.x = src->right.x;
    dst->right.y = src->right.y;
}

/// Scale every field of a disc-layout height box in place.
static inline void ftCollisionBoxDisc_Scale(ftCollisionBoxDisc* box, float s)
{
    box->top *= s;
    box->bottom *= s;
    box->left.x *= s;
    box->left.y *= s;
    box->right.x *= s;
    box->right.y *= s;
}

/// On disc (ftData::x30) and also built on the stack; same layout either way.
struct DISC_STRUCT ftHurtboxInit {
    Fighter_Part bone_idx;
    HurtHeight height;
    u32 is_grabbable;
    DiscVec3 a_offset;
    DiscVec3 b_offset;
    float scale;
};
DISC_ASSERT_SIZE(struct ftHurtboxInit, 0x28);

union ftCommon_MotionVars {
    struct {
        /* fp+2340 */ int x0;
        /* fp+2344 */ Vec3 x4;
        /* fp+2350 */ u32 x10;
        /* fp+2354 */ float x14;
        /* fp+2358 */ float x18;
        /* fp+235C */ float x1C;
        /* fp+2360 */ float x20;
        /* fp+2364 */ float x24;
        /* fp+2368 */ Vec3 x28;
        /* fp+2374 */ Vec3 x34;
        /* fp+2380 */ Vec3 x40;
        /* fp+238C */ Vec3 x4C;
        /* fp+2398 */ Vec3 x58;
    } common;
    struct {
        /* fp+2340 */ float x0;
        /* fp+2344 */ FtMotionId msid;
        /* fp+2348 */ float slow_anim_frame;
        /* fp+234C */ float middle_anim_frame;
        /* fp+2350 */ float fast_anim_frame;
        /* fp+2354 */ float slow_anim_rate;
        /* fp+2358 */ float middle_anim_rate;
        /* fp+235C */ float fast_anim_rate;
        /* fp+2360 */ float accel_mul;
    } walk;
    struct {
        /* fp+2340 */ bool has_turned;
        /* fp+2344 */ float facing_after;
        /* fp+2348 */ float x8;
        /* fp+234C */ u8 pad_xC[4];
        /* fp+2350 */ float frames_to_turn;
        /* fp+2354 */ u8 pad_x14[4];
        /* fp+2358 */ bool just_turned;
        /* fp+235C */ HSD_Pad x1C;

    } turn;
    struct {
        /* fp+2340 */ u8 pad_x0[12];
        /* fp+234C */ float accel_mul;
        /* fp+2350 */ u8 pad_x10[4];
        /* fp+2354 */ int x14;
    } turnrun;
    struct {
        /* fp+2340 */ float x0;
        /* fp+2344 */ int x4;
    } dash;
    struct {
        /* fp+2340 */ float x0;
        /* fp+2344 */ float x4;
    } run;
    struct {
        /* fp+2340 */ bool x0;
        /* fp+2344 */ float frames;
    } runbrake;
    struct {
        /* fp+2340 */ int is_short_hop;
        /* fp+2344 */ ftCo_JumpInput jump_input;
    } kneebend;
    struct {
        /* fp+2340 */ int x0;
        /* fp+2344 */ bool x4;
        /* fp+2348 */ float jump_mul;
    } jump;
    struct {
        /* fp+2340 */ int x0;
        /* fp+2344 */ float init_h_vel;
    } jumpaerial;
    struct {
        /* fp+2340 */ FtMotionId smid;
        /* fp+2344 */ float x4;
    } fall;
    struct {
        /* fp+2340 */ FtMotionId smid;
        /* fp+2344 */ float x4;
    } fallaerial;
    struct {
        /* fp+2340 */ int x0;
        /* fp+2344 */ float x4;
    } squat;
    struct {
        /* fp+2340 */ bool allow_interrupt;
    } landing;
    struct {
        /* fp+2340 */ bool x0;
    } attack1;
    struct {
        /* fp+2340 */ bool x0;
        /* fp+2344 */ bool x4;
    } attack100;
    struct {
        /* fp+2340 */ int x0;
    } attackdash;
    struct {
        /* fp+2340 */ bool x0;
    } attacklw3;
    struct {
        /* fp+2340 */ float x0;
        /* fp+2344 */ int x4;
        /* fp+2348 */ int x8;
        /* fp+234C */ u32 xC;
        /* fp+2350 */ u32 x10;
        /* fp+2354 */ float x14;
        /* fp+2358 */ u8 x18;
        /* fp+2359 */ u8 x19;
        /* fp+235A */ u8 x1A;
        /* fp+235B */ u8 x1B;
    } damage;
    struct {
        /* fp+2340 */ u8 wall_hit_dir;
        /* fp+2344 */ float rot_speed;
        /* fp+2348 */ ftCollisionBox ice_coll;
    } damageice;
    struct {
        /* fp+2340 */ float escape_timer;
    } damageicejump;
    struct {
        /* fp+2340 */ float x0;
        /* fp+2344 */ float x4;
        /* fp+2348 */ float x8;
        /* fp+234C */ bool xC;
        /* fp+2350 */ float x10;
        /* fp+2354 */ float x14;
        /* fp+2358 */ float x18;
        /* fp+235C */ int x1C;
        /* fp+2360 */ int x20;
        /* fp+2364 */ int x24;
        /* fp+2368 */ u32 x28;
        /* fp+236C */ float x2C;
    } guard;
    struct {
        /* fp+2340 */ bool x0; // itemget action is heavy type?
    } itemget;
    struct {
        /* self_vel_y/self_vel_x are the same words as `throw`'s
         * @c xC.y / @c xC.z (ftCo_Throw.c writes one view and reads the
         * other), so they must stay at fp+2350 / fp+2354. GameCube kept the
         * thrower gobj at fp+234C, where a 4-byte pointer only overlapped
         * @c throw::xC.x ; an 8-byte one would eat both velocities and
         * anywhere else in the union is written by the damage view that runs
         * between the store and the read, so it lives in
         * Fighter::throw_thrower instead. */
        /* fp+2340 */ u32 x0;
        /* fp+2344 */ int x4;
        /* fp+2348 */ float x8;
        /* fp+234C */ u8 pad_xC[4];
        /* fp+2350 */ float self_vel_y;
        /* fp+2354 */ float self_vel_x;
    } fighterthrow;
    struct {
        /* fp+2340 */ float facing_dir;
        /* fp+2344 */ float x4;
        /* fp+2348 */ int x8;
        /* fp+234C */ int xC;
        /* fp+2350 */ float x10;
        /* fp+2354 */ int x14;
        /* fp+2358 */ u32 x18;
        /* fp+235C */ u32 x1C;
        /* fp+2360 */ int x20;
    } itemthrow;
    struct {
        /* fp+2340 */ int unk_timer;
        /* fp+2344 */ float anim_spd;
        /* fp+2348 */ Vec3 x8;
    } itemthrow4;
    struct {
        /* fp+2340 */ int x0;
        /* fp+2344 */ float x4;
        /* fp+2348 */ float mobility;
        /* fp+234C */ int xC;
        /* fp+2350 */ bool x10;
        /* fp+2354 */ float landing_lag;
        /* fp+2358 */ bool allow_interrupt;
    } fallspecial;
    struct {
        /* fp+2340 */ bool x0;
        /* fp+2344 */ float x4;
        /* fp+2348 */ bool x8;
    } lift;
    struct {
        /* fp+2340 */ float x0;
    } downwait;
    struct {
        /* fp+2340 */ u8 pad_x0[4];
        /* fp+2344 */ u8 x4;
    } downspot;
    struct {
        /* fp+2340 */ float x0;
    } catch;
    struct {
        /* fp+2340 */ bool x0;
        /* fp+2344 */ bool x4;
    } escape;
    struct {
        /* fp+2340 */ int timer;
        /* fp+2344 */ Vec3 self_vel;
    } escapeair;
    struct {
        /* fp+2340 */ float x0;
        /* fp+2344 */ float anim_speed;
    } rebound;
    struct {
        /* fp+2340 */ u8 pad_x0[4];
        /* fp+2344 */ u8 x4;
    } downreflect;
    struct {
        /* fp+2340 */ bool x0;
        /* fp+2344 */ float x4;
    } pass;
    struct {
        /**
         * Used to check against opponent's #Fighter::213C while they're
         * occupying a ledge within grab range
         */
        /* fp+2340 */ int ledge_id;
        /* fp+2344 */ float x4;
        /* fp+2348 */ bool x8;
    } cliff;
    struct {
        /* fp+2340 */ bool x0;
    } cliffjump;
    struct {
        /* fp+2340 */ bool x0;
    } cargoturn;
    struct {
        /* fp+2340 */ int x0;
        /* fp+2344 */ int x4;
        /* fp+2348 */ float x8;
    } cargokneebend;
    struct {
        /* fp+2340 */ float x0;
        /* fp+2344 */ int x4;
    } shouldered;
    struct {
        /* fp+2340 */ float x0;
    } downdamage;
    struct {
        /* x4..scale are written through the `walk`, `common` and `ca.specialhi`
         * views (ftCo_YoshiEgg.c, ftkirbyyoshiegg.c) and read back here, so
         * they must keep their GameCube offsets. `x0` is write-only, so park
         * the 8-byte pointer past `scale` instead of at fp+2340. */
        /* fp+2340 */ u8 pad_x0[4];
        /* fp+2344 */ bool x4;
        /* fp+2348 */ float x8;
        /* fp+234C */ float xC;
        /* fp+2350 */ float x10;
        /* fp+2354 */ float x14;
        /* fp+2358 */ Vec3 x18;
        /* fp+2364 */ Vec3 scale;
        Fighter_GObj* x0; ///< write-only; moved off fp+2340 (see above)
    } yoshiegg;
    struct {
        /* fp+2340 */ bool x0;
        /* fp+2344 */ u32 x4;
        /* fp+2348 */ float x8;
        /* fp+234C */ float xC;
        /* fp+2350 */ float x10;
    } capturekoopa;
    struct {
        /* fp+2340 */ Vec2 pos_offset;
        /* fp+2348 */ Vec2 x8;
        /* fp+2350 */ Vec2 x10;
        /* fp+2358 */ bool x18;
        /* fp+235C */ u32 x1C;
        /* fp+2360 */ u32 x20;
        /* fp+2364 */ u32 x24;
        /* fp+2368 */ u32 x28;
        /* fp+236C */ Vec3 scale;
    } capturekirby;
    struct {
        /* x4/x8 are also read as `ca.specialhi.vel.x` / `.vel.y`
         * (ftCo_ThrownKirby.c:171,278), so every member must keep its
         * GameCube offset. `thrower_gobj` is write-only, so park the 8-byte
         * pointer past `coll_box` instead of at fp+2340. */
        /* fp+2340 */ u8 pad_x0[4];
        /* fp+2344 */ float x4;
        /* fp+2348 */ float x8;
        /* fp+234C */ float xC;
        /* fp+2350 */ float x10;
        /* fp+2354 */ bool x14;
        union {
            u8 x18;
            struct {
                /* fp+2358:0 */ u8 x18_b0 : 1;
                /* fp+2358:1 */ u8 x18_b1 : 1;
                /* fp+2358:2 */ u8 x18_b2 : 1;
                /* fp+2358:3 */ u8 x18_b3 : 1;
                /* fp+2358:4 */ u8 x18_b4 : 1;
                /* fp+2358:5 */ u8 x18_b5 : 1;
                /* fp+2358:6 */ u8 x18_b6 : 1;
                /* fp+2358:7 */ u8 x18_b7 : 1;
            };
        };
        /* fp+235C */ Vec3 scale;
        /* fp+2368 */ ftCollisionBox coll_box;
        Fighter_GObj* thrower_gobj; ///< write-only; moved off fp+2340
    } thrownkirby;
    struct {
        /* fp+2340 */ int x0;
        /* fp+2344 */ ftCollisionBox coll_box;
        /* fp+235C */ float x1C;
        /* fp+2360 */ enum_t x20;
        /* fp+2364 */ Vec3 translate;
    } bury;
    struct {
        /* fp+2340 */ float x0;
    } buryjump;
    struct {
        /* fp+2340 */ int timer;
        /* fp+2344 */ int x4;
        /* fp+2348 */ bool x8;
        /* fp+234C */ int vel_y_exponent;
    } passivewall;
    struct {
        /* fp+2340 */ int x0;
        /* fp+2344 */ float x4;
    } aircatchhit;
    struct {
        /* fp+2340 */ float x0;
    } aircatch;
    struct {
        /* fp+2340 */ Vec3 cur_pos;
        /* fp+234C */ Vec3 self_vel;
        /* fp+2358 */ float facing_dir;
        /* fp+235C */ int x1C;
        /* fp+2360 */ ftCollisionBox ecb;
    } warpstar;
    struct {
        /* fp+2340 */ int x0;
        /* fp+2344 */ int x4;
        /* fp+2348 */ float x8;
    } hammerkneebend;
    struct {
        /* fp+2340 */ u32 x0;
        /* fp+2344 */ float x4;
    } hammerlanding;
    struct {
        /* fp+2340 */ Item_GObj* x0;
    } captureleadead;
    struct {
        /* fp+2340 */ int x0;
        /* fp+2344 */ float x4;
        /* fp+2348 */ u8 pad_x8[0x18 - 0x8];
        /* fp+2358 */ HSD_JObj* x18;
    } capturedamage;
    struct {
        /* fp+2340 */ s32 timer; ///< frame timer; `bool` truncated it to 0/1
        /* fp+2344 */ float x4;
        /* fp+2348 */ Vec3 x8;
        /* fp+2354 */ Vec3 x14;
        /* fp+2360 */ float x20;
        /* fp+2364 */ float x24;
        /* fp+2368 */ float x28;
        /* fp+236C */ ftCollisionBox x2C;
    } entry;
    struct {
        /* fp+2340 */ Item_GObj* x0;
        /* fp+2344 */ int x4;
    } capturelikelike;
    struct {
        /* GameCube had these three words at fp+2340..2348, which 8-byte
         * function pointers cannot reproduce. The Kinoko states hold the
         * `walk` view's anim frames and the `common` view's saved velocities
         * at the same time, so place these after both (`common` is the
         * largest, ending at 0x68) instead of letting them overlap. */
        u8 pad_0[0x68];
        HSD_GObjEvent x0;
        HSD_GObjEvent x4;
        int x8;
    } mushroom;
    struct {
        /* fp+2340 */ int x0;
        /* fp+2344 */ int x4;
        /* fp+2348 */ Item_GObj* x8;
    } barrel;
    struct {
        /* fp+2340 */ u8 pad_x0[0x6c - 0x40];
        /* fp+236C */ int x6C;
        /* fp+2370 */ int x70;
    } unk_800D331C;
    struct {
        /* fp+2340 */ u8 pad_x0[0x6c - 0x40];
        /* fp+236C */ int x6C;
        /* fp+2370 */ int x70;
    } unk_800D34E0;
    struct {
        /* x40/x74 are plain 4-byte words on disc+GameCube (a frame counter
         * shared with #unk_deadleft); typing them `void*` widened them and
         * shifted x6C/x70 out of alignment with #unk_800D331C. */
        /* fp+2340 */ int x40;
        /* fp+2344 */ u8 pad_x44[0x6c - 0x44];
        /* fp+236C */ int x6C;
        /* fp+2370 */ int x70;
        /* fp+2374 */ int x74;
    } unk_800D3680;
    struct {
        /* fp+2340 */ int x40;
    } unk_deadleft;
    struct {
        /* fp+2340 */ int x40;
        /* fp+2344 */ int x44;
        /* fp+2348 */ u8 pad_x48[0x4C - 0x48];
        /* fp+234C */ float x4C;
        /* fp+2350 */ Vec3 x50;
        /* fp+235C */ Vec3 x5C;
        /* fp+2368 */ int x68;
    } unk_deadup;
    struct {
        /* fp+2340 */ bool unk_bool;
        /* fp+2344 */ float anim_timer;
        /* fp+2348 */ u32 x8;
        /* fp+234C */ u8 xC;
    } thrown;
    struct {
        /* fp+2340 */ FtMotionId prev_msid;
    } parasol_open;
    struct {
        /* fp+2340 */ int x0;
        /* fp+2344 */ int x4;
        /* fp+2344 */ float x8;
    } swing;
    struct {
        /* fp+2340 */ int x0;
        /* fp+2344 */ int x4;
        /* fp+2348 */ int x8;
        /* fp+234C */ Vec xC;
    } throw;
    struct {
        /* fp+2340 */ float x0;
        /* fp+2344 */ float x4;
        /* fp+2348 */ int x8;
        /* fp+234C */ u8 xC;
    } capturewait;
    struct {
        /* fp+2340 */ float timer;
        /* fp+2344 */ int flag;
    } itemscope;
};

/// @todo Fake, need to find real size of #HitCapsule
struct SmallerHitCapsule {
    /*  +0 */ HitCapsuleState state;
    /*  +4 */ u32 x4;
    /*  +8 */ u32 unk_count;
    /*  +C */ float damage;
    /* +10 */ Vec3 b_offset;
    /* +1C */ float scale;
    /* +20 */ int kb_angle;
    /* +24 */ u32 x24;
    /* +28 */ u32 x28;
    /* +2C */ u32 x2C;
    /* +30 */ u32 element;
    /* +34 */ char pad_34[0xFC];
};

/// Read in place from the tether article's Article::x4_specialAttributes disc
/// slot (Link/Y.Link hookshot, Samus grapple beam) — big-endian on disc.
struct DISC_STRUCT TetherAttributes {
    char pad_0[0x38];
    /* +38 */ float pos_x_0;
    /* +3C */ float x3C;
    /* +40 */ float pos_x_1;
};
DISC_ASSERT_SIZE(struct TetherAttributes, 0x44);

#endif
