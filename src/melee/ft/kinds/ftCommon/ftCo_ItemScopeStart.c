#include "ftCo_Attack100.h"
#include <melee/ft/fighter.h>
#include <melee/ft/ft_081B.h>
#include <melee/ft/ft_0CDD.h>
#include <melee/ft/ftcommon.h>
#include <melee/ft/inlines.h>
#include <melee/it/kinds/itsscope.h>

FtMotionId fn_800D769C(Fighter* ft, FtMotionId msid)
{
    if (ft->motion_id >= ftCo_MS_ItemScopeStartEmpty) {
        return msid + 8;
    }
    return msid;
}

void ft_800D76B8(Fighter_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);

    if (it_8026B594(fp->item_gobj) == false) {
        Fighter_ChangeMotionState(gobj, ftCo_MS_ItemScopeStart, Ft_MF_None,
                                  0.0F, 1.0F, 0.0F, NULL);
    } else {
        Fighter_ChangeMotionState(gobj, ftCo_MS_ItemScopeStartEmpty,
                                  Ft_MF_None, 0.0F, 1.0F, 0.0F, NULL);
    }

    ftAnim_8006EBA4(gobj);
    fp->mv.co.itemscope.timer = 0.0F;
    fp->mv.co.itemscope.flag = 0;

    ftCommon_8007E79C(fp->gobj, 1);
    fp->take_dmg_cb = (HSD_GObjEvent) fn_800D7938;
}

void ft_800D7770(Fighter_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);

    if (it_8026B594(fp->item_gobj) == false) {
        Fighter_ChangeMotionState(gobj, ftCo_MS_ItemScopeAirStart, Ft_MF_None,
                                  0.0F, 1.0F, 0.0F, NULL);
    } else {
        Fighter_ChangeMotionState(gobj, ftCo_MS_ItemScopeAirStartEmpty,
                                  Ft_MF_None, 0.0F, 1.0F, 0.0F, NULL);
    }

    ftAnim_8006EBA4(gobj);
    ftCommon_ClampAirDrift(fp);
    fp->mv.co.itemscope.timer = 0.0F;
    fp->mv.co.itemscope.flag = 0;

    ftCommon_8007E79C(fp->gobj, 1);
    fp->take_dmg_cb = (HSD_GObjEvent) fn_800D7938;
}

void fn_800D7830(Fighter_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);
    FtMotionId msid;

    ftCommon_8007D7FC(fp);
    if (fp->motion_id >= ftCo_MS_ItemScopeStartEmpty) {
        msid = ftCo_MS_ItemScopeStartEmpty;
    } else {
        msid = ftCo_MS_ItemScopeStart;
    }
    Fighter_ChangeMotionState(gobj, msid, 0x0C4C5280, fp->cur_anim_frame,
                              fp->frame_speed_mul, 0.0F, NULL);
    fp->take_dmg_cb = (HSD_GObjEvent) fn_800D7938;
}

void fn_800D78B0(Fighter_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);
    FtMotionId msid;

    ftCommon_8007D5D4(fp);
    if (fp->motion_id >= ftCo_MS_ItemScopeStartEmpty) {
        msid = ftCo_MS_ItemScopeAirStartEmpty;
    } else {
        msid = ftCo_MS_ItemScopeAirStart;
    }
    Fighter_ChangeMotionState(gobj, msid, 0x0C4C5280, fp->cur_anim_frame,
                              fp->frame_speed_mul, 0.0F, NULL);
    ftCommon_ClampAirDrift(fp);
    fp->take_dmg_cb = (HSD_GObjEvent) fn_800D7938;
}

void fn_800D7938(Fighter_GObj* gobj)
{
    Fighter_GObj* temp_r30;
    Fighter* temp_r31;
    s32 temp_r4;

    temp_r30 = gobj;
    temp_r31 = GET_FIGHTER(temp_r30);
    if (temp_r31->item_gobj != NULL) {
        temp_r4 = it_80291DAC(temp_r31->item_gobj,
                              (s32) temp_r31->mv.co.itemscope.timer);
        if (temp_r4 != -1) {
            it_80291F14(temp_r31->item_gobj, temp_r4);
        }
        ftCommon_8007E7E4(temp_r30, 1);
    }
}

void ftCo_ItemScopeStart_Anim(Fighter_GObj* gobj) {}

void ftCo_ItemScopeAirStart_Anim(Fighter_GObj* gobj) {}

void fn_800D79B4(HSD_GObj* gobj, void (*cb_ground)(HSD_GObj*),
                 void (*cb_air)(HSD_GObj*, int))
{
    Fighter* fp = GET_FIGHTER(gobj);
    int item_val;
    s32 stack_var;

    if (fp->item_gobj == NULL) {
        return;
    }

    if (fp->mv.co.itemscope.flag == 0) {
        if (fp->input.held_buttons[0] & HSD_PAD_A) {
            fp->mv.co.itemscope.timer += 1.0F;
        }
    }

    if (!(fp->input.held_buttons[0] & HSD_PAD_A)) {
        fp->mv.co.itemscope.flag = 1;
    }

    // Timer to int conversion
    stack_var = (s32) fp->mv.co.itemscope.timer;

    item_val = it_80291DAC(fp->item_gobj, stack_var);

    // Threshold check (Float vs Float)
    if (fp->mv.co.itemscope.timer >= p_ftCommonData->x5B8) {
        if (!ftAnim_IsFramesRemaining(gobj)) {
            cb_air(gobj, item_val);
        }
    }

    if (fp->mv.co.itemscope.flag == 0) {
        return;
    }

    if (ftAnim_IsFramesRemaining(gobj)) {
        return;
    }

    stack_var = (s32) fp->mv.co.itemscope.timer;

    if (it_80291CF4(fp->item_gobj, stack_var) == 0) {
        cb_ground(gobj);
    } else {
        cb_air(gobj, item_val);
    }
}

void ftCo_ItemScopeStart_IASA(Fighter_GObj* gobj)
{
    fn_800D79B4(gobj, fn_800D7BDC, fn_800D8140);
}

void ftCo_ItemScopeAirStart_IASA(Fighter_GObj* gobj)
{
    fn_800D79B4(gobj, fn_800D7C60, fn_800D81D0);
}

void ftCo_ItemScopeStart_Phys(Fighter_GObj* gobj)
{
    ftCo_800CDE54(gobj);
}

void ftCo_ItemScopeAirStart_Phys(Fighter_GObj* gobj)
{
    ftCo_800CDE74(gobj);
}

void ftCo_ItemScopeStart_Coll(Fighter_GObj* gobj)
{
    ft_800841B8(gobj, fn_800D78B0);
}

void ftCo_ItemScopeAirStart_Coll(Fighter_GObj* gobj)
{
    ft_80082C74(gobj, fn_800D7830);
}
