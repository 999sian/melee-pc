#include "mnonline.h"

#include "forward.h"
#include "inlines.h"
#include "mnmain.h"
#include "types.h"
#include <melee/gm/gmonlinemode.h>
#include <melee/gm/gmscene.h>
#include <melee/gm/types.h>
#include <melee/lb/lbaudio_ax.h>
#include <sysdolphin/baselib/gobj.h>

/* VS Mode > Online. LAN Play and Direct Connect enter GM_ONLINE
 * (gmonlinemode.c); the rest are stubs until the DHT rendezvous lands
 * (docs/netcode-plan.md §12). */

static const char* const online_labels[] = {
    "LAN PLAY", "DIRECT CONNECT", "RANKED", "UNRANKED", "PROFILE",
};

/* <= 44 chars: that is what fits the bottom bar at mnmain.c's font size */
static const char* const online_descriptions[] = {
    "Play another player on your local network.",
    "Connect to a friend by address.",
    "Ranked matchmaking - coming soon.",
    "Unranked matchmaking - coming soon.",
    "Your name, code and rating - coming soon.",
};

static const char* notice;

const char* mnOnline_Label(MenuKind kind, int selection)
{
    if (kind == MENU_KIND_VS && selection == SEL_VS_ONLINE) {
        return "ONLINE";
    }
    if (kind == MENU_KIND_ONLINE) {
        return online_labels[selection];
    }
    return NULL;
}

const char* mnOnline_Description(MenuKind kind, int selection)
{
    if (kind == MENU_KIND_VS && selection == SEL_VS_ONLINE) {
        return "Play against other players over the network.";
    }
    if (kind == MENU_KIND_ONLINE) {
        return online_descriptions[selection];
    }
    return NULL;
}

const char* mnOnline_TakeNotice(void)
{
    const char* s = notice;
    notice = NULL;
    return s;
}

static void enterOnline(OnlineKind kind)
{
    MenuExitData* data = gm_GetCurrentSceneExitData();
    sfxForward();
    gmOnline_SetKind(kind);
    data->pending_mode = GM_ONLINE;
    gm_801A4B60();
}

/// @brief Online menu think, after mn_8022D594
void mnOnline_Think(HSD_GObj* gp)
{
    u32 buttons = mn_80229624(4);
    int count = ARRAY_SIZE(online_labels);

    mn_804A04F0.buttons = buttons;
    if (buttons & MenuInput_Confirm) {
        switch (mn_804A04F0.hovered_selection) {
        case SEL_ONLINE_LAN:
            enterOnline(ONLINE_KIND_LAN);
            break;
        case SEL_ONLINE_DIRECT:
            enterOnline(ONLINE_KIND_DIRECT);
            break;
        default:
            lbAudioAx_80024030(3);
            notice = "Coming soon.";
            break;
        }
    } else if (buttons & MenuInput_Back) {
        sfxBack();
        mn_804A04F0.entering_menu = 0;
        mn_80229894(MENU_KIND_VS, SEL_VS_ONLINE, 3);
    } else if (buttons & MenuInput_Up) {
        sfxMove();
        mn_804A04F0.hovered_selection =
            (mn_804A04F0.hovered_selection + count - 1) % count;
    } else if (buttons & MenuInput_Down) {
        sfxMove();
        mn_804A04F0.hovered_selection =
            (mn_804A04F0.hovered_selection + 1) % count;
    }
}
