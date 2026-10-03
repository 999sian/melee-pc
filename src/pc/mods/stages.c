/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pc/compat.h"

#include "stages.h"

#include "image.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <melee/gr/forward.h>
#include <melee/gr/stage.h>

#define MAX_STAGE_PACKS 128

typedef struct StagePack {
    char* id;
    char* name;
    char* mod_id;
    int base;
    char* file;
    char* music;
    PcImage icon, preview, name_image;
} StagePack;

static StagePack s_stages[MAX_STAGE_PACKS];
static int s_count;
static int s_active = -1;

/* Every stage on the VS stage select, with the names a modder might use. */
static const struct {
    const char* name; /* folded: lowercase alphanumerics */
    int stkind;
} s_names[] = {
    {"fountainofdreams", St_Kind_Izumi},
    {"izumi", St_Kind_Izumi},
    {"pokemonstadium", St_Kind_PStadium},
    {"pstadium", St_Kind_PStadium},
    {"princesspeachscastle", St_Kind_Castle},
    {"peachscastle", St_Kind_Castle},
    {"castle", St_Kind_Castle},
    {"kongojungle", St_Kind_Kongo},
    {"kongo", St_Kind_Kongo},
    {"brinstar", St_Kind_Zebes},
    {"zebes", St_Kind_Zebes},
    {"corneria", St_Kind_Corneria},
    {"yoshisstory", St_Kind_Story},
    {"story", St_Kind_Story},
    {"onett", St_Kind_Onett},
    {"mutecity", St_Kind_MuteCity},
    {"rainbowcruise", St_Kind_RCruise},
    {"rcruise", St_Kind_RCruise},
    {"junglejapes", St_Kind_Garden},
    {"garden", St_Kind_Garden},
    {"greatbay", St_Kind_GreatBay},
    {"hyruletemple", St_Kind_Shrine},
    {"shrine", St_Kind_Shrine},
    {"brinstardepths", St_Kind_Kraid},
    {"kraid", St_Kind_Kraid},
    {"yoshisisland", St_Kind_Yoster},
    {"yoster", St_Kind_Yoster},
    {"greengreens", St_Kind_Greens},
    {"greens", St_Kind_Greens},
    {"fourside", St_Kind_Fourside},
    {"mushroomkingdom", St_Kind_Inishie1},
    {"inishie1", St_Kind_Inishie1},
    {"mushroomkingdomii", St_Kind_Inishie2},
    {"mushroomkingdom2", St_Kind_Inishie2},
    {"inishie2", St_Kind_Inishie2},
    {"venom", St_Kind_Venom},
    {"pokefloats", St_Kind_Pura},
    {"pura", St_Kind_Pura},
    {"bigblue", St_Kind_BigBlue},
    {"iciclemountain", St_Kind_Icemt},
    {"icemt", St_Kind_Icemt},
    {"flatzone", St_Kind_Flatzone},
    {"dreamland", St_Kind_OldPupupu},
    {"oldpupupu", St_Kind_OldPupupu},
    {"yoshisislandn64", St_Kind_OldYoshi},
    {"oldyoshi", St_Kind_OldYoshi},
    {"kongojunglen64", St_Kind_OldKongo},
    {"oldkongo", St_Kind_OldKongo},
    {"battlefield", St_Kind_Battle},
    {"battle", St_Kind_Battle},
    {"finaldestination", St_Kind_Last},
    {"last", St_Kind_Last},
};

static char* dup(const char* s) {
    if (s == NULL)
        return NULL;
    size_t n = strlen(s) + 1;
    char* d = (char*)malloc(n);
    if (d)
        memcpy(d, s, n);
    return d;
}

int pc_stage_from_name(const char* name) {
    char folded[64];
    size_t n = 0;
    if (name == NULL)
        return -1;
    for (const char* c = name; *c && n < sizeof(folded) - 1; ++c)
        if (isalnum((unsigned char)*c))
            folded[n++] = (char)tolower((unsigned char)*c);
    folded[n] = '\0';
    for (size_t i = 0; i < sizeof(s_names) / sizeof(s_names[0]); ++i)
        if (strcmp(s_names[i].name, folded) == 0)
            return s_names[i].stkind;
    return -1;
}

