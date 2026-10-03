/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Character-pack registry. A pack fighter is a new *asset kind*: it gets its
 * own entries in every asset-kind table in ftdata.c (data file, costumes,
 * animations, load caches), copied from its base fighter and then pointed at
 * the pack's files. Behaviour tables are never touched -- a pack fighter runs
 * as its base (Fighter::kind), so every move and kind check works unchanged. */
#include "pc/compat.h"

#include "roster.h"

#include "mod_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "image.h"

#include <melee/ft/ftdata.h>
#include <dolphin/os.h>
#include <melee/ft/types.h>

typedef PcImage RosterImage;

typedef struct Pack {
    char* id;
    char* name;
    char* mod_id;
    int base;
    struct StringPair data;
    char* anim_file;
    Fighter_CostumeStrings* costume_strings;
    UnkCostumeStruct* costumes;
    int costume_count;
    bool hidden; /* a partner: an asset kind, not a CSS slot */
    int partner; /* pack index of the other half, or -1 */
    char* announcer;
    char* victory_theme;
    RosterImage icon;
    RosterImage portraits[PC_ROSTER_MAX_COSTUMES];
    RosterImage stocks[PC_ROSTER_MAX_COSTUMES];
    RosterImage emblem, name_image, winner_name;
    int series;
    char* sjis_name;
} Pack;

static Pack s_packs[Ft_Kind_PackMax];
static int s_pack_count;
static const Pack* pack_at(int i);

static char* dup(const char* s) {
    if (s == NULL)
        return NULL;
    size_t n = strlen(s) + 1;
    char* d = (char*)malloc(n);
    if (d)
        memcpy(d, s, n);
    return d;
}

/* The base the second fighter of a two-fighter character runs as, or -1. */
static int partner_base(int base) {
    switch (base) {
    case Ft_Kind_Popo:
        return Ft_Kind_Nana;
    case Ft_Kind_Zelda:
        return Ft_Kind_Seak;
    case Ft_Kind_Seak:
        return Ft_Kind_Zelda;
    default:
        return -1;
    }
}

const char* pc_roster_base_unsupported(int base) {
    switch (base) {
    case Ft_Kind_Nana:
        return "Nana is the partner half of an Ice Climbers pack, not a base of her own";
    case Ft_Kind_MasterH:
    case Ft_Kind_CrezyH:
    case Ft_Kind_Boy:
    case Ft_Kind_Girl:
    case Ft_Kind_GKoops:
    case Ft_Kind_Sandbag:
        return "bosses, wireframes and Sandbag cannot be a base";
    default:
        return (base >= 0 && base < Ft_Kind_Max) ? NULL : "unknown base fighter";
    }
}

