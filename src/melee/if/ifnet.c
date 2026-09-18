#include "ifnet.h"

#ifdef TARGET_PC
#include "forward.h"
#include "ifall.h"
#include <sysdolphin/baselib/gobj.h>
#include <sysdolphin/baselib/gobjplink.h>
#include <sysdolphin/baselib/gobjproc.h>
#include <sysdolphin/baselib/sislib.h>

#include "pc/net.h"
#include "pc/pc.h"
#include "pc/widescreen.h"

#include <stdlib.h>

/* Same recipe as the score text in if_2FF2.c (un_802FF498/un_802FF364): a
 * SIS canvas on font 2 (SdIntro.dat, loaded by ifnametag.c un_802FD4C8)
 * parented to the HUD camera, one dynamic text with one formatted entry.
 * Entry coordinates are HUD world units, y down (hsd_3A76.c negates y);
 * 1 px = 0.09125 units in x (widescreen.c PC_HUD_WORLD_SCALE), 0.1 in y. */
#define IFNET_X -27 /* 24 px in from the 4:3 left edge */
#define IFNET_Y -22 /* 20 px down from the top edge */
#define IFNET_SCALE 0.05f /* 32 px glyphs -> 16 px */

static struct {
    HSD_GObj* gobj;
    HSD_Text* text;
    int entry;
    int player;
    int ping;
    int delay;
    unsigned rollbacks;
} ifNet;

static void ifNet_Think(HSD_GObj* gobj)
{
    int ping, delay;
    unsigned rollbacks;
    if (!pc_net_stats(&ping, &delay, &rollbacks)) {
        return;
    }
    if (ping == ifNet.ping && delay == ifNet.delay &&
        rollbacks == ifNet.rollbacks)
    {
        return;
    }
    ifNet.ping = ping;
    ifNet.delay = delay;
    ifNet.rollbacks = rollbacks;
    HSD_SisLib_803A70A0(ifNet.text, ifNet.entry,
                        "P%d  delay %d  ping %dms  rb %u", ifNet.player,
                        delay, ping, rollbacks);
}

void ifNet_Create(void)
{
    int ping, delay;
    unsigned rollbacks;
    int canvas;
    const char* player;

    ifNet.text = NULL;
    ifNet.gobj = NULL;
    if (!pc_net_stats(&ping, &delay, &rollbacks)) {
        return;
    }
    /* ponytail: mirrors net.c's MELEE_NET_PLAYER parse; swap for an
     * accessor when net.h grows one. */
    player = getenv("MELEE_NET_PLAYER");
    ifNet.player = player && player[0] == '1' ? 2 : 1;
    ifNet.ping = ping;
    ifNet.delay = delay;
    ifNet.rollbacks = rollbacks;

    canvas = HSD_SisLib_803A611C(2, ifAll_GetHUDGObj(), HSD_GOBJ_CLASS_UI, 15,
                                 0, 11, 0, 19);
    ifNet.text = HSD_SisLib_803A6754(2, canvas);
    ifNet.text->default_kerning = 1;
    ifNet.entry = HSD_SisLib_803A6B98(
        ifNet.text, pc_widescreen_hud_player_x(0, 2, IFNET_X), IFNET_Y,
        "P%d  delay %d  ping %dms  rb %u", ifNet.player, delay, ping,
        rollbacks);
    HSD_SisLib_803A7548(ifNet.text, ifNet.entry, IFNET_SCALE, IFNET_SCALE);
    /* TEMP calibration: glyph tops at (0,0) and (-20,-20) in SIS units */
    {
        int a = HSD_SisLib_803A6B98(ifNet.text, 0, -32, "I");
        int b = HSD_SisLib_803A6B98(ifNet.text, -20, -52, "I");
        HSD_SisLib_803A7548(ifNet.text, a, 1.0f, 1.0f);
        HSD_SisLib_803A7548(ifNet.text, b, 1.0f, 1.0f);
        pc_log_line("net: hud p0=(%f,%f) timer=(%f,%f) hudmode=%d",
                    ifAll_GetPlayerHUDPosition(0)->x,
                    ifAll_GetPlayerHUDPosition(0)->y,
                    ifAll_GetTimerPosition()->x, ifAll_GetTimerPosition()->y,
                    pc_get_hud_mode());
    }

    ifNet.gobj = GObj_Create(HSD_GOBJ_CLASS_UI, 15, 0);
    HSD_GObj_SetupProc(ifNet.gobj, ifNet_Think, 17);
}

void ifNet_Free(void)
{
    if (ifNet.gobj != NULL) {
        HSD_GObjFree(ifNet.gobj);
        ifNet.gobj = NULL;
    }
    if (ifNet.text != NULL) {
        HSD_SisLib_803A5CC4(ifNet.text);
        ifNet.text = NULL;
    }
}
#endif
