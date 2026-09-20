#include "mnstagesel.h"

#include <placeholder.h>

#include "inlines.h"
#include "mnmain.h"
#include "mnstagesel.static.h"
#include <melee/gm/gm_unsplit.h>
#include <melee/gm/gmmain_lib.h>
#include <melee/lb/lb_00B0.h>
#include <melee/lb/lb_013B.h>
#include <melee/lb/lbarchive.h>
#include <melee/lb/lbaudio_ax.h>
#include <melee/lb/lbdvd.h>
#include <melee/lb/lblanguage.h>
#include <melee/lb/types.h>
#include <sysdolphin/baselib/controller.h>
#include <sysdolphin/baselib/fog.h>
#include <sysdolphin/baselib/gobj.h>
#include <sysdolphin/baselib/gobjgxlink.h>
#include <sysdolphin/baselib/gobjobject.h>
#include <sysdolphin/baselib/gobjplink.h>
#include <sysdolphin/baselib/gobjproc.h>
#include <sysdolphin/baselib/gobjuserdata.h>
#include <sysdolphin/baselib/jobj.h>
#include <sysdolphin/baselib/lobj.h>
#include <sysdolphin/baselib/memory.h>
#include <sysdolphin/baselib/random.h>
#ifdef TARGET_PC
#include "pc/net.h"
#include "pc/net_lan.h"
#include "pc/net_rank_session.h"
#include <sysdolphin/baselib/sislib.h>
#include "pc/pc.h"

/* A compact ranked list uses synchronized pads and makes both bans visible.
 * It does not depend on the archive's cursor geometry. */
static HSD_Text* rank_text;
static int rank_lines[7], rank_cursor, rank_axis;
static const unsigned rank_stages[] = {2, 3, 8, 28, 31, 32};
static const char* rank_names[] = {"FOUNTAIN OF DREAMS", "POKEMON STADIUM",
    "YOSHI'S STORY", "DREAM LAND", "BATTLEFIELD", "FINAL DESTINATION"};
static void rankStageDraw(void)
{
    int port = pc_rank_session_stage_port();
    HSD_SisLib_803A70A0(rank_text, rank_lines[0], "P%d %s", port + 1,
                       pc_rank_session_stage_prompt());
    for (int i = 0; i < 6; i++) {
        HSD_SisLib_803A70A0(rank_text, rank_lines[i + 1], "%s %s %s",
            i == rank_cursor ? ">" : " ", rank_names[i],
            pc_rank_session_stage_available(rank_stages[i]) ? "" : "(BANNED)");
    }
}
static void rankStageCreate(void)
{
    HSD_SisLib_803A62A0(0, lbLang_IsSavedLanguageUS() ? "SdMenu.usd" : "SdMenu.dat",
                       "SIS_MenuData");
    HSD_SisLib_803A611C(0, NULL, 9, 0xD, 0, 0xE, 0, 0x13);
    rank_text = HSD_SisLib_803A6754(0, 0);
    rank_text->default_kerning = 1;
    rank_cursor = rank_axis = 0;
    for (int i = 0; i < 7; i++) {
        rank_lines[i] = HSD_SisLib_803A6B98(rank_text, 50.0f, 100.0f + i * 40.0f, " ");
        HSD_SisLib_803A7548(rank_text, rank_lines[i], 0.55f, 0.55f);
    }
    rankStageDraw();
}
static void rankStageFrame(void)
{
    int port = pc_rank_session_stage_port();
    if (port < 0) return;
    HSD_PadStatus* pad = &HSD_PadCopyStatus[port];
    int axis = pad->stickY > 30 ? -1 : pad->stickY < -30 ? 1 : 0;
    int move = (pad->trigger & 8) ? -1 : (pad->trigger & 4) ? 1 :
               axis != rank_axis ? axis : 0;
    rank_axis = axis;
    if (move) rank_cursor = (rank_cursor + move + 6) % 6;
    if (pad->trigger & 0x200) {
        pc_rank_session_abort("ranked stage selection cancelled");
        gm_801A4B60();
        return;
    }
    if (pad->trigger & 0x1100) {
        if (!pc_rank_session_choose_stage(port, rank_stages[rank_cursor])) {
            lbAudioAx_80024030(3);
        } else if (pc_rank_session_stage()) {
            sss_data->vs.start.rules.stkind = pc_rank_session_stage();
            mnStageSel_804D6CAF = 2;
            gm_801A4B60();
            return;
        }
    }
    rankStageDraw();
}

/* Online: each player picks a stage on their own port (the SSS otherwise
 * merges every port into one cursor), picks are exchanged over the reliable
 * channel, and the stage is one of the two chosen by a coin flip from the
 * shared seed, so both peers agree without another message. Values are
 * table indices (mnStageSel_803F06D0), 30 = random. */
#define NET_MSG_STAGE_PICK 0x20
static int net_local_pick = -1;
static int net_remote_pick = -1;

static bool netStageSel_Active(void)
{
    return pc_net_active();
}

static void netStageSel_Reset(void)
{
    net_local_pick = net_remote_pick = -1;
}

static void netStageSel_Poll(void)
{
    u8 type;
    u8 buf[4];
    while (pc_net_recv_reliable(&type, buf, sizeof buf) >= 0) {
        if (type == NET_MSG_STAGE_PICK) {
            net_remote_pick = buf[0];
            pc_log_line("sss: opponent picked %d", net_remote_pick);
        }
    }
}

static void netStageSel_SendPick(int idx)
{
    u8 b = (u8) idx;
    net_local_pick = idx;
    pc_net_send_reliable(NET_MSG_STAGE_PICK, &b, 1);
    pc_log_line("sss: we picked %d", idx);
}

/* Both peers have picked: order the picks by port so the expression is the
 * same on both sides, then let the shared seed flip the coin. */
static u32 netStageSel_Mix(void)
{
    int local = pc_net_local_player();
    int p0 = local == 0 ? net_local_pick : net_remote_pick;
    int p1 = local == 0 ? net_remote_pick : net_local_pick;
    if (p0 < 0 || p1 < 0) {
        return pc_net_seed() * 2654435761u + (u32) mnStageSel_804D6CAE;
    }
    return pc_net_seed() * 2654435761u + (u32) (p0 * 31 + p1);
}

