#define __assert libc_assert
#include <assert.h>
#undef __assert
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/melee/ft/ftwobble.c"

// Drives Slippi Online's Disable Wobbling rule (src/melee/ft/ftwobble.c)
// through scripted hit sequences on a held fighter and checks when the grab
// breaks. The mocks record the CatchCut / CaptureJump calls the break makes.

ftCommonData* p_ftCommonData;

static bool rule_on;
static bool teams;
bool pc_net_deterministic(void) {
    return rule_on;
}
bool gm_8016B168(void) {
    return teams;
}

enum { VICTIM, POPO, NANA, OTHER, N_FIGHTERS };
static HSD_GObj gobjs[N_FIGHTERS];
static Fighter* fps[N_FIGHTERS];
static HSD_GObj item_gobj;
static Item* item;

static int catchcut_calls;
static HSD_GObj* catchcut_gobj[4];
static bool catchcut_arg[4];
void ftCo_800DA698(Fighter_GObj* gobj, bool arg1) {
    assert(catchcut_calls < 4);
    catchcut_gobj[catchcut_calls] = gobj;
    catchcut_arg[catchcut_calls] = arg1;
    catchcut_calls++;
}

static int lose_ground_calls;
void ftCommon_8007D5D4(Fighter* fp) {
    assert(fp == fps[NANA]);
    lose_ground_calls++;
}

static int change_calls;
static FtMotionId change_msid;
void Fighter_ChangeMotionState(Fighter_GObj* gobj, FtMotionId msid, MotionFlags flags,
    f32 anim_start, f32 anim_speed, f32 anim_blend, Fighter_GObj* arg3) {
    assert(gobj == &gobjs[NANA]);
    change_calls++;
    change_msid = msid;
}

static bool nana_present;
HSD_GObj* Player_GetEntityAtIndex(int slot, int index) {
    assert(slot == fps[POPO]->player_id);
    assert(index == 1);
    return nana_present ? &gobjs[NANA] : NULL;
}

static u16 next_move_id;

/// Fresh match: Popo (port 1) holds the victim; Nana grounded and alive.
static void setup(bool on, bool is_teams) {
    int i;
    rule_on = on;
    teams = is_teams;
    nana_present = true;
    catchcut_calls = lose_ground_calls = change_calls = 0;
    next_move_id = 100;
    for (i = 0; i < N_FIGHTERS; i++) {
        memset(fps[i], 0, sizeof(Fighter));
        memset(&gobjs[i], 0, sizeof(HSD_GObj));
        gobjs[i].classifier = HSD_GOBJ_CLASS_FIGHTER;
        gobjs[i].user_data = fps[i];
        fps[i]->gobj = &gobjs[i];
        fps[i]->player_id = i == NANA ? 1 : i;
    }
    memset(item, 0, sizeof(Item));
    memset(&item_gobj, 0, sizeof(item_gobj));
    item_gobj.classifier = HSD_GOBJ_CLASS_ITEM;
    item_gobj.user_data = item;
    item->owner = &gobjs[POPO];

    fps[POPO]->kind = Ft_Kind_Popo;
    fps[POPO]->x2222_b5 = true;
    fps[NANA]->kind = Ft_Kind_Nana;
    fps[NANA]->is_sub_fighter = true;
    fps[NANA]->ground_or_air = GA_Ground;
    fps[NANA]->facing_dir = 1.0f;

    fps[VICTIM]->victim_gobj = &gobjs[POPO];
    fps[POPO]->victim_gobj = &gobjs[VICTIM];
    // Grab lands: CapturePulled, then CaptureWait (fn_800DB6C8 -> fn_800DBAE4).
    memset(&fps[VICTIM]->mv, 0xAB, sizeof(fps[VICTIM]->mv));
    fps[VICTIM]->motion_id = ftCo_MS_CapturePulledLw;
    ftWobble_Reset(fps[VICTIM]);
    fps[VICTIM]->motion_id = ftCo_MS_CaptureWaitLw;
    ftWobble_Reset(fps[VICTIM]);
}