static int add_entry(
    const PcRosterFighter* d, int base, bool hidden, int max_costumes, char* err, size_t err_size) {
#define FAIL(...)                                                                                  \
    do {                                                                                           \
        if (err && err_size)                                                                       \
            snprintf(err, err_size, __VA_ARGS__);                                                  \
        return -1;                                                                                 \
    } while (0)
    if (d == NULL || d->id == NULL || d->data_file == NULL || d->anim_file == NULL)
        FAIL("a fighter needs \"id\", \"data\" and \"animations\"");
    if (s_pack_count >= Ft_Kind_PackMax)
        FAIL("too many character packs (limit %d)", Ft_Kind_PackMax);
    if (pc_roster_find_fighter(d->id) >= 0)
        FAIL("a fighter with id \"%s\" is already registered", d->id);

    const int base_costumes = CostumeListsForeachCharacter[base].numCostumes;
    int count = (int)d->costume_count;
    if (max_costumes > 0 && count > max_costumes)
        count = max_costumes;
    if (count <= 0)
        FAIL("a fighter needs at least one costume");
    if (count > PC_ROSTER_MAX_COSTUMES)
        FAIL("too many costumes (limit %d)", PC_ROSTER_MAX_COSTUMES);
    /* These bases index per-costume tables in their own code (Kirby's copy
     * hats, Jigglypuff's bows, Game & Watch's colours), sized for the disc
     * costumes; a costume beyond those would read past them. */
    if ((base == Ft_Kind_Kirby || base == Ft_Kind_Purin || base == Ft_Kind_GameWatch) &&
        count > base_costumes)
    {
        OSReport("roster: %s: %s supports at most %d costumes; using the first %d\n", d->id,
            pc_mod_fighter_name(base), base_costumes, base_costumes);
        count = base_costumes;
    }

    Pack* p = &s_packs[s_pack_count];
    memset(p, 0, sizeof(*p));
    p->hidden = hidden;
    p->partner = -1;
    p->id = dup(d->id);
    p->name = dup(d->name ? d->name : d->id);
    p->mod_id = dup(d->mod_id ? d->mod_id : "");
    p->base = base;
    p->data.a = dup(d->data_file);
    p->data.b = dup(d->data_symbol ? d->data_symbol : ftData_803C1F40[base].b);
    p->anim_file = dup(d->anim_file);
    p->icon.path = dup(d->icon_path);
    p->emblem.path = dup(d->emblem_path);
    p->name_image.path = dup(d->name_image_path);
    p->winner_name.path = dup(d->winner_name_path);
    p->series = d->series_kind;
    p->announcer = dup(d->announcer_path);
    p->victory_theme = dup(d->victory_theme_path);
    p->costume_count = count;
    p->costume_strings =
        (Fighter_CostumeStrings*)calloc((size_t)count, sizeof(*p->costume_strings));
    p->costumes = (UnkCostumeStruct*)calloc((size_t)count, sizeof(*p->costumes));
    if (!p->id || !p->name || !p->data.a || !p->data.b || !p->anim_file || !p->costume_strings ||
        !p->costumes)
        FAIL("out of memory");
    for (int i = 0; i < count; ++i) {
        /* Symbols default to the base costume in the same slot, wrapping when
         * the pack has more costumes than its base. */
        const Fighter_CostumeStrings* bs = &ftData_803C2360[base][i % base_costumes];
        const PcRosterCostume* c = &d->costumes[i];
        if (c->file == NULL)
            FAIL("costume %d has no \"file\"", i + 1);
        p->costume_strings[i].dat_filename = dup(c->file);
        p->costume_strings[i].joint_name = dup(c->joint ? c->joint : bs->joint_name);
        p->costume_strings[i].matanim_joint_name =
            c->matanim ? (c->matanim[0] ? dup(c->matanim) : NULL) : dup(bs->matanim_joint_name);
        p->portraits[i].path = dup(c->portrait_path);
        p->stocks[i].path = dup(c->stock_path);
    }

    const int k = Ft_Kind_PackFirst + s_pack_count;
    ftData_AssetBaseKind[k] = (s8)base;
    ftData_803C1F40[k] = p->data;
    ftData_803C2360[k] = p->costume_strings;
    ftData_803C23E4[k] = p->anim_file;
    ftData_803C2468[k] = ftData_803C2468[base]; /* results/intro poses: base's for now */
    CostumeListsForeachCharacter[k].costume_list = p->costumes;
    CostumeListsForeachCharacter[k].numCostumes = (u8)count;
    /* Same moveset, so the same animation and demo-animation counts. */
    ftData_Table_Unk0[k].data = NULL;
    ftData_Table_Unk0[k].count = ftData_Table_Unk0[base].count;
    ftData_UnkIntPairs[k].data = NULL;
    ftData_UnkIntPairs[k].count = ftData_UnkIntPairs[base].count;
    ftData_UnkBytePerCharacter[k] = ftData_UnkBytePerCharacter[base]; /* effect bank */
    gFtDataList[k] = 0; /* not NULL: void* would toggle storage order */
    ft_8045996C[k] = 0;
    ftData_AssetPartner[k] = -1;
    return s_pack_count++;
#undef FAIL
}

int pc_roster_add_fighter(const PcRosterFighter* d, char* err, size_t err_size) {
    if (d == NULL) {
        if (err && err_size)
            snprintf(err, err_size, "no fighter");
        return -1;
    }
    const char* why = pc_roster_base_unsupported(d->base_kind);
    if (why) {
        if (err && err_size)
            snprintf(err, err_size, "%s", why);
        return -1;
    }
    const int pbase = partner_base(d->base_kind);
    if (pbase < 0)
        return add_entry(d, d->base_kind, false, 0, err, err_size);

    /* Two-fighter base: both halves must exist, with matching costumes (the
     * pair always wears the same costume slot). */
    if (d->partner == NULL) {
        if (err && err_size)
            snprintf(err, err_size, "a %s pack needs a \"partner\" block for %s",
                pc_mod_fighter_name(d->base_kind), pc_mod_fighter_name(pbase));
        return -1;
    }
    if (s_pack_count + 2 > Ft_Kind_PackMax) {
        if (err && err_size)
            snprintf(err, err_size, "too many character packs (limit %d)", Ft_Kind_PackMax);
        return -1;
    }
    int pair = (int)d->costume_count;
    if ((int)d->partner->costume_count < pair)
        pair = (int)d->partner->costume_count;
    if (pair != (int)d->costume_count || pair != (int)d->partner->costume_count)
        OSReport("roster: %s: main and partner costume counts differ; using %d\n", d->id, pair);
    const int main = add_entry(d, d->base_kind, false, pair, err, err_size);
    if (main < 0)
        return -1;
    PcRosterFighter half = *d->partner;
    char id[200];
    snprintf(id, sizeof(id), "%s/partner", d->id);
    half.id = id;
    half.name = d->name;
    half.mod_id = d->mod_id;
    half.icon_path = NULL;
    half.announcer_path = NULL;
    half.victory_theme_path = NULL;
    const int other = add_entry(&half, pbase, true, s_packs[main].costume_count, err, err_size);
    if (other < 0) {
        /* Roll the half-registered main entry back out of the visible set. */
        s_packs[main].hidden = true;
        return -1;
    }
    s_packs[main].partner = other;
    s_packs[other].partner = main;
    ftData_AssetPartner[Ft_Kind_PackFirst + main] = (s8)(Ft_Kind_PackFirst + other);
    ftData_AssetPartner[Ft_Kind_PackFirst + other] = (s8)(Ft_Kind_PackFirst + main);
    return main;
}

