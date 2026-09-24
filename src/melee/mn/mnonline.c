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
#ifdef TARGET_PC
#include "pc/net_match.h"
#endif

/* VS Mode > Online. Each entry selects its GM_ONLINE lobby mode. */

static const char* const online_labels[] = {
    "LAN PLAY", "DIRECT CONNECT", "RANKED", "UNRANKED", "PROFILE",
};

/* <= 44 chars: that is what fits the bottom bar at mnmain.c's font size */
static const char* const online_descriptions[] = {
    "Play another player on your local network.",
    "Connect to a friend using a connect code.",
    "Play a rated best-of-three set.",
    "Find an opponent over the internet.",
    "View your player identity and connect code.",
};

static const char* notice;

const char* mnOnline_Label(MenuKind kind, int selection)
{
    if (kind == MENU_KIND_VS && selection == SEL_VS_ONLINE) {
        return "ONLINE";
    }
    if (kind == MENU_KIND_ONLINE && selection >= 0 &&
        selection < (int) ARRAY_SIZE(online_labels))
    {
        return online_labels[selection];
    }
    return NULL;
}

const char* mnOnline_Description(MenuKind kind, int selection)
{
    if (kind == MENU_KIND_VS && selection == SEL_VS_ONLINE) {
        return "Play against other players over the network.";
    }
    if (kind == MENU_KIND_ONLINE && selection >= 0 &&
        selection < (int) ARRAY_SIZE(online_descriptions))
    {
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

#ifdef TARGET_PC
    /* Open and bootstrap the DHT node while the player is still choosing, so
     * Direct/Unranked/Ranked start searching with a populated routing table.
     * The lobby takes it over (or closes it for LAN) on entry. */
    pc_net_match_warm();
#endif

    mn_804A04F0.buttons = buttons;
    if (buttons & MenuInput_Confirm) {
        switch (mn_804A04F0.hovered_selection) {
        case SEL_ONLINE_LAN:
            enterOnline(ONLINE_KIND_LAN);
            break;
        case SEL_ONLINE_DIRECT:
            enterOnline(ONLINE_KIND_DIRECT);
            break;
        case SEL_ONLINE_UNRANKED:
            enterOnline(ONLINE_KIND_UNRANKED);
            break;
        case SEL_ONLINE_RANKED:
            enterOnline(ONLINE_KIND_RANKED);
            break;
        case SEL_ONLINE_PROFILE:
            enterOnline(ONLINE_KIND_PROFILE);
            break;
        default:
            break;
        }
    } else if (buttons & MenuInput_Back) {
        sfxBack();
#ifdef TARGET_PC
        pc_net_match_stop(); /* nothing polls it outside this menu */
#endif
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