/// One hit on the held fighter from `who`, with a fresh move id unless
/// `move_id` is given. Returns whether the grab broke. The held fighter
/// then sits in CaptureDamage (still in hitstun) unless the grab broke.
static bool hit_by(HSD_GObj* who, int move_id) {
    bool broke;
    u16 id = move_id >= 0 ? (u16)move_id : next_move_id++;
    if (who == &item_gobj) {
        item->xDA8_short = id;
    } else {
        GET_FIGHTER(who)->x2074.x2088 = id;
    }
    fps[VICTIM]->dmg.x1868_source = who;
    broke = ftWobble_Check(&gobjs[VICTIM]);
    if (!broke) {
        fps[VICTIM]->motion_id = ftCo_MS_CaptureDamageLw;
    }
    return broke;
}
#define FRESH -1

/// CaptureDamage ran out: back to CaptureWait (fn_800DBAE4).
static void leave_hitstun(void) {
    fps[VICTIM]->motion_id = ftCo_MS_CaptureWaitLw;
    ftWobble_Reset(fps[VICTIM]);
}

static void expect_grab_broken(bool held_also_cut) {
    assert(catchcut_calls >= 1);
    assert(catchcut_gobj[0] == &gobjs[POPO]);
    assert(catchcut_arg[0] == held_also_cut);
}

