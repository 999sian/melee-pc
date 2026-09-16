/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pc/region.h"

#include <aurora/dvd.h>
#include <dolphin/os.h>
#include <stdio.h>
#include <string.h>

static bool s_is_pal;

void pc_region_set(const char* game_id)
{
    s_is_pal = memcmp(game_id, "GALP01", 6) == 0;
    aurora_dvd_set_locale_extension(s_is_pal ? "ukd" : NULL);
}

bool pc_region_is_pal(void)
{
    return s_is_pal;
}

/* All big-endian, read in place by the game. */
static const unsigned char k_trophy_row_end[0x24] = { 0xFF, 0xFF, 0xFF, 0xFF }; /* TrophyData.id == -1 */
static const unsigned char k_s16_end[2] = { 0xFF, 0xFF };                       /* DiscS16 -1 */

static const struct {
    const char* name;
    const void* data;
} k_pal_stand_ins[] = {
    /* TyDatai.usd only; PAL's TyDatai.ukd has the plain three tables. */
    { "tyInitModelDTbl", k_trophy_row_end },
    { "tyExpDifferentTbl", k_s16_end },
    { "tyNoGetUsTbl", k_s16_end },
    { "tyDisplayModelUsTbl", k_trophy_row_end }, /* loaded, never read */
};

const void* pc_region_missing_symbol(const char* symbol_name)
{
    if (!s_is_pal) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof k_pal_stand_ins / sizeof k_pal_stand_ins[0]; i++) {
        if (strcmp(k_pal_stand_ins[i].name, symbol_name) == 0) {
            OSReport("PAL: substituting empty %s\n", symbol_name);
            return k_pal_stand_ins[i].data;
        }
    }
    return NULL;
}
