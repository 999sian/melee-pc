#include "itdraw.h"

#include "inlines.h"
#include "it_2725.h"
#include <melee/cm/camera.h>
#include <melee/ft/ftlib.h>
#include <melee/lb/lb_0146.h>
#include <melee/lb/lbcollision.h>
#include <melee/lb/lbgx.h>
#include <sysdolphin/baselib/tev.h>

U8Vec4 it_804D5168 = { 0xFF, 0x40, 0x80, 0x80 };

void it_8026EB18(HSD_GObj* gobj, s32 arg1, Vec3* arg2)
{
    Mtx m2;
    MtxPtr mptr;
    if (arg2 != NULL) {
        HSD_CObj* cobj = HSD_CObjGetCurrent();
        MtxPtr vmtx = (MtxPtr) &cobj->view_mtx;
        { ///< @todo This appears in several places in the codebase,
            // it's probably an inline
            Mtx m;
            PAD_STACK(4);
            MTXIdentity((MtxPtr) &m);
            m[0][3] = arg2->x;
            m[1][3] = arg2->y;
            m[2][3] = arg2->z;
            MTXConcat(vmtx, (MtxPtr) &m, (MtxPtr) &m2);
            mptr = (MtxPtr) &m2;
        }
    } else {
        mptr = NULL;
    }
    HSD_JObjDispAll(GET_JOBJ(gobj), mptr, HSD_GObj_80390EB8(arg1), 0);
}

void it_8026EBC8(HSD_GObj* gobj, u16 arg1, u8* arg2)
{
    Item* ip = GET_ITEM(gobj);
    u16 cnt = 0;
    HSD_JObj* jobj;
    HSD_JObj* jobj_parent;
    u8* index = arg2;
    while (cnt < arg1) {
        jobj = ip->xBBC_dynamicBoneTable->bones[*index];
        jobj_parent = HSD_JObjGetParent(jobj);
        if (!(HSD_JObjGetFlags(jobj_parent) & 0x10)) {
            it_80272A18(jobj);
        }
        cnt++;
        index++;
    }
}

void it_8026EC54(HSD_GObj* gobj, u16 arg1, u8* arg2)
{
    Item* ip = GET_ITEM(gobj);
    u16 cnt = 0;
    HSD_JObj* jobj;
    HSD_JObj* jobj_parent;
    u8* index = arg2;
    while (cnt < arg1) {
        jobj = ip->xBBC_dynamicBoneTable->bones[*index];
        jobj_parent = HSD_JObjGetParent(jobj);
        if (!(HSD_JObjGetFlags(jobj_parent) & 0x10)) {
            it_80272A3C(jobj);
        }
        cnt++;
        index++;
    }
}

/// arg1 is some kind of enum type? It gets passed to functions that check if
/// it's 0 or 2 to run
u32 it_8026ECE0(Item_GObj* gobj, u32 arg1)
{
    Item* ip;
    u32 ret;
    u32 idx;

    ret = 0;
    ip = GET_ITEM(gobj);
    if (ip->kind == It_Kind_Unk4) {
        /* The coin's item vars are owned by it_2E5A_ItemVars, which stores a
         * host pointer inside its `sub` record; the old spelling read them
         * back through it_266F_ItemVars, whose flag byte and collision
         * record only sit at the same offsets when pointers are 4 bytes.
         * Read through the owning view and hand the collider a copy in its
         * own layout (it only reads x0/x8/x14). */
        it_2E5A_SubVars* sub = &ip->xDD4_itemVar.it_2E5A.sub;
        struct lbColl_8000A10C_arg0_t sphere;
        sphere.x0 = sub->x0;
        sphere.x4 = 0.0f;
        sphere.x8 = sub->x8;
        sphere.x14 = sub->x14;
        if (ip->xDAA_flag.b0 &&
            (ip->xDD4_itemVar.it_2E5A.x18.b0 ||
             ip->xDD4_itemVar.it_2E5A.x18.b1) &&
            lbColl_8000A10C(&sphere, arg1, ip->scl))
        {
            ret = 1;
        }
    } else {
        if (ip->xDAA_flag.b6) {
            if (ip->xDAA_flag.b2) {
                idx = 0U;
                while (idx < 4U) {
                    if (lbColl_80009F54(&ip->x5D4_hitboxes[idx].hit, arg1,
                                        ip->scl) != false)
                    {
                        ret = 1;
                    }
                    idx++;
                }
            }
            if (!ip->xDC8_word.flags.x13 && ip->xDAA_flag.b1) {
                if (ip->xD0C == 0) {
                    idx = 0U;
                    while (idx < ip->xAC8_hurtboxNum) {
                        if (lbColl_8000A244(&ip->xACC_itemHurtbox[idx], arg1,
                                            NULL, 0.0f) != false)
                        {
                            ret = 1;
                        }
                        idx++;
                    }
                } else {
                    idx = 0U;
                    while (idx < ip->xAC8_hurtboxNum) {
                        if (lbColl_8000A584(&ip->xACC_itemHurtbox[idx],
                                            ip->xD0C, arg1, NULL,
                                            0.0f) != false)
                        {
                            ret = 1;
                        }
                        idx++;
                    }
                }
            }
        }
        if (ip->xDAA_flag.b4 && ip->xDC8_word.flags.x15 &&
            (lbGx_8001E2F8((Vec4*) &ip->xBCC_unk, &ip->pos, &it_804D5168, arg1,
                           ip->facing_dir) != false))
        {
            ret = 1;
        }
        if (ip->xDAA_flag.b3 && ip->xDD0_flag.b0 &&
            (lb_800149E0(&ip->xB54, arg1) != false))
        {
            ret = 1;
        }
    }
    return ret;
}

