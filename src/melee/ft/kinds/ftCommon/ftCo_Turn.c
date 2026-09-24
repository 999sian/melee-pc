#include "ftCo_Turn.h"

#include <melee/ft/forward.h>

#include <placeholder.h>

#include "forward.h"
#include "ftCo_AppealS.h"
#include "ftCo_Attack1.h"
#include "ftCo_Attack100.h"
#include "ftCo_AttackHi3.h"
#include "ftCo_AttackHi4.h"
#include "ftCo_AttackLw3.h"
#include "ftCo_AttackLw4.h"
#include "ftCo_AttackS3.h"
#include "ftCo_AttackS4.h"
#include "ftCo_Dash.h"
#include "ftCo_Guard.h"
#include "ftCo_Jump.h"
#include "ftCo_SpecialS.h"
#include <melee/ft/fighter.h>
#include <melee/ft/ft_081B.h>
#include <melee/ft/ft_084E.h>
#include <melee/ft/ft_0892.h>
#include <melee/ft/inlines.h>
#include <melee/ft/types.h>
#include <melee/pl/player.h>
#include <pc/pc.h>

s8 ftCo_ucf_raw_x[4][3];

/// UCF 0.8x dashback (AltimorTASDK/ucf src/dashback/dashback.cpp +
/// include/ucf/pad_buffer.h check_ucf_xsmash, "tilt intent algorithm by
/// tauKhan"). Vanilla only dashes back when the stick reaches
/// dash_smash_stick_threshold (0.8) on the very frame it leaves the smash
/// deadzone; one frame of an in-between poll enters a tilt turn instead. UCF
/// lets tilt-turn anim frame 2 cancel into the dashback when the vanilla
/// smash-turn stick conditions hold (|x| >= 0.8 toward the new direction,
/// stick X active < dash_smash_window = 2 frames) AND the raw stick X moved
/// more than 75 of 80 units since two frames ago, i.e. it was a fast flick,
/// not a slow tilt. Nana (sub fighter) is excluded; she is patched
/// retroactively by the caller instead.
bool ftCo_UcfDashback(Fighter* fp)
{
    // The history is the pre-clamp stick (pc_pad_game_raw_x, fighter.c), as
    // UCF reads it: past the 80-unit rim the clamped value is shorter, and a
    // -5 -> -85 flick (80 units raw, 75 clamped) used to miss the dashback.
    s8* h = ftCo_ucf_raw_x[fp->x618_player_id];
    int delta = h[0] - h[2];
    return !fp->is_sub_fighter && fp->cur_anim_frame == 2.0f &&
           fp->input.lstick[0].x * fp->mv.co.turn.facing_after >=
               p_ftCommonData->dash_smash_stick_threshold &&
           fp->active_timer.lstick.x < p_ftCommonData->dash_smash_window &&
           delta * delta > 75 * 75;
}

bool ftCo_800C97A8(Fighter_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);

    if (fp->input.lstick[0].x * fp->facing_dir <= p_ftCommonData->x34) {
        return true;
    }
    return false;
}

bool ftCo_Turn_CheckInput(Fighter_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);

    if (ftCo_800C97A8(gobj)) {
        ftCo_Turn_Enter_Basic(gobj);
        return true;
    }
    return false;
}

void ftCo_Turn_Enter(Fighter_GObj* gobj, FtMotionId msid, MotionFlags flags,
                     f32 arg3, f32 frames_to_turn, f32 anim_start)
{
    Fighter* fp = GET_FIGHTER(gobj);

    fp->mv.co.turn.has_turned = false;
    fp->mv.co.turn.just_turned = 0;
    fp->mv.co.turn.facing_after = -fp->facing_dir;
    fp->mv.co.turn.frames_to_turn = frames_to_turn;
    fp->mv.co.turn.x8 = arg3;
    fp->mv.co.turn.x1C = 0;
    Fighter_ChangeMotionState(gobj, msid, flags, anim_start, 1.0F, 0.0F, NULL);
    ftAnim_8006EBA4(gobj);
}

void ftCo_Turn_Enter_Basic(Fighter_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);
    float frames = fp->co_attrs.standing_turn_frames;
    PAD_STACK(8);
    ftCo_Turn_Enter(gobj, ftCo_MS_Turn, Ft_MF_None, 0.0F, frames, 0.0F);
}

void ftCo_Turn_Anim_Inner(Fighter_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);

    if (fp->mv.co.turn.frames_to_turn > 0.0F) {
        fp->mv.co.turn.frames_to_turn -= 1.0F;
        return;
    }

    if (!fp->mv.co.turn.has_turned) {
        fp->mv.co.turn.has_turned = true;
        fp->mv.co.turn.just_turned = true;
        fp->facing_dir = -fp->facing_dir;
    }
}

