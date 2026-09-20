#include "gmboot.h"

#include "gm_unsplit.h"
#include "gmmain_lib.h"
#include "types.h"
#include <melee/lb/lbcardgame.h>
#include <melee/lb/lbcardnew.h>
#include <melee/lb/lblanguage.h>
#include <melee/ty/toy.h>
#ifdef TARGET_PC
#include "gmonlinemode.h"
#include <stdlib.h>
#include <string.h>
#endif

/* 1BF948 */ static void bootOnLoad(GameModeState*);
/* 1BF9A8 */ static void bootOnLeave(GameModeState*);
/* 1BFA3C */ static void memcardOnLoad(GameModeState*);

/// @todo Move to toy header
enum {
    TROPHY_PIKMIN = 275,
};

struct loadData {
    u32 x0;
    u8 x4;
    u8 mode_id; ///< Copied to ::leaveData::mode_id to set next mode
};

struct leaveData {
    u32 x0;
    u8 mode_id;
};

static struct loadData load_data;
static struct leaveData leave_data;

#ifdef TARGET_PC
/* MELEE_BOOT_SCENE=<title|vs|classic|training|unranked|direct|ranked>: skip the whole menu walk and
 * boot into one scene with a fixed setup. Menu navigation here can only be
 * driven by synthetic input, which misses keypresses often enough that an
 * automated run cannot rely on it (see tools/smoke_test.py).
 *
 * vs maps to GM_DEBUG_VS because that mode already *is* a fixed direct start
 * (onEnterDebugVs in gmvsmode.c fills the StartMeleeData itself). classic and
 * training still open on a character-select state, so their on_load hooks
 * seed the pick and jump past it. */
u8 pc_boot_scene(void)
{
    static int done;
    static u8 scene = GM_COUNT;

    if (!done) {
        const char* want = getenv("MELEE_BOOT_SCENE");
        done = 1;
        if (want == NULL || want[0] == '\0') {
            /* nothing */
        } else if (strcmp(want, "title") == 0) {
            scene = GM_TITLE;
        } else if (strcmp(want, "vs") == 0) {
            scene = GM_DEBUG_VS;
        } else if (strcmp(want, "classic") == 0) {
            scene = GM_CLASSIC;
        } else if (strcmp(want, "training") == 0) {
            scene = GM_TRAINING;
        } else if (strcmp(want, "unranked") == 0) {
            scene = GM_ONLINE;
            gmOnline_SetKind(ONLINE_KIND_UNRANKED);
        } else if (strcmp(want, "direct") == 0) {
            scene = GM_ONLINE;
            gmOnline_SetKind(ONLINE_KIND_DIRECT);
        } else if (strcmp(want, "ranked") == 0) {
            scene = GM_ONLINE;
            gmOnline_SetKind(ONLINE_KIND_RANKED);
        } else {
            OSReport("MELEE_BOOT_SCENE: unknown scene '%s'; valid values are "
                     "title, vs, classic, training, unranked, direct, ranked\n",
                     want);
        }
    }
    return scene;
}
#endif

GameModeState gm_Mode_Boot_States[] = {
    {
        0,
        1,
        0,
        bootOnLoad,
        bootOnLeave,
        GS_MEMCARD,
        &load_data,
        &leave_data,
    },
    {
        -1,
    },
};

void bootOnLoad(GameModeState* scene)
{
    struct loadData* scene_data = gm_GetGameModeStateEnterData(scene);
    scene_data->x4 = 0;
    scene_data->x0 = 0;
    if (gmMainLib_8046B0F0.skip_intro == true) {
        scene_data->mode_id = GM_TITLE;
    } else {
        gm_801BF708(0);
        scene_data->mode_id = GM_OPENING_MV;
    }
}

void bootOnLeave(GameModeState* data)
{
    struct leaveData* scene_data = gm_GetGameModeStateExitData(data);

    if (!Toy_803048C0(TROPHY_PIKMIN)) {
        if (!lb_8001C2D8(0, "01",
                         lbLang_GetLanguageSetting() == LANG_JP ? "GPIJ"
                                                                : "GPIE",
                         "Pikmin dataFile"))
        {
            Toy_803124BC();
            Toy_SetUnlockState(TROPHY_PIKMIN, true);
        }
    }

    gm_SetGameModeOverride(lbCardGame_DecideGameMode);

    // Enter mode
    // Gekko "boot to CSS" code changes scene_id to a hardcoded 2 (::GM_VS)
    gm_ChangeGameModeAfterCurrentScene(scene_data->mode_id);
}

GameModeState gm_Mode_MemCard_States[] = {
    {
        0,
        3,
        0,
        memcardOnLoad,
        NULL,
        {
            GS_MEMCARD,
            &load_data,
            &leave_data,
        },
    },
    { -1 },
};

void memcardOnLoad(GameModeState* scene)
{
    struct loadData* temp_r3 = gm_GetGameModeStateEnterData(scene);
    temp_r3->x4 = 0;
    temp_r3->x0 = 1;
}