static int netStageSel_Resolve(void)
{
    int local = pc_net_local_player();
    int p0 = local == 0 ? net_local_pick : net_remote_pick;
    int p1 = local == 0 ? net_remote_pick : net_local_pick;
    int pick = (netStageSel_Mix() >> 16) & 1 ? p1 : p0;
    pc_log_line("sss: picks P1=%d P2=%d -> %d", p0, p1, pick);
    return pick;
}

/* "Random" online: the offline roll uses the live RNG and a per-machine
 * cooldown table, neither of which is in sync at this point (the peer's
 * pick lands on a different frame on each side). Draw from the seed
 * instead, over the stages the synced random-stage switches allow. */
static int netStageSel_Random(void)
{
    int allowed[NUM_STAGES];
    int n = 0;
    /* NUM_STAGES is 29 and the table holds 30: the last entry is the RANDOM
     * button, not a stage (mnstagesel.static.h:45, stkind 0). The bound
     * already excludes it; the stkind test says so out loud, because a
     * stkind of 0 starts a match with no stage that falls straight through
     * to the results screen, in sync, with nothing logged. */
    for (int i = 0; i < NUM_STAGES; i++) {
        if (mnStageSel_803F06D0[i].stkind != 0 &&
            (u8) gm_80164330(mnStageSel_803F06D0[i].xA)) {
            allowed[n++] = i;
        }
    }
    if (n == 0) {
        return 0;
    }
    u32 r = netStageSel_Mix() * 22695477u + 1u;
    return allowed[(r >> 8) % (u32) n];
}
#endif

/// @todo .sdata2 order hack
#ifdef MUST_MATCH
static void order_sdata2(void)
{
    (void) S32_TO_F32;
}
#endif

/// Random stage selection
/// Returns an internal stage ID - 2 (since first 2 internal stage IDs are
/// invalid)
int mnStageSel_802599EC(void)
{
    int var_r0;
    int iter;
    bool var_r29 = true;
    int i;

    for (i = 0; i < NUM_STAGES; i++) {
        if (mnStageSel_803F06D0[i].x4 >= 0 &&
            (u8) gm_80164330(mnStageSel_803F06D0[i].xA))
        {
            break;
        }
    }
    if (i == NUM_STAGES) {
        for (i = 0; i < NUM_STAGES; i++) {
            mnStageSel_803F06D0[i].x4 = 0;
        }
    }
    while (var_r29) {
        for (i = 0; i < NUM_STAGES; i++) {
            if (mnStageSel_803F06D0[i].x4 > 0) {
                mnStageSel_803F06D0[i].x4--;
            }
        }
        for (i = 0; i < NUM_STAGES; i++) {
            if (mnStageSel_803F06D0[i].x4 == 0 &&
                (u8) gm_80164330(mnStageSel_803F06D0[i].xA))
            {
                var_r29 = false;
            }
        }
    }
    for (iter = 0; iter < MAX_ITER; iter++) {
        int tmp = HSD_Randi(NUM_STAGES);
        i = tmp;
        if (mnStageSel_803F06D0[i].x4 == 0) {
            if ((u8) gm_80164330(mnStageSel_803F06D0[i].xA)) {
                break;
            }
        }
    }
    if (iter >= MAX_ITER) {
        i = 0;
    }
    mnStageSel_803F06D0[i].x4 = -1;
    if (i < 22) {
        if (i & 1) {
            var_r0 = i - 1;
        } else {
            var_r0 = i + 1;
        }
        if (mnStageSel_803F06D0[var_r0].x4 >= 0) {
            mnStageSel_803F06D0[var_r0].x4 = 3;
        }
    }
    return i;
}

void mnStageSel_80259C28(void)
{
    HSD_JObj* jobj;
    HSD_GObj* gobj;
    u64 _[2];

#ifdef TARGET_PC
    if (rank_text) return;
#endif
    if (mnStageSel_804D6CA4 != 0) {
        return;
    }
    switch (mnStageSel_804D6CAE) {
    case 30:
        if (!(mnStageSel_804D6CA0 & 0x1000)) {
            return;
        }
        break;
    case 29:
        if (!(mnStageSel_804D6CA0 & 0x1100)) {
            return;
        }
        break;
    default:
        if (!(mnStageSel_804D6CA0 & 0x1100)) {
            return;
        }
        if (mnStageSel_804D6CAE < 0x1E &&
            mnStageSel_803F06D0[mnStageSel_804D6CAE].x8 >= 2)
        {
#ifdef TARGET_PC
            if (netStageSel_Active() && !pc_rank_session_active()) {
                netStageSel_SendPick(mnStageSel_804D6CAE);
            }
#endif
            goto skip_randomize;
        }
        lbAudioAx_80024030(3);
        return;
    }
#ifdef TARGET_PC
    if (netStageSel_Active() && !pc_rank_session_active()) {
        /* Send the cell (30 = random) and defer the roll to the resolve so
         * both peers roll from the same RNG state. */
        netStageSel_SendPick(mnStageSel_804D6CAE);
        goto skip_randomize;
    }
#endif
    mnStageSel_804D6CAE = mnStageSel_802599EC();
skip_randomize:

    gobj = GObj_Create(HSD_GOBJ_CLASS_FIGHTER, 5, 0x80);
    jobj = HSD_JObjLoadJoint(DP(HSD_Joint, mnStageSel_804D6C98->xB0));
    HSD_GObjObject_80390A70(gobj, HSD_GObj_JObjKind, jobj);
    GObj_SetupGXLink(gobj, HSD_GObj_JObjCallback, 4, 0x87);
    HSD_GObj_SetupProc(gobj, mn_8022EAE0, 0);
    HSD_JObjAddAnimAll(jobj, DP(HSD_AnimJoint, mnStageSel_804D6C98->xB4),
                       DP(HSD_MatAnimJoint,
                          mnStageSel_804D6C98->xB8), DP(HSD_ShapeAnimJoint,
                          mnStageSel_804D6C98->xBC));
    HSD_JObjReqAnimAll(gobj->hsd_obj, 0.0F);
    HSD_JObjAnimAll(gobj->hsd_obj);
    mnStageSel_804D6CAF = 1;
    mnStageSel_804D6CA4 = 0x1E;
    sfxForward();
}