void ftCo_Turn_Anim(Fighter_GObj* gobj)
{
    ftCo_Turn_Anim_Inner(gobj);

    if (!ftAnim_IsFramesRemaining(gobj)) {
        ft_8008A2BC(gobj);
    }
}

void ftCo_Turn_IASA(Fighter_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);

    if (fp->mv.co.turn.just_turned) {
        fp->input.pressed_buttons |= fp->mv.co.turn.x1C;
    }
    if (!fp->mv.co.turn.has_turned) {
        fp->facing_dir = -fp->facing_dir;
        if (pc_is_ucf_enabled() && ftCo_UcfDashback(fp)) {
            Fighter_GObj* nana = Player_GetEntityAtIndex(fp->player_id, 1);
            fp->mv.co.turn.has_turned = true;
            fp->mv.co.turn.just_turned = true;
            if (nana != NULL && GET_FIGHTER(nana)->kind == Ft_Kind_Nana) {
                // Nana replays Popo's inputs later; rewrite the pending
                // entry so she dashes back too (UCF dashback.cpp).
                Fighter* nfp = GET_FIGHTER(nana);
                nfp->cpu.x444->facing_dir = fp->facing_dir;
                nfp->cpu.x444->lstick.x = fp->facing_dir < 0 ? -128 : 127;
            }
        }
    }

    RETURN_IF(ftCo_SpecialS_CheckInput(gobj));
    RETURN_IF(ftCo_800D68C0(gobj));
    RETURN_IF(ftCo_Attack100_CheckInput(gobj));
    RETURN_IF(ftCo_Catch_CheckInput(gobj));
    RETURN_IF(ftCo_AttackS4_CheckInput(gobj));
    RETURN_IF(ftCo_AttackHi4_CheckInput(gobj));
    RETURN_IF(ftCo_AttackLw4_CheckInput(gobj));
    RETURN_IF(ftCo_AttackS3_CheckInput(gobj));
    RETURN_IF(ftCo_AttackHi3_CheckInput(gobj));
    RETURN_IF(ftCo_AttackLw3_CheckInput(gobj));
    RETURN_IF(ftCo_Attack1_CheckInput(gobj));

    if (!fp->mv.co.turn.has_turned) {
        fp->facing_dir = -fp->facing_dir;
    }

    RETURN_IF(ftCo_80091A4C(gobj));
    RETURN_IF(ftCo_800DE9D8(gobj));
    RETURN_IF(ftCo_Jump_CheckInput(gobj));

    fn_800C9C2C(gobj);
    if (fp->mv.co.turn.just_turned && fp->mv.co.turn.x8) {
        if (fp->input.lstick[0].x * fp->mv.co.turn.facing_after >=
            p_ftCommonData->dash_smash_stick_threshold)
        {
            ftCo_Dash_Enter(gobj, 0);
        }
    }

    if (fp->input.pressed_buttons & HSD_PAD_A) {
        fp->mv.co.turn.x1C |= HSD_PAD_A;
    }

    if (fp->input.pressed_buttons & HSD_PAD_B) {
        fp->mv.co.turn.x1C |= HSD_PAD_B;
    }

    if (fp->mv.co.turn.just_turned) {
        fp->mv.co.turn.just_turned = false;
    }
}

void ftCo_Turn_Phys(Fighter_GObj* gobj)
{
    ft_80084F3C(gobj);
}

void ftCo_Turn_Coll(Fighter_GObj* gobj)
{
    ft_80083F88(gobj);
}

bool fn_800C9C2C(Fighter_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);
    if (fp->input.lstick[0].x * fp->mv.co.turn.facing_after >=
            p_ftCommonData->dash_smash_stick_threshold &&
        fp->active_timer.lstick.x < p_ftCommonData->dash_smash_window)
    {
        fp->mv.co.turn.x8 = fp->mv.co.turn.facing_after;
        return true;
    }
    return false;
}

void ftCo_Turn_Enter_Smash(Fighter_GObj* gobj)
{
    Fighter* fp_r7 = GET_FIGHTER(gobj);
    float facing = fp_r7->facing_dir;
    PAD_STACK(1);

    fp_r7->mv.co.turn.has_turned = false;
    fp_r7->mv.co.turn.just_turned = false;
    fp_r7->mv.co.turn.facing_after = -fp_r7->facing_dir;
    fp_r7->mv.co.turn.frames_to_turn = 0.0F;
    fp_r7->mv.co.turn.x8 = facing;
    fp_r7->mv.co.turn.x1C = 0;

    Fighter_ChangeMotionState(gobj, ftCo_MS_Turn, Ft_MF_None, 0.0F, 1.0F, 0.0F,
                              NULL);
    ftAnim_8006EBA4(gobj);
}