int pc_roster_visible_count(void) {
    int n = 0;
    for (int i = 0; i < s_pack_count; ++i)
        n += !s_packs[i].hidden;
    return n;
}

int pc_roster_visible_pack(int n) {
    for (int i = 0; i < s_pack_count; ++i)
        if (!s_packs[i].hidden && n-- == 0)
            return i;
    return -1;
}

int pc_roster_partner(int i) {
    return i >= 0 && i < s_pack_count ? s_packs[i].partner : -1;
}

const char* pc_roster_announcer_path(int i) {
    const Pack* p = pack_at(i);
    return p ? p->announcer : NULL;
}

const char* pc_roster_victory_theme_path(int i) {
    const Pack* p = pack_at(i);
    return p ? p->victory_theme : NULL;
}

int pc_roster_fighter_count(void) {
    return s_pack_count;
}

int pc_roster_find_fighter(const char* id) {
    if (id == NULL)
        return -1;
    for (int i = 0; i < s_pack_count; ++i)
        if (strcmp(s_packs[i].id, id) == 0)
            return i;
    return -1;
}

static const Pack* pack_at(int i) {
    return i >= 0 && i < s_pack_count ? &s_packs[i] : NULL;
}

const char* pc_roster_fighter_id(int i) {
    const Pack* p = pack_at(i);
    return p ? p->id : NULL;
}

const char* pc_roster_fighter_name(int i) {
    const Pack* p = pack_at(i);
    return p ? p->name : NULL;
}

const char* pc_roster_fighter_mod(int i) {
    const Pack* p = pack_at(i);
    return p ? p->mod_id : NULL;
}

int pc_roster_fighter_base(int i) {
    const Pack* p = pack_at(i);
    return p ? p->base : -1;
}

int pc_roster_fighter_costumes(int i) {
    const Pack* p = pack_at(i);
    return p ? p->costume_count : 0;
}

int pc_roster_asset_kind(int i) {
    return pack_at(i) ? Ft_Kind_PackFirst + i : -1;
}

static const void* image_gx(const Pack* p, RosterImage* img, int w, int h) {
    return pc_image_gx(img, p->id, w, h);
}

const void* pc_roster_icon_gx(int i, int w, int h) {
    Pack* p = (Pack*)pack_at(i);
    return p ? image_gx(p, &p->icon, w, h) : NULL;
}

const void* pc_roster_portrait_gx(int i, int costume, int w, int h) {
    Pack* p = (Pack*)pack_at(i);
    if (p == NULL || costume < 0 || costume >= p->costume_count)
        return NULL;
    return image_gx(p, &p->portraits[costume], w, h);
}

const void* pc_roster_stock_gx(int i, int costume, int w, int h) {
    Pack* p = (Pack*)pack_at(i);
    if (p == NULL || costume < 0 || costume >= p->costume_count)
        return NULL;
    return image_gx(p, &p->stocks[costume], w, h);
}

const void* pc_roster_emblem_gx(int i, int w, int h) {
    Pack* p = (Pack*)pack_at(i);
    return p ? image_gx(p, &p->emblem, w, h) : NULL;
}

const void* pc_roster_name_image_gx(int i, int w, int h) {
    Pack* p = (Pack*)pack_at(i);
    return p ? image_gx(p, &p->name_image, w, h) : NULL;
}

const void* pc_roster_winner_name_gx(int i, int w, int h) {
    Pack* p = (Pack*)pack_at(i);
    return p ? image_gx(p, &p->winner_name, w, h) : NULL;
}

int pc_roster_series(int i) {
    const Pack* p = pack_at(i);
    return p ? p->series : -1;
}

/* ASCII -> the full-width Shift-JIS (CP932) the game's text renderer draws,
 * matching the disc's own names ("Ｍｒ． Ｇａｍｅ ＆ Ｗａｔｃｈ" in gm_1601.c):
 * full-width letters, digits and punctuation, half-width spaces. Anything
 * without a full-width form becomes a full-width space. */
const char* pc_roster_fighter_sjis_name(int i) {
    Pack* p = (Pack*)pack_at(i);
    if (p == NULL)
        return NULL;
    if (p->sjis_name == NULL)
        p->sjis_name = pc_mod_ascii_to_sjis(p->name);
    return p->sjis_name;
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

uint32_t pc_roster_hash(uint32_t h) {
    for (int i = 0; i < s_pack_count; ++i) {
        const Pack* p = &s_packs[i];
        char base[8];
        snprintf(base, sizeof(base), "%d", p->base);
        h = fnv(h, p->id);
        h = fnv(h, base);
        h = fnv(h, p->data.a);
        h = fnv(h, p->data.b);
        h = fnv(h, p->anim_file);
        for (int c = 0; c < p->costume_count; ++c)
            h = fnv(h, p->costume_strings[c].dat_filename);
    }
    return h;
}