void fn_80259D84(HSD_GObj* gobj)
{
    struct StageSelUserData* temp_r31 = HSD_GObjGetUserData(gobj);
    HSD_JObj* jobj = GET_JOBJ(gobj);

    switch (temp_r31->x2) {
    case 0:
        if (mnStageSel_804D6CAF == 0) {
            mnStageSel_80259C28();
        }
        if (++temp_r31->x4 >= 9U) {
            HSD_ForeachAnim(jobj, JOBJ_TYPE, ALL_TYPE_MASK, HSD_AObjStopAnim,
                            AOBJ_ARG_AOV, NULL);
            temp_r31->x2 = 1;
        }
        break;
    case 1:
        if (mnStageSel_804D6CAF == 0) {
            mnStageSel_80259C28();
        }
        if (temp_r31->x0 != mnStageSel_804D6CAE) {
            if (temp_r31->x0 < 0x1E &&
                mnStageSel_803F06D0[temp_r31->x0].x8 >= 2)
            {
                HSD_JObjReqAnimAllByFlags(jobj, 1, 10.0F);
            }
            sfxMove();
            temp_r31->x4 = 0;
            temp_r31->x2 = 2;
            mnStageSel_80259ED8(mnStageSel_804D6CAE);
        }
        break;
    case 2:
        if (++temp_r31->x4 > 0xAU) {
            HSD_GObjFree(gobj);
            temp_r31->x2++;
        }
        break;
    }
}

static void do_anim(HSD_JObj* jobj, int frame)
{
    HSD_JObjReqAnimAll(jobj, 0.0F);
    HSD_JObjReqAnimAllByFlags(jobj, 0x10, frame);
    HSD_JObjAnimAll(jobj);
    HSD_ForeachAnim(jobj, JOBJ_TYPE, TOBJ_MASK, HSD_AObjStopAnim, AOBJ_ARG_AOV,
                    NULL);
}

void mnStageSel_80259ED8(int id)
{
    HSD_GObj* gobj;
    HSD_JObj* jobj;
    struct StageSelUserData* temp_r3_2;
    u8 _[4];

    gobj = GObj_Create(HSD_GOBJ_CLASS_FIGHTER, 5, 0x80);
    jobj = HSD_JObjLoadJoint(DP(HSD_Joint, mnStageSel_804D6C98->x30.joint));
    HSD_GObjObject_80390A70(gobj, HSD_GObj_JObjKind, jobj);
    GObj_SetupGXLink(gobj, HSD_GObj_JObjCallback, 4, 0x84);
    HSD_GObj_SetupProc(gobj, mn_8022EAE0, 1);
    HSD_JObjAddAnimAll(jobj,
                       DP(HSD_AnimJoint, mnStageSel_804D6C98->x30.animjoint),
                       DP(HSD_MatAnimJoint,
                          mnStageSel_804D6C98->x30.matanim_joint),
                       DP(HSD_ShapeAnimJoint,
                          mnStageSel_804D6C98->x30.shapeanim_joint));
    jobj = GET_JOBJ(gobj);
    temp_r3_2 = HSD_MemAlloc(sizeof(struct StageSelUserData));
    GObj_InitUserData(gobj, 4, HSD_Free, temp_r3_2);
    HSD_GObj_SetupProc(gobj, fn_80259D84, 1);
    temp_r3_2->x0 = id;
    temp_r3_2->x4 = 0;
    temp_r3_2->x2 = 0;
    if (id < 0x1E && mnStageSel_803F06D0[id].x8 >= 2) {
        do_anim(jobj, 20.0F * mnStageSel_803F06D0[id].x9);
    }
}

void fn_8025A090(HSD_GObj* gobj)
{
    u32 var_r3;
    struct {
        u32 x0;
        u32 x4;
    }* temp_r30;
    HSD_JObj* jobj;

    jobj = GET_JOBJ(gobj);
    temp_r30 = HSD_GObjGetUserData(gobj);
    var_r3 = mnStageSel_804D6CAE;
    /* 0x1E is the "random" sentinel (see :485), one past the end of the
     * 30-entry table. */
    if (mnStageSel_804D6CAE >= 0x1E ||
        mnStageSel_803F06D0[mnStageSel_804D6CAE].x8 < 2)
    {
        var_r3 = 0x1E;
    }
    if (temp_r30->x0 != var_r3) {
        temp_r30->x0 = var_r3;
        temp_r30->x4 = 0;
        if ((s32) var_r3 < 0x1D) {
            HSD_JObjReqAnimAll(jobj, 50.0F * mnStageSel_803F06D0[var_r3].x9);
            HSD_JObjAnimAll(jobj);
            HSD_ForeachAnim(jobj, JOBJ_TYPE, ALL_TYPE_MASK, HSD_AObjStopAnim,
                            AOBJ_ARG_AOV, NULL);
            HSD_JObjSetTranslateX(jobj, 0.0F);
        } else {
            HSD_JObjSetTranslateX(jobj, 100.0F);
        }
    }
    if (temp_r30->x4 < 0x5A) {
        temp_r30->x4++;
        if (temp_r30->x4 == 0x14 && temp_r30->x0 < 0x1E) {
            HSD_JObjReqAnimAll(jobj,
                               50.0F * mnStageSel_803F06D0[temp_r30->x0].x9);
        }
        if (temp_r30->x4 == 0x45) {
            HSD_ForeachAnim(jobj, JOBJ_TYPE, ALL_TYPE_MASK, HSD_AObjStopAnim,
                            AOBJ_ARG_AOV, NULL);
        }
    } else if (mnStageSel_804D6CAF) {
        mnStageSel_804D6CAF = 2;
    }
}