const char* pc_stage_display_name(int stkind) {
    /* The first alias of each stage is its English name, folded; good
     * enough for log lines. */
    for (size_t i = 0; i < sizeof(s_names) / sizeof(s_names[0]); ++i)
        if (s_names[i].stkind == stkind)
            return s_names[i].name;
    return "?";
}

int pc_stages_add(const PcStagePackDesc* d, char* err, size_t err_size) {
#define FAIL(...)                                                                                  \
    do {                                                                                           \
        if (err && err_size)                                                                       \
            snprintf(err, err_size, __VA_ARGS__);                                                  \
        return -1;                                                                                 \
    } while (0)
    if (d == NULL || d->id == NULL || d->file == NULL)
        FAIL("a stage needs \"id\" and \"file\"");
    if (d->base_stkind < 0)
        FAIL("unknown base stage");
    if (s_count >= MAX_STAGE_PACKS)
        FAIL("too many map packs (limit %d)", MAX_STAGE_PACKS);
    for (int i = 0; i < s_count; ++i)
        if (strcmp(s_stages[i].id, d->id) == 0)
            FAIL("a stage with id \"%s\" is already registered", d->id);
    StagePack* s = &s_stages[s_count];
    memset(s, 0, sizeof(*s));
    s->id = dup(d->id);
    s->name = dup(d->name ? d->name : d->id);
    s->mod_id = dup(d->mod_id ? d->mod_id : "");
    s->base = d->base_stkind;
    /* The game opens stage files by absolute disc path ("/GrNBa.dat"). */
    s->file = d->file[0] == '/' ? dup(d->file) : NULL;
    if (s->file == NULL) {
        size_t n = strlen(d->file) + 2;
        s->file = (char*)malloc(n);
        if (s->file)
            snprintf(s->file, n, "/%s", d->file);
    }
    s->music = dup(d->music_path);
    s->icon.path = dup(d->icon_path);
    s->preview.path = dup(d->preview_path);
    s->name_image.path = dup(d->name_image_path);
    if (!s->id || !s->name || !s->file)
        FAIL("out of memory");
    return s_count++;
#undef FAIL
}

static StagePack* at(int i) {
    return i >= 0 && i < s_count ? &s_stages[i] : NULL;
}

int pc_stages_count(void) {
    return s_count;
}

const char* pc_stages_id(int i) {
    return at(i) ? at(i)->id : NULL;
}

const char* pc_stages_name(int i) {
    return at(i) ? at(i)->name : NULL;
}

int pc_stages_base(int i) {
    return at(i) ? at(i)->base : -1;
}

const char* pc_stages_music(int i) {
    return at(i) ? at(i)->music : NULL;
}

const void* pc_stages_icon_gx(int i, int w, int h) {
    return at(i) ? pc_image_gx(&at(i)->icon, at(i)->id, w, h) : NULL;
}

const void* pc_stages_preview_gx(int i, int w, int h) {
    return at(i) ? pc_image_gx(&at(i)->preview, at(i)->id, w, h) : NULL;
}

const void* pc_stages_name_gx(int i, int w, int h) {
    return at(i) ? pc_image_gx(&at(i)->name_image, at(i)->id, w, h) : NULL;
}

void pc_stages_set_active(int pack) {
    s_active = at(pack) ? pack : -1;
}

const char* pc_stages_file_for(int grkind) {
    StagePack* s = at(s_active);
    if (s == NULL)
        return NULL;
    return (int)Stage_8022519C((StKind)s->base) == grkind ? s->file : NULL;
}

static uint32_t fnv(uint32_t h, const char* s) {
    for (; s && *s; ++s) {
        h ^= (unsigned char)*s;
        h *= 16777619u;
    }
    h ^= 0xFF;
    h *= 16777619u;
    return h;
}

uint32_t pc_stages_hash(uint32_t h) {
    for (int i = 0; i < s_count; ++i) {
        char base[8];
        snprintf(base, sizeof(base), "%d", s_stages[i].base);
        h = fnv(h, s_stages[i].id);
        h = fnv(h, base);
        h = fnv(h, s_stages[i].file);
    }
    return h;
}