int main(void) {
    int i;
    ftCommonData* cd = calloc(1, sizeof(ftCommonData));
    cd->x374 = 1.5f;
    cd->x378 = 2.5f;
    p_ftCommonData = cd;
    for (i = 0; i < N_FIGHTERS; i++) {
        fps[i] = malloc(sizeof(Fighter));
    }
    item = malloc(sizeof(Item));

    // Wobbling: Popo's pummels keep landing while Nana holds the victim in
    // CaptureDamage. Three are allowed; the fourth distinct one breaks it.
    setup(true, false);
    for (i = 0; i < 3; i++) {
        assert(!hit_by(&gobjs[POPO], FRESH));
        assert(!hit_by(&gobjs[NANA], FRESH));
    }
    assert(fps[VICTIM]->mv.co.wobble.count == 3);
    assert(catchcut_calls == 0);
    assert(hit_by(&gobjs[POPO], FRESH));
    expect_grab_broken(true);
    // Grounded Nana is knocked into CatchCut too.
    assert(catchcut_calls == 2);
    assert(catchcut_gobj[1] == &gobjs[NANA] && !catchcut_arg[1]);
    assert(change_calls == 0 && lose_ground_calls == 0);

    // Rule off (offline play): retail, never breaks.
    setup(false, false);
    for (i = 0; i < 8; i++) {
        assert(!hit_by(&gobjs[POPO], FRESH));
    }
    assert(catchcut_calls == 0);

    // Teams: counted, never broken.
    setup(true, true);
    for (i = 0; i < 8; i++) {
        assert(!hit_by(&gobjs[POPO], FRESH));
    }
    assert(fps[VICTIM]->mv.co.wobble.count == 8);
    assert(catchcut_calls == 0);

    // A repeat of the same move id (one multi-frame hitbox) counts once.
    setup(true, false);
    assert(!hit_by(&gobjs[POPO], 7));
    for (i = 0; i < 6; i++) {
        assert(!hit_by(&gobjs[POPO], 7));
    }
    assert(fps[VICTIM]->mv.co.wobble.count == 1);
    assert(!hit_by(&gobjs[POPO], 8));
    assert(!hit_by(&gobjs[POPO], 9));
    assert(catchcut_calls == 0);
    assert(hit_by(&gobjs[POPO], 10));
    expect_grab_broken(true);

    // Nana's hits never count, whatever their move ids; neither do hits
    // from anyone else. Slippi compares the source with the grabber.
    setup(true, false);
    for (i = 0; i < 10; i++) {
        assert(!hit_by(&gobjs[NANA], FRESH));
        assert(!hit_by(&gobjs[OTHER], FRESH));
    }
    assert(fps[VICTIM]->mv.co.wobble.count == 0);
    assert(catchcut_calls == 0);

    // A grabber that is not Popo (no follower): never counted.
    setup(true, false);
    fps[POPO]->x2222_b5 = false;
    for (i = 0; i < 8; i++) {
        assert(!hit_by(&gobjs[POPO], FRESH));
    }
    assert(catchcut_calls == 0);

    // Leaving hitstun between pummels (a normal pummel) resets the count.
    setup(true, false);
    for (i = 0; i < 10; i++) {
        assert(!hit_by(&gobjs[POPO], FRESH));
        assert(!hit_by(&gobjs[POPO], FRESH));
        assert(!hit_by(&gobjs[POPO], FRESH));
        leave_hitstun();
    }
    assert(catchcut_calls == 0);

    // Items Popo owns count with the item's move id; others' items do not.
    setup(true, false);
    assert(!hit_by(&item_gobj, FRESH));
    item->owner = &gobjs[OTHER];
    assert(!hit_by(&item_gobj, FRESH));
    item->owner = &gobjs[POPO];
    assert(fps[VICTIM]->mv.co.wobble.count == 1);
    assert(!hit_by(&gobjs[POPO], FRESH));
    assert(!hit_by(&item_gobj, FRESH));
    assert(hit_by(&gobjs[POPO], FRESH));
    expect_grab_broken(true);

    // Only while held: a thrown fighter (CaptureCut and on) is left alone.
    setup(true, false);
    for (i = 0; i < 3; i++) {
        assert(!hit_by(&gobjs[POPO], FRESH));
    }
    fps[VICTIM]->motion_id = ftCo_MS_CaptureCut;
    assert(!ftWobble_Check(&gobjs[VICTIM]));
    assert(catchcut_calls == 0);

    // Airborne Nana goes to CaptureJump with the CatchCut air velocity.
    setup(true, false);
    fps[NANA]->ground_or_air = GA_Air;
    for (i = 0; i < 3; i++) {
        assert(!hit_by(&gobjs[POPO], FRESH));
    }
    assert(hit_by(&gobjs[POPO], FRESH));
    expect_grab_broken(true);
    assert(catchcut_calls == 1);
    assert(lose_ground_calls == 1 && change_calls == 1);
    assert(change_msid == ftCo_MS_CaptureJump);
    assert(fps[NANA]->self_vel.x == -1.5f && fps[NANA]->self_vel.y == 2.5f);

    // Dead, in hitlag or star-KO'd Nana is left alone, as is a missing one.
    for (i = 0; i < 4; i++) {
        setup(true, false);
        switch (i) {
        case 0:
            fps[NANA]->x221F_b1 = true;
            break;
        case 1:
            fps[NANA]->x2219_b5 = true;
            break;
        case 2:
            fps[NANA]->x2070.x2071_b0_3 = 13;
            break;
        case 3:
            nana_present = false;
            break;
        }
        hit_by(&gobjs[POPO], FRESH);
        hit_by(&gobjs[POPO], FRESH);
        hit_by(&gobjs[POPO], FRESH);
        assert(hit_by(&gobjs[POPO], FRESH));
        expect_grab_broken(true);
        assert(catchcut_calls == 1 && change_calls == 0);
    }

    // Slippi's CatchCut call reuses the previous move id as its "cut the
    // victim too" flag: a 0 there leaves the held fighter alone.
    setup(true, false);
    assert(!hit_by(&gobjs[POPO], FRESH));
    assert(!hit_by(&gobjs[POPO], FRESH));
    assert(!hit_by(&gobjs[POPO], 0));
    assert(hit_by(&gobjs[POPO], FRESH));
    expect_grab_broken(false);

    // The reset is a no-op offline too: retail motion vars stay untouched.
    rule_on = false;
    memset(&fps[VICTIM]->mv, 0x5A, sizeof(fps[VICTIM]->mv));
    ftWobble_Reset(fps[VICTIM]);
    assert(fps[VICTIM]->mv.co.wobble.count == 0x5A);

    // Slippi's slots: fp+2384 (count) and fp+2386 (last move id) are
    // mv+0x44/0x46, clear of the capture states' own motion vars.
    assert(offsetof(Fighter, mv.co.wobble.count) - offsetof(Fighter, mv) == 0x44);
    assert(offsetof(Fighter, mv.co.wobble.last_move_id) - offsetof(Fighter, mv) == 0x46);
    assert(sizeof(fps[0]->mv.co.capturewait) <= 0x44);
    assert(sizeof(fps[0]->mv.co.capturedamage) <= 0x44);

    for (i = 0; i < N_FIGHTERS; i++) {
        free(fps[i]);
    }
    free(item);
    free(cd);
    puts("wobble: ok");
    return 0;
}