void fn_8025A310(HSD_GObj* gobj)
{
    Vec3 sp1C;
    Vec3 sp10;
    f32 temp_f1;
    f32 temp_f2;
    int i;
    HSD_JObj* jobj;
    u32 unused;

    jobj = gobj->hsd_obj;
    if (mnStageSel_804D6CAF != 0) {
        HSD_JObjSetFlags(jobj, JOBJ_HIDDEN);
        return;
    }
    HSD_JObjGetTranslation(jobj, &sp1C);
    sp1C.x = 0.03f * mnStageSel_804D6CAC + sp1C.x;
    if (-27.0F > sp1C.x) {
        sp1C.x = -27.0F;
    }
    if (27.0F < sp1C.x) {
        sp1C.x = 27.0F;
    }
    sp1C.y = 0.03f * mnStageSel_804D6CAD + sp1C.y;
    if (-19.0F > sp1C.y) {
        sp1C.y = -19.0F;
    }
    if (19.0F < sp1C.y) {
        sp1C.y = 19.0F;
    }

    HSD_JObjSetTranslate(jobj, &sp1C);
    lb_8000B1CC(jobj, NULL, &sp1C);
    for (i = 0; i < 0x1E; i++) {
        if (mnStageSel_803F06D0[i].x8 != 0) {
            lb_8000B1CC(mnStageSel_803F06D0[i].x0, NULL, &sp10);
            temp_f2 = sp10.x;
            temp_f1 = mnStageSel_803F06D0[i].xC;
            if (temp_f2 - temp_f1 < sp1C.x && temp_f2 + temp_f1 > sp1C.x) {
                if (sp10.y - mnStageSel_803F06D0[i].x10 < sp1C.y &&
                    sp10.y + mnStageSel_803F06D0[i].x10 > sp1C.y)
                {
                    mnStageSel_804D6CAE = i;
                    return;
                }
            }
        }
    }
}

void fn_8025A560(HSD_GObj* gobj)
{
    struct StageSelUserData {
        int x0;
    }* temp_r30;
    Vec3 sp10;
    HSD_JObj* jobj = GET_JOBJ(gobj);
    temp_r30 = HSD_GObjGetUserData(gobj);

    if (mnStageSel_804D6CAE < 0x16 && (mnStageSel_804D6CAE & 1) &&
        mnStageSel_803F06D0[mnStageSel_804D6CAE].x8 == 0)
    {
        HSD_JObjSetTranslateX(jobj, 100.0F);
    } else if (mnStageSel_804D6CAE < 0x1E) {
        lb_8000B1CC(mnStageSel_803F06D0[mnStageSel_804D6CAE].x0, NULL, &sp10);
        HSD_JObjSetTranslateX(jobj, sp10.x);
        HSD_JObjSetTranslateY(jobj, sp10.y);
        HSD_JObjSetScaleX(jobj, mnStageSel_803F06D0[mnStageSel_804D6CAE].x14);
        HSD_JObjSetScaleY(jobj, mnStageSel_803F06D0[mnStageSel_804D6CAE].x18);
    } else {
        HSD_JObjSetTranslateX(jobj, 100.0F);
    }
    if (mnStageSel_804D6CAF != 0) {
        HSD_JObjReqAnimAll(jobj, 0.0F);
        HSD_JObjAnimAll(jobj);
        return;
    }
    if (++temp_r30->x0 >= 10) {
        temp_r30->x0 = 0;
        HSD_JObjReqAnimAll(jobj, 0.0F);
        HSD_JObjAnimAll(jobj);
    }
}

void fn_8025A91C(HSD_GObj* gobj)
{
    HSD_JObj* jobj = GET_JOBJ(gobj);
    if (++mnStageSel_804D6CA8 >= 0xFA) {
        mnStageSel_804D6CA8 = 0;
        HSD_JObjReqAnimAll(jobj, 0.0F);
        HSD_JObjAnimAll(jobj);
    }
}

void fn_8025A974(HSD_GObj* gobj, int unused)
{
    HSD_FogSet(gobj->hsd_obj);
}

static const Vec3 mnStageSel_803B8550 = { 0, -13, 0 };

static inline void make_stage_icon(HSD_JObj** out)
{
    HSD_GObj* gobj;
    HSD_JObj* jobj;
    gobj = GObj_Create(4, 5, 0x80);
    jobj = HSD_JObjLoadJoint(DP(HSD_Joint, mnStageSel_804D6C98->x40.joint));
    HSD_GObjObject_80390A70(gobj, HSD_GObj_JObjKind, jobj);
    GObj_SetupGXLink(gobj, HSD_GObj_JObjCallback, 4, 0x83);
    HSD_GObj_SetupProc(gobj, mn_8022EAE0, 3);
    HSD_JObjAddAnimAll(jobj,
                       DP(HSD_AnimJoint, mnStageSel_804D6C98->x40.animjoint),
                       DP(HSD_MatAnimJoint,
                          mnStageSel_804D6C98->x40.matanim_joint),
                       DP(HSD_ShapeAnimJoint,
                          mnStageSel_804D6C98->x40.shapeanim_joint));
    *out = GET_JOBJ(gobj);
}

static inline void attach_menu_model(HSD_GObj* gobj)
{
    HSD_JObj* jobj;
    jobj = HSD_JObjLoadJoint(DP(HSD_Joint, mnStageSel_804D6C98->xA0.joint));
    HSD_GObjObject_80390A70(gobj, HSD_GObj_JObjKind, jobj);
    GObj_SetupGXLink(gobj, HSD_GObj_JObjCallback, 4, 0x80);
    HSD_GObj_SetupProc(gobj, mn_8022EAE0, 0);
    HSD_JObjAddAnimAll(jobj,
                       DP(HSD_AnimJoint, mnStageSel_804D6C98->xA0.animjoint),
                       DP(HSD_MatAnimJoint,
                          mnStageSel_804D6C98->xA0.matanim_joint),
                       DP(HSD_ShapeAnimJoint,
                          mnStageSel_804D6C98->xA0.shapeanim_joint));
}

static inline void make_bg_model(HSD_JObj** out)
{
    HSD_GObj* gobj;
    HSD_JObj* jobj;
    gobj = GObj_Create(4, 5, 0x80);
    jobj = HSD_JObjLoadJoint(DP(HSD_Joint, mnStageSel_804D6C98->x50.joint));
    HSD_GObjObject_80390A70(gobj, HSD_GObj_JObjKind, jobj);
    GObj_SetupGXLink(gobj, HSD_GObj_JObjCallback, 4, 0x82);
    HSD_GObj_SetupProc(gobj, mn_8022EAE0, 0);
    HSD_JObjAddAnimAll(jobj,
                       DP(HSD_AnimJoint, mnStageSel_804D6C98->x50.animjoint),
                       DP(HSD_MatAnimJoint,
                          mnStageSel_804D6C98->x50.matanim_joint),
                       DP(HSD_ShapeAnimJoint,
                          mnStageSel_804D6C98->x50.shapeanim_joint));
    *out = gobj->hsd_obj;
}

