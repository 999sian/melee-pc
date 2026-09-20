#include "ifnet.h"

#ifdef TARGET_PC
#include "forward.h"
#include "ifall.h"
#include <sysdolphin/baselib/gobj.h>
#include <sysdolphin/baselib/gobjplink.h>
#include <sysdolphin/baselib/gobjproc.h>
#include <sysdolphin/baselib/sislib.h>

#include "pc/net.h"
#include "pc/net_match.h"
#include "pc/net_chat.h"
#include "pc/widescreen.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* Same recipe as the score text in if_2FF2.c (un_802FF498/un_802FF364): a
 * SIS canvas on font 2 (SdIntro.dat, loaded by ifnametag.c un_802FD4C8)
 * parented to the HUD camera, one dynamic text with one formatted entry.
 * Entry coordinates are HUD world units, origin at screen centre, y down
 * (hsd_3A76.c negates y), 1 unit = 10 logical px (measured). The line box
 * is measured before the per-entry scale applies (HSD_SisLib_803A8134), so
 * the glyph cell's bottom edge always sits at y + 32 whatever the scale. */
#define IFNET_X -29 /* 30 px in from the 4:3 left edge */
#define IFNET_Y -51 /* cell bottom at -19: 50 px down from the top edge */
#define IFNET_SCALE 0.04f /* 32-unit glyphs -> 12.8 px */

static struct {
    HSD_GObj* gobj;
    HSD_Text* text;
    int entry;
    int chat_entry;
    bool debug;
    char line[160], chat[80];
} ifNet;

/* pc_net_quality() 0/1/2 -> nothing / "!" / "!!". The SIS ASCII encoder
 * (HSD_SisLib_803A67EC) has no '!', so spell the fullwidth SJIS pair. */
#define SJIS_BANG "\x81\x49"
static const char* ifNet_Marker(int quality)
{
    return quality >= 2 ? "  " SJIS_BANG SJIS_BANG
           : quality == 1 ? "  " SJIS_BANG : "";
}

static void ifNet_Think(HSD_GObj* gobj)
{
    (void)gobj;
    int ping, delay;
    unsigned rollbacks;
    if (!pc_net_stats(&ping, &delay, &rollbacks)) return;
    char line[160], opponent[32], debug[32] = "";
    const char* code = pc_net_match_state(NULL) == PC_MATCH_READY ?
                       pc_net_match_opponent_code() : NULL;
    if (code && code[0]) snprintf(opponent, sizeof opponent, "%s", code);
    else snprintf(opponent, sizeof opponent, "P%d", 2 - pc_net_local_player());
    if (ifNet.debug) snprintf(debug, sizeof debug, "  rb %u", rollbacks);
    snprintf(line, sizeof line, "P%d vs %s  delay %d  ping %dms%s%s",
             pc_net_local_player() + 1, opponent, delay, ping, debug,
             ifNet_Marker(pc_net_quality()));
    if (strcmp(line, ifNet.line)) {
        HSD_SisLib_803A70A0(ifNet.text, ifNet.entry, "%s", line);
        memcpy(ifNet.line, line, strlen(line) + 1);
    }
    const char* chat = pc_net_chat_line();
    if (strcmp(chat, ifNet.chat)) {
        HSD_SisLib_803A70A0(ifNet.text, ifNet.chat_entry, "%s", chat);
        snprintf(ifNet.chat, sizeof ifNet.chat, "%s", chat);
    }
}

void ifNet_Create(void)
{
    int ping, delay;
    unsigned rollbacks;
    ifNet.text = NULL;
    ifNet.gobj = NULL;
    if (!pc_net_stats(&ping, &delay, &rollbacks)) return;
    const char* debug = getenv("MELEE_NET_DEBUG");
    ifNet.debug = debug && debug[0] && strcmp(debug, "0");
    ifNet.line[0] = ifNet.chat[0] = 0;
    int canvas = HSD_SisLib_803A611C(2, ifAll_GetHUDGObj(), HSD_GOBJ_CLASS_UI, 15,
                                  0, 11, 0, 19);
    ifNet.text = HSD_SisLib_803A6754(2, canvas);
    ifNet.text->default_kerning = 1;
    ifNet.entry = HSD_SisLib_803A6B98(ifNet.text,
        pc_widescreen_hud_player_x(0, 2, IFNET_X), IFNET_Y, " ");
    ifNet.chat_entry = HSD_SisLib_803A6B98(ifNet.text,
        pc_widescreen_hud_player_x(0, 2, IFNET_X), IFNET_Y + 1.6f, " ");
    HSD_SisLib_803A7548(ifNet.text, ifNet.entry, IFNET_SCALE, IFNET_SCALE);
    HSD_SisLib_803A7548(ifNet.text, ifNet.chat_entry, IFNET_SCALE, IFNET_SCALE);
    ifNet_Think(NULL);
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
