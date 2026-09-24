#include "ftwobble.h"

#ifdef TARGET_PC

#include <melee/ft/fighter.h>
#include <melee/ft/ftcommon.h>
#include <melee/ft/inlines.h>
#include <melee/ft/kinds/ftCommon/ftCo_Attack100.h>
#include <melee/ft/types.h>
#include <melee/gm/gmvs.h>
#include <melee/it/forward.h>
#include <melee/it/inlines.h>
#include <melee/it/types.h>
#include <melee/pl/player.h>
#include <pc/net.h>
#include <sysdolphin/baselib/gobj.h>

/* Slippi Online's "Disable Wobbling" (UnclePunch, slippi-ssbm-asm
 * External/PreventWobbling, part of "Required: Slippi Online" in
 * netplay.json): "Breaks Popo's grab after four pummels if the victim has not
 * left hitstun since the first pummel."
 *
 * The held fighter counts distinct hits (by move id) from the Ice Climber
 * holding it: Popo himself, or an item Popo owns. Nana's hits are not
 * counted. Slippi's follower lookup is commented out, so the source is
 * compared against the grabber. Re-entering CaptureWait, which means leaving
 * CaptureDamage hitstun, resets the count. A normal pummel lets
 * CaptureDamage run out between hits, so it never gets past one. Wobbling
 * uses Nana's hits to hold the victim in CaptureDamage while Popo pummels. In
 * singles the fourth such hit breaks the grab, and Nana is knocked out of
 * whatever she was doing.
 *
 * Slippi gates the codeset on the online and playback scenes. Here
 * pc_net_deterministic() covers the same set (netplay, replay record and
 * playback, sync test), so offline play stays retail. */

enum {
    WOBBLE_MAX_HITS = 3,
    /* Slippi's "star and screen KOs" (fp+2071 >> 4). */
    WOBBLE_STATE_KIND_KO = 13,
};

/// Wobbling_InitWobbleCount, injected at the end of fn_800DB790
/// (0x800DB880) and fn_800DBAE4 (0x800DBBD4): entering CaptureWaitHi/Lw.
void ftWobble_Reset(Fighter* fp)
{
    if (!pc_net_deterministic()) {
        return;
    }
    fp->mv.co.wobble.count = 0;
    fp->mv.co.wobble.last_move_id = 0xFFFF;
}

/// Wobbling_Check, injected at 0x8008F090 in ftCo_8008EC90 (the held
/// fighter's inlineB2, right after ftCo_800C8D00). Returns true when it broke
/// the grab. The caller then skips the CaptureDamage re-entry, as Slippi's
/// branch to 0x8008F0C8 does.
bool ftWobble_Check(Fighter_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);
    Fighter_GObj* grabber;
    Fighter* grabber_fp;
    HSD_GObj* source;
    Fighter_GObj* nana;
    u16 move_id;
    u16 prev_move_id;

    if (!pc_net_deterministic()) {
        return false;
    }
    if (fp->motion_id < ftCo_MS_CapturePulledHi ||
        fp->motion_id > ftCo_MS_CaptureDamageLw)
    {
        return false;
    }
    grabber = fp->victim_gobj;
    if (grabber == NULL) {
        return false;
    }
    grabber_fp = GET_FIGHTER(grabber);
    /* Popo, the Ice Climber with a follower (ftPp_Init_OnLoad). */
    if (!grabber_fp->x2222_b5) {
        return false;
    }

    source = fp->dmg.x1868_source;
    if (source == grabber) {
        move_id = grabber_fp->x2074.x2088;
    } else if (source != NULL && source->classifier == HSD_GOBJ_CLASS_ITEM &&
               GET_ITEM(source)->owner == grabber)
    {
        move_id = GET_ITEM(source)->xDA8_short;
    } else {
        return false;
    }
    prev_move_id = fp->mv.co.wobble.last_move_id;
    if (move_id == prev_move_id) {
        return false;
    }
    fp->mv.co.wobble.last_move_id = move_id;
    fp->mv.co.wobble.count++;

    /* Counted in teams too; only the break is singles-only. */
    if (gm_8016B168()) {
        return false;
    }
    if (fp->mv.co.wobble.count <= WOBBLE_MAX_HITS) {
        return false;
    }

    /* Slippi never sets r4 for this call. It still holds the previous move
     * id from the duplicate check, so the held fighter also enters
     * CaptureCut unless that id was 0. */
    ftCo_800DA698(grabber, prev_move_id != 0);

    nana = Player_GetEntityAtIndex(grabber_fp->player_id, 1);
    if (nana != NULL) {
        Fighter* nfp = GET_FIGHTER(nana);
        if (!nfp->x221F_b1 && !nfp->x2219_b5 &&
            nfp->x2070.x2071_b0_3 != WOBBLE_STATE_KIND_KO)
        {
            if (nfp->ground_or_air == GA_Ground) {
                ftCo_800DA698(nana, false);
            } else {
                /* fn_800DC070 without its ftCo_800DC920 hand-off, since
                 * Nana holds no one. */
                ftCommon_8007D5D4(nfp);
                nfp->self_vel.x = -nfp->facing_dir * p_ftCommonData->x374;
                nfp->self_vel.y = p_ftCommonData->x378;
                nfp->mv.co.buryjump.x0 = 0;
                Fighter_ChangeMotionState(nana, ftCo_MS_CaptureJump, 0, 0.0F,
                                          1.0F, 0.0F, NULL);
            }
        }
    }
    return true;
}

#endif