static inline void make_icon_root(HSD_JObj** icons)
{
    HSD_GObj* gobj;
    HSD_JObj* jobj;
    gobj = GObj_Create(4, 5, 0x80);
    jobj = HSD_JObjLoadJoint(DP(HSD_Joint, mnStageSel_804D6C98->x90.joint));
    HSD_GObjObject_80390A70(gobj, HSD_GObj_JObjKind, jobj);
    GObj_SetupGXLink(gobj, HSD_GObj_JObjCallback, 4, 0x82);
    HSD_GObj_SetupProc(gobj, mn_8022EAE0, 4);
    HSD_JObjAddAnimAll(jobj,
                       DP(HSD_AnimJoint, mnStageSel_804D6C98->x90.animjoint),
                       DP(HSD_MatAnimJoint,
                          mnStageSel_804D6C98->x90.matanim_joint),
                       DP(HSD_ShapeAnimJoint,
                          mnStageSel_804D6C98->x90.shapeanim_joint));
    icons[0] = GET_JOBJ(gobj)->child;
    HSD_JObjReqAnimAll(icons[0], 0.0F);
    HSD_JObjAnimAll(icons[0]);
}

static inline HSD_JObj* get_jobj(HSD_GObj* gobj)
{
    HSD_JObj* jobj = GET_JOBJ(gobj);
    return jobj;
}