/* The two "hide these bones" lists it_8026EECC walks live in the article's
 * special attributes inside the .dat: it_8027CE64 relocates that slot and
 * parks the host pointer in the item-vars union (itGamewatch_ItemVars::attr),
 * and this is the only reader. The old spelling read it back as a native
 * `it_266F_ItemVars*`, which assumes the two index pointers sit at +4 and +C
 * with 4-byte pointers and that the counts are host-endian. Both are false
 * here: describe the record as on-disc so the counts get swapped and the
 * slots get relocated. */
struct DISC_STRUCT it_8026EECC_BoneLists {
    /* +0 */ u16 n0;
    /* +4 */ DISC_PTR(u8) idx0;
    /* +8 */ u16 n1;
    /* +C */ DISC_PTR(u8) idx1;
};
DISC_ASSERT_SIZE(struct it_8026EECC_BoneLists, 0x10);

#define it_8026EECC_VARS(ip)                                                  \
    ((struct it_8026EECC_BoneLists*) (ip)->xDD4_itemVar.gamewatch.attr)

static inline void it_8026EECC_inline_1(HSD_GObj* gobj, s32 arg1, Vec3* pos)
{
    Item* ip = GET_ITEM(gobj);

    ip->xDCF_flag.b4 = 1;
    ip->xDCF_flag.b5 = 0;
    it_8026EC54(gobj, it_8026EECC_VARS(ip)->n0,
                DP(u8, it_8026EECC_VARS(ip)->idx0));
    it_8026EBC8(gobj, it_8026EECC_VARS(ip)->n1,
                DP(u8, it_8026EECC_VARS(ip)->idx1));
    it_8026EB18(gobj, arg1, ip->xDCF_flag.b7 ? pos : NULL);
    it_8026EBC8(gobj, it_8026EECC_VARS(ip)->n0,
                DP(u8, it_8026EECC_VARS(ip)->idx0));
    it_8026EC54(gobj, it_8026EECC_VARS(ip)->n1,
                DP(u8, it_8026EECC_VARS(ip)->idx1));
}

static inline void it_8026EECC_inline_2(HSD_GObj* gobj, s32 arg1, Vec3* pos)
{
    Item* ip = GET_ITEM(gobj);

    ip->xDCF_flag.b4 = 0;
    ip->xDCF_flag.b5 = 0;
    it_8026EB18(gobj, arg1, ip->xDCF_flag.b7 ? pos : NULL);
}

static inline void it_8026EECC_inline_3(HSD_GObj* gobj, s32 arg1, Vec3* pos)
{
    Item* ip = GET_ITEM(gobj);

    ip->xDCF_flag.b4 = 1;
    ip->xDCF_flag.b5 = 1;
    it_8026EC54(gobj, it_8026EECC_VARS(ip)->n0,
                DP(u8, it_8026EECC_VARS(ip)->idx0));
    it_8026EBC8(gobj, it_8026EECC_VARS(ip)->n1,
                DP(u8, it_8026EECC_VARS(ip)->idx1));
    it_8026EB18(gobj, arg1, ip->xDCF_flag.b7 ? pos : NULL);
    it_8026EBC8(gobj, it_8026EECC_VARS(ip)->n0,
                DP(u8, it_8026EECC_VARS(ip)->idx0));
    it_8026EC54(gobj, it_8026EECC_VARS(ip)->n1,
                DP(u8, it_8026EECC_VARS(ip)->idx1));
}

static inline Item* it_8026EECC_inline_0(HSD_GObj* gobj, Vec3* pos)
{
    Item* ip = GET_ITEM(gobj);
    ip->xDCF_flag.b7 = 0;
    if ((ip->owner != NULL) && ftLib_80086960(ip->owner)) {
        if (ftLib_80087074(ip->owner, pos)) {
            ip->xDCF_flag.b7 = 1;
        }
    } else {
        pos->x = pos->y = pos->z = 0.0F;
    }
    return GET_ITEM(gobj);
}

static inline void it_8026EECC_inline_sw(HSD_GObj* gobj, s32 arg1, Vec3* pos)
{
    Item* ip = gobj->user_data;
    switch (Camera_80031060()) {
    case 1:
        if (ip->xDCF_flag.b3) {
            it_8026EECC_inline_1(gobj, arg1, pos);
            it_8026EECC_inline_2(gobj, arg1, pos);
            it_8026EECC_inline_3(gobj, arg1, pos);
        }
        break;
    case 0:
        if (!ip->xDCF_flag.b3) {
            it_8026EECC_inline_2(gobj, arg1, pos);
        }
        break;
    }
}

void it_8026EECC(HSD_GObj* gobj, int arg1)
{
    Item* ip = GET_ITEM(gobj);
    Vec3 pos;

    if (ip->xDAA_flag.b7) {
        pos.x = pos.y = pos.z = 0.0F;
        if (ip->xDC8_word.flags.x13) {
            if ((ip->owner == NULL) || !ftLib_80086960(ip->owner) ||
                ftLib_800868D4(ip->owner, gobj))
            {
                ip = it_8026EECC_inline_0(gobj, &pos);
                it_8026EECC_inline_sw(gobj, arg1, &pos);
            }
        } else {
            it_8026EECC_inline_sw(gobj, arg1, &pos);
        }
    }
    if (it_8026ECE0((Item_GObj*) gobj, arg1) != 0U) {
        HSD_StateInvalidate(-1);
        HSD_StateInitTev();
        HSD_ClearVtxDesc();
    }
}