void mnStageSel_Scene_OnEnter(void* arg0)
{
    HSD_JObj* spDC[0x13];
    u8 _[0xDC - 0xD8];
    Vec3 spCC;

    int i;
    struct DISC_STRUCT {
        DISC_PTR(HSD_CObjDesc) unk0;
        DISC_PTR(HSD_LightDesc) unk4;
        DISC_PTR(HSD_LightDesc) unk8;
        DISC_PTR(HSD_FogDesc) unkC;
        struct mnStageSel_804D6C98_t x10;
    }* temp_r3;

    PAD_STACK(0xDC - 0x50);

    sss_data = (SSSData*) arg0;

    if (sss_data->force_stage_id < 0) {
        if (lbLang_IsSavedLanguageUS() != 0) {
            mnStageSel_804D6C94 = lbArchive_LoadArchive("MnSlMap.usd");
        } else {
            mnStageSel_804D6C94 = lbArchive_LoadArchive("MnSlMap.dat");
        }
        temp_r3 = HSD_ArchiveGetPublicAddress(mnStageSel_804D6C94,
                                              "MnSelectStageDataTable");
        MenMain_cam = DP(HSD_CObjDesc, temp_r3->unk0);
        mnStageSel_804D6C98 = &temp_r3->x10;
        mnStageSel_804D6CAF = 0;
        mnStageSel_804D6CA0 = 0;
        mnStageSel_804D6CAC = 0;
        mnStageSel_804D6CAD = 0;
        mnStageSel_804D6CAE = 0x1E;
        mnStageSel_804D50A0 = sss_data->unk_stage - 1;
#ifdef TARGET_PC
        if (netStageSel_Active()) {
            if (!pc_rank_session_active()) {
                mnStageSel_804D50A0 = pc_net_local_player();
                netStageSel_Reset();
            } else {
                mnStageSel_804D50A0 = -1;
            }
        }
#endif
        mnStageSel_804D6CA4 = 0x14;

        {
            HSD_GObj* gobj = mnStageSel_804D6C9C = GObj_Create(2, 3, 0x80);
            HSD_CObj* cobj = HSD_CObjLoadDesc(MenMain_cam);
            HSD_GObjObject_80390A70(gobj, HSD_GObj_CameraKind, cobj);
            GObj_SetupGXLinkMax(gobj, HSD_GObj_803910D8, 0);
            gobj->gxlink_prios = 0x11;
            HSD_GObj_SetupProc(gobj, mn_8022BA1C, 5);
        }

        {
            HSD_GObj* gobj;
            HSD_LObj* lobj1;
            HSD_LObj* lobj2;
            gobj = GObj_Create(3, 4, 0x80);
            lobj1 = HSD_LObjLoadDesc(DP(HSD_LightDesc, temp_r3->unk4));
            lobj2 = HSD_LObjLoadDesc(DP(HSD_LightDesc, temp_r3->unk8));
            HSD_LObjSetNext(lobj1, lobj2);
            HSD_GObjObject_80390A70(gobj, (u8) HSD_GObj_LightKind, lobj1);
            GObj_SetupGXLink(gobj, HSD_GObj_LObjCallback, 0, 0x80);
        }

        {
            HSD_GObj* gobj = GObj_Create(0xE, 0xF, 0);
            HSD_Fog* fog = HSD_FogLoadDesc(DP(HSD_FogDesc, temp_r3->unkC));
            HSD_GObjObject_80390A70(gobj, HSD_GObj_FogKind, fog);
            GObj_SetupGXLink(gobj, fn_8025A974, 0, 0x80);
        }

        {
            HSD_JObj* jobj2;
            HSD_GObj* gobj;
            gobj = GObj_Create(4, 5, 0x80);
            attach_menu_model(gobj);
            {
                HSD_GObj* g = gobj;
                jobj2 = GET_JOBJ(g);
                HSD_GObj_SetupProc(g, fn_8025A91C, 0);
                HSD_JObjReqAnimAll(jobj2, 0.0F);
                HSD_JObjAnimAll(jobj2);
            }
        }

        {
            HSD_JObj* temp_r22_4;
            make_bg_model(&temp_r22_4);
            HSD_JObjReqAnimAll(temp_r22_4, 0.0F);
            HSD_JObjAnimAll(temp_r22_4);
        }

        make_icon_root(spDC);

        for (i = 0; i < 0x12; i++) {
            spDC[i + 1] = spDC[i]->next;
            HSD_JObjReqAnimAll(spDC[i + 1], 0.0F);
            HSD_JObjAnimAll(spDC[i + 1]);
        }

        for (i = 0; i < 0x1D; i++) {
            mnStageSel_803F06D0[i].x8 =
                gm_80164430(mnStageSel_803F06D0[i].stkind) ? 2 : 1;
        }

        for (i = 0; i <= 0xA; i++) {
            HSD_JObj* temp_r22_6;
            HSD_JObj* jobj;
            make_stage_icon(&temp_r22_6);
            jobj = temp_r22_6;
            lb_8000C1C0(jobj, spDC[i]);
            mnStageSel_803F06D0[i * 2].x0 = temp_r22_6->child->next;
            switch (mnStageSel_803F06D0[i * 2].x8) {
            case 0:
                HSD_JObjSetFlags(mnStageSel_803F06D0[i * 2].x0, JOBJ_HIDDEN);
                break;
            case 1:
                HSD_JObjReqAnimAllByFlags(mnStageSel_803F06D0[i * 2].x0, 0x10,
                                          1.0F);
                break;
            default:
                HSD_JObjReqAnimAllByFlags(mnStageSel_803F06D0[i * 2].x0, 0x10,
                                          mnStageSel_803F06D0[i * 2].x9 / 2 +
                                              2);
                break;
            }
            mnStageSel_803F06D0[i * 2 + 1].x0 = temp_r22_6->child;
            switch (mnStageSel_803F06D0[i * 2 + 1].x8) {
            case 0:
                HSD_JObjSetFlags(mnStageSel_803F06D0[i * 2 + 1].x0,
                                 JOBJ_HIDDEN);
                break;
            case 1:
                HSD_JObjReqAnimAllByFlags(mnStageSel_803F06D0[i * 2 + 1].x0,
                                          0x10, 1.0F);
                break;
            default:
                HSD_JObjReqAnimAllByFlags(
                    mnStageSel_803F06D0[i * 2 + 1].x0, 0x10,
                    mnStageSel_803F06D0[i * 2 + 1].x9 / 2 + 2);
                break;
            }
            HSD_JObjAnimAll(jobj);
            HSD_ForeachAnim(jobj, JOBJ_TYPE, TOBJ_MASK, HSD_AObjStopAnim,
                            AOBJ_ARG_AOV, NULL);
        }

        for (i = 0xB; i <= 0xF; i++) {
            HSD_JObj* jobj;
            HSD_GObj* gobj = GObj_Create(4, 5, 0x80);
            s32 temp_r22_7;
            HSD_JObj* temp_r23_3;
            jobj = HSD_JObjLoadJoint(DP(HSD_Joint,
                                        mnStageSel_804D6C98->x20.joint));
            HSD_GObjObject_80390A70(gobj, HSD_GObj_JObjKind, jobj);
            GObj_SetupGXLink(gobj, HSD_GObj_JObjCallback, 4, 0x83);
            HSD_GObj_SetupProc(gobj, mn_8022EAE0, 3);
            HSD_JObjAddAnimAll(jobj,
                               DP(HSD_AnimJoint,
                                  mnStageSel_804D6C98->x20.animjoint),
                               DP(HSD_MatAnimJoint,
                                  mnStageSel_804D6C98->x20.matanim_joint),
                               DP(HSD_ShapeAnimJoint,
                                  mnStageSel_804D6C98->x20.shapeanim_joint));
            temp_r23_3 = gobj->hsd_obj;
            lb_8000C1C0(temp_r23_3, spDC[i]);
            mnStageSel_803F06D0[i + 13].x0 = temp_r23_3;
            switch (mnStageSel_803F06D0[i + 13].x8) {
            case 1:
                mnStageSel_803F06D0[i + 13].x8 = 0;
                /* fallthrough */
            case 0:
                HSD_JObjSetFlagsAll(temp_r23_3, JOBJ_HIDDEN);
                break;
            default:
                temp_r22_7 = mnStageSel_803F06D0[i + 13].x9 - 0x16;
                do_anim(temp_r23_3, temp_r22_7);
                break;
            }
        }

        {
            HSD_JObj* jobj;
            HSD_GObj* gobj = GObj_Create(4, 5, 0x80);
            HSD_JObj* temp_r22_8;
            jobj = HSD_JObjLoadJoint(DP(HSD_Joint,
                                        mnStageSel_804D6C98->x10.joint));
            HSD_GObjObject_80390A70(gobj, HSD_GObj_JObjKind, jobj);
            GObj_SetupGXLink(gobj, HSD_GObj_JObjCallback, 4, 0x83);
            HSD_GObj_SetupProc(gobj, mn_8022EAE0, 3);
            HSD_JObjAddAnimAll(jobj,
                               DP(HSD_AnimJoint,
                                  mnStageSel_804D6C98->x10.animjoint),
                               DP(HSD_MatAnimJoint,
                                  mnStageSel_804D6C98->x10.matanim_joint),
                               DP(HSD_ShapeAnimJoint,
                                  mnStageSel_804D6C98->x10.shapeanim_joint));
            temp_r22_8 = gobj->hsd_obj;
            lb_8000C1C0(temp_r22_8, spDC[0x10]);
            do_anim(temp_r22_8, 2);
            mnStageSel_803F06D0[0x1D].x0 = temp_r22_8;
        }

        for (i = 0x11; i <= 0x12; i++) {
            HSD_JObj* jobj;
            HSD_AnimJoint* animjoint;
            HSD_GObj* gobj = GObj_Create(4, 5, 0x80);
            s32 temp_r22_9;
            HSD_JObj* temp_r23_6;
            jobj = HSD_JObjLoadJoint(DP(HSD_Joint,
                                        mnStageSel_804D6C98->x0.joint));
            HSD_GObjObject_80390A70(gobj, HSD_GObj_JObjKind, jobj);
            GObj_SetupGXLink(gobj, HSD_GObj_JObjCallback, 4, 0x83);
            HSD_GObj_SetupProc(gobj, mn_8022EAE0, 3);
            animjoint = DP(HSD_AnimJoint, mnStageSel_804D6C98->x0.animjoint);
            HSD_JObjAddAnimAll(jobj, animjoint,
                               DP(HSD_MatAnimJoint,
                                  mnStageSel_804D6C98->x0.matanim_joint),
                               DP(HSD_ShapeAnimJoint,
                                  mnStageSel_804D6C98->x0.shapeanim_joint));

            temp_r23_6 = gobj->hsd_obj;
            lb_8000C1C0(temp_r23_6, spDC[i]);
            mnStageSel_803F06D0[i + 5].x0 = temp_r23_6;
            switch (mnStageSel_803F06D0[i + 5].x8) {
            case 1:
                mnStageSel_803F06D0[i + 5].x8 = 0;
                /* fallthrough */
            case 0:
                HSD_JObjSetFlagsAll(temp_r23_6, JOBJ_HIDDEN);
                break;
            default:
                temp_r22_9 = mnStageSel_803F06D0[i + 5].x9 - 0x14;
                do_anim(temp_r23_6, temp_r22_9);
                break;
            }
        }

        {
            HSD_JObj* jobj;
            HSD_GObj* gobj;
            gobj = GObj_Create(4, 5, 0x80);
            jobj = HSD_JObjLoadJoint(DP(HSD_Joint,
                                        mnStageSel_804D6C98->x80.joint));
            HSD_GObjObject_80390A70(gobj, HSD_GObj_JObjKind, jobj);
            GObj_SetupGXLink(gobj, HSD_GObj_JObjCallback, 4, 0x86);
            HSD_GObj_SetupProc(gobj, mn_8022EAE0, 2);
            HSD_JObjAddAnimAll(jobj,
                               DP(HSD_AnimJoint,
                                  mnStageSel_804D6C98->x80.animjoint),
                               DP(HSD_MatAnimJoint,
                                  mnStageSel_804D6C98->x80.matanim_joint),
                               DP(HSD_ShapeAnimJoint,
                                  mnStageSel_804D6C98->x80.shapeanim_joint));

            {
                HSD_JObj* jobj2;
                HSD_GObj* g;
                jobj = get_jobj(gobj);
                spCC = mnStageSel_803B8550;
                g = gobj;
                jobj2 = jobj;
                HSD_GObj_SetupProc(g, fn_8025A310, 2);
                do_anim(jobj2, mnStageSel_804D50A0 + 1);
                HSD_JObjSetTranslate(jobj2, &spCC);
            }
        }

        {
            HSD_JObj* jobj;
            HSD_GObj* gobj;
            gobj = GObj_Create(HSD_GOBJ_CLASS_FIGHTER, 5, 0x80);
            jobj = HSD_JObjLoadJoint(DP(HSD_Joint,
                                        mnStageSel_804D6C98->x30.joint));
            HSD_GObjObject_80390A70(gobj, HSD_GObj_JObjKind, jobj);
            GObj_SetupGXLink(gobj, HSD_GObj_JObjCallback, 4, 0x84);
            HSD_GObj_SetupProc(gobj, mn_8022EAE0, 1);
            HSD_JObjAddAnimAll(jobj,
                               DP(HSD_AnimJoint,
                                  mnStageSel_804D6C98->x30.animjoint),
                               DP(HSD_MatAnimJoint,
                                  mnStageSel_804D6C98->x30.matanim_joint),
                               DP(HSD_ShapeAnimJoint,
                                  mnStageSel_804D6C98->x30.shapeanim_joint));
            {
                struct StageSelUserData* userdata;
                HSD_JObj* jobj = GET_JOBJ(gobj);
                userdata = HSD_MemAlloc(sizeof(struct StageSelUserData));
                GObj_InitUserData(gobj, 4, HSD_Free, userdata);
                HSD_GObj_SetupProc(gobj, fn_80259D84, 1);
                userdata->x0 = 0x1E;
                userdata->x4 = 0;
                userdata->x2 = 0;
            }
        }

        {
            HSD_JObj* jobj;
            HSD_GObj* gobj = GObj_Create(4, 5, 0x80);
            jobj = HSD_JObjLoadJoint(DP(HSD_Joint,
                                        mnStageSel_804D6C98->x60.joint));
            HSD_GObjObject_80390A70(gobj, HSD_GObj_JObjKind, jobj);
            GObj_SetupGXLink(gobj, HSD_GObj_JObjCallback, 4, 0x81);
            HSD_GObj_SetupProc(gobj, mn_8022EAE0, 1);
            HSD_JObjAddAnimAll(jobj,
                               DP(HSD_AnimJoint,
                                  mnStageSel_804D6C98->x60.animjoint),
                               DP(HSD_MatAnimJoint,
                                  mnStageSel_804D6C98->x60.matanim_joint),
                               DP(HSD_ShapeAnimJoint,
                                  mnStageSel_804D6C98->x60.shapeanim_joint));

            {
                HSD_GObj* g;
                HSD_JObj* jobj;
                struct foo {
                    int x0, x4;
                }* temp_r3_14;
                g = gobj;
                jobj = GET_JOBJ(g);
                temp_r3_14 = HSD_MemAlloc(sizeof(*temp_r3_14));
                GObj_InitUserData(g, 4, HSD_Free, temp_r3_14);
                HSD_GObj_SetupProc(g, fn_8025A090, 1);
                HSD_JObjReqAnimAll(jobj, 0.0F);
                HSD_JObjAnimAll(jobj);
                HSD_ForeachAnim(jobj, JOBJ_TYPE, ALL_TYPE_MASK,
                                HSD_AObjStopAnim, AOBJ_ARG_AOV, NULL);
                HSD_JObjSetTranslateX(jobj, 100.0F);
                temp_r3_14->x0 = 0x1E;
                temp_r3_14->x4 = 0;
            }
        }

        {
            HSD_JObj* jobj;
            HSD_GObj* gobj = GObj_Create(4, 5, 0x80);
            HSD_JObj* temp_r3_15 =
                HSD_JObjLoadJoint(DP(HSD_Joint,
                                     mnStageSel_804D6C98->x70.joint));
            s32* temp_r3_16;
            HSD_GObjObject_80390A70(gobj, HSD_GObj_JObjKind, temp_r3_15);
            GObj_SetupGXLink(gobj, HSD_GObj_JObjCallback, 4, 0x85);
            HSD_GObj_SetupProc(gobj, mn_8022EAE0, 1);
            HSD_JObjAddAnimAll(temp_r3_15,
                               DP(HSD_AnimJoint,
                                  mnStageSel_804D6C98->x70.animjoint),
                               DP(HSD_MatAnimJoint,
                                  mnStageSel_804D6C98->x70.matanim_joint),
                               DP(HSD_ShapeAnimJoint,
                                  mnStageSel_804D6C98->x70.shapeanim_joint));
            jobj = gobj->hsd_obj;
            temp_r3_16 = HSD_MemAlloc(sizeof(*temp_r3_16));
            GObj_InitUserData(gobj, 4, HSD_Free, temp_r3_16);
            *temp_r3_16 = 0;
            HSD_GObj_SetupProc(gobj, fn_8025A560, 1);
            HSD_JObjSetTranslateX(jobj, 100.0F);
        }

        lbAudioAx_80023F28(gmMainLib_8015ECB0());
#ifdef TARGET_PC
        if (pc_rank_session_active()) rankStageCreate();
#endif
    }
}

static inline HSD_PadStatus* get_pad(u8 i)
{
    return &HSD_PadCopyStatus[i];
}

/// OnFrame
void mnStageSel_Scene_OnFrame(void)
{
#ifdef TARGET_PC
    if (rank_text) {
        if (!pc_rank_session_active()) { gm_801A4B60(); return; }
        rankStageFrame();
        return;
    }
#endif
    if (sss_data->force_stage_id >= 0) {
        mnStageSel_804D6CAF = 2;
        sss_data->vs.start.rules.stkind = sss_data->force_stage_id;
        gm_801A4B60();
        return;
    }
    if (sss_data->no_lras == 0 && mn_8022F218()) {
        sfxBack();
        lb_800145F4();
        HSD_GObjFree(mnStageSel_804D6C9C);
        mn_8022F268();
        gm_ChangeGameModeAfterCurrentScene(GM_MENU);
        gm_801A4B60();
        return;
    }
    bool b_pressed = false;
    if (mnStageSel_804D50A0 < 0) {
        mnStageSel_804D6CA0 = 0;
        mnStageSel_804D6CA0 |= HSD_PadCopyStatus[0].trigger;
        mnStageSel_804D6CA0 |= HSD_PadCopyStatus[1].trigger;
        mnStageSel_804D6CA0 |= HSD_PadCopyStatus[2].trigger;
        mnStageSel_804D6CA0 |= HSD_PadCopyStatus[3].trigger;
        b_pressed = (mnStageSel_804D6CA0 & 0x200) != 0;
        {
            int i;
            for (i = 0; i < 4; i++) {
                mnStageSel_804D6CAC = get_pad(i)->stickX;
                mnStageSel_804D6CAD = get_pad(i)->stickY;
                if (get_pad(i)->stickX < -0x1E || get_pad(i)->stickX > +0x1E ||
                    get_pad(i)->stickY < -0x1E || get_pad(i)->stickY > +0x1E)
                {
                    break;
                }
            }
        }
    } else {
        mnStageSel_804D6CA0 = get_pad(mnStageSel_804D50A0)->trigger;
        mnStageSel_804D6CAC = get_pad(mnStageSel_804D50A0)->stickX;
        mnStageSel_804D6CAD = get_pad(mnStageSel_804D50A0)->stickY;
#ifdef TARGET_PC
        if (netStageSel_Active() && !pc_rank_session_active()) {
            b_pressed = (HSD_PadCopyStatus[0].trigger & 0x200) ||
                        (HSD_PadCopyStatus[1].trigger & 0x200);
        } else
#endif
        {
            b_pressed = (mnStageSel_804D6CA0 & 0x200) != 0;
        }
    }
    if (mnStageSel_804D6CAC < -0x1E) {
        mnStageSel_804D6CAC += 0x1E;
    } else if (mnStageSel_804D6CAC > 0x1E) {
        mnStageSel_804D6CAC -= 0x1E;
    } else {
        mnStageSel_804D6CAC = 0;
    }
    if (mnStageSel_804D6CAD < -0x1E) {
        mnStageSel_804D6CAD += 0x1E;
    } else if (mnStageSel_804D6CAD > 0x1E) {
        mnStageSel_804D6CAD -= 0x1E;
    } else {
        mnStageSel_804D6CAD = 0;
    }
    if (mnStageSel_804D6CA4 != 0) {
        mnStageSel_804D6CA4 -= 1;
        return;
    }
    if (sss_data->x1 == 0 && b_pressed && mnStageSel_804D6CAF == 0)
    {
        sfxBack();
        gm_801A4B60();
    }
    if (mnStageSel_804D6CAF == 2) {
#ifdef TARGET_PC
        if (netStageSel_Active() && !pc_rank_session_active()) {
            netStageSel_Poll();
            if (pc_net_peer_status() != 0) {
                gm_801A4B60();
                return;
            }
            if (net_remote_pick < 0) {
                return; /* opponent still choosing; keep showing our pick */
            }
            mnStageSel_804D6CAE = netStageSel_Resolve();
            if (mnStageSel_804D6CAE >= NUM_STAGES) {
                mnStageSel_804D6CAE = netStageSel_Random();
            }
        }
#endif
        /* Never index the sentinel cells, including in offline play. */
        if (mnStageSel_804D6CAE < 0 || mnStageSel_804D6CAE >= NUM_STAGES) {
            mnStageSel_804D6CAE = mnStageSel_802599EC();
        }
        sss_data->vs.start.rules.stkind =
            mnStageSel_803F06D0[mnStageSel_804D6CAE].stkind;
#ifdef TARGET_PC
        if (netStageSel_Active())
            pc_log_line("sss: resolved cell %d stage %d", mnStageSel_804D6CAE,
                        sss_data->vs.start.rules.stkind);
#endif
        gm_801A4B60();
    }
}

void mnStageSel_Scene_OnExit(UNUSED void* exit_data)
{
#ifdef TARGET_PC
    if (rank_text) { HSD_SisLib_803A5CC4(rank_text); rank_text = NULL; }
#endif
    if (mnStageSel_804D6C94 != NULL) {
        lbArchive_80016EFC(mnStageSel_804D6C94);
        mnStageSel_804D6C94 = NULL;
    }
    {
        SSSData* sss = sss_data;
        sss->start_game = mnStageSel_804D6CAF == 2 ? true : false;
        if (sss->start_game) {
            PreloadedGameModeState* cache = lbDvd_GetPreloadCacheScene();
            cache->game_cache.stkind = sss->vs.start.rules.stkind;
            lbDvd_80018254();
        }
    }
}

int mnSelStageRandom(void)
{
    return mnStageSel_803F06D0[mnStageSel_802599EC()].stkind;
}

int mnStageSel_8025BC08(int idx)
{
    return mnStageSel_803F06D0[idx].stkind;
}
