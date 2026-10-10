/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pc/compat.h"

#include "items.h"

#include "mod_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ITEM_PACKS 64

typedef struct ItemPack {
    char* id;
    char* name;
    char* mod_id;
    int base;
    char* file;
    char* symbol;
    float frequency;
    char* sjis_name;
    int owner_pack, owner_kind, stage_pack, stage_kind;
} ItemPack;

static ItemPack s_items[MAX_ITEM_PACKS];
static int s_count;

/* Common items in ItemKind order (forward.h), English then internal name.
 * Poke Ball (0x22) is left out on purpose. */
static const char* const s_names[][2] = {
    {"Capsule", "Capsule"},
    {"Crate", "Box"},
    {"Barrel", "Taru"},
    {"Egg", "Egg"},
    {"Party Ball", "Kusudama"},
    {"Barrel Cannon", "TaruCann"},
    {"Bob-omb", "BombHei"},
    {"Mr. Saturn", "Dosei"},
    {"Heart Container", "Heart"},
    {"Maxim Tomato", "Tomato"},
    {"Starman", "Star"},
    {"Home-Run Bat", "Bat"},
    {"Beam Sword", "Sword"},
    {"Parasol", "Parasol"},
    {"Green Shell", "G_Shell"},
    {"Red Shell", "R_Shell"},
    {"Ray Gun", "L_Gun"},
    {"Freezie", "Freeze"},
    {"Food", "Foods"},
    {"Motion-Sensor Bomb", "MSBomb"},
    {"Flipper", "Flipper"},
    {"Super Scope", "S_Scope"},
    {"Star Rod", "StarRod"},
    {"Lip's Stick", "LipStick"},
    {"Fan", "Harisen"},
    {"Fire Flower", "F_Flower"},
    {"Super Mushroom", "Kinoko"},
    {"Poison Mushroom", "DKinoko"},
    {"Hammer", "Hammer"},
    {"Warp Star", "WStar"},
    {"Screw Attack", "ScBall"},
    {"Bunny Hood", "RabbitC"},
    {"Metal Box", "MetalB"},
    {"Cloaking Device", "Spycloak"},
};
#define NUM_NAMES ((int)(sizeof(s_names) / sizeof(s_names[0])))

/* Poke Ball Pokemon from 0xA1 (forward.h It_PKind_Start), English then
 * internal name; NULL marks the two that are not bases (Mew, Celebi). */
#define PKIND_START 0xA1
static const char* const s_pokemon[][2] = {
    {"Goldeen", "Tosakinto"},
    {"Chikorita", "Chicorita"},
    {"Snorlax", "Kabigon"},
    {"Blastoise", "Kamex"},
    {"Weezing", "Matadogas"},
    {"Charizard", "Lizardon"},
    {"Moltres", "Fire"},
    {"Zapdos", "Thunder"},
    {"Articuno", "Freezer"},
    {"Wobbuffet", "Sonans"},
    {"Scizor", "Hassam"},
    {"Unown", "Unknown"},
    {"Entei", "Entei"},
    {"Raikou", "Raikou"},
    {"Suicune", "Suikun"},
    {"Bellossom", "Kireihana"},
    {"Electrode", "Marumine"},
    {"Lugia", "Lugia"},
    {"Ho-oh", "Houou"},
    {"Ditto", "Metamon"},
    {"Clefairy", "Pippi"},
    {"Togepi", "Togepy"},
    {NULL, "Mew"},
    {NULL, "Cerebi"},
    {"Staryu", "Hitodeman"},
    {"Chansey", "Lucky"},
    {"Porygon2", "Porygon2"},
    {"Cyndaquil", "Hinoarashi"},
    {"Marill", "Maril"},
    {"Venusaur", "Fushigibana"},
};
#define NUM_POKEMON ((int)(sizeof(s_pokemon) / sizeof(s_pokemon[0])))

/* Character, projectile, Pokemon-attack and stage items by their ItemKind
 * names (forward.h, without "It_Kind_"); generated from the enum. */
static const struct {
    const char* name;
    int kind;
} s_owned[] = {
    {"L_Gun_Ray", 0x23},
    {"StarRod_Star", 0x24},
    {"LipStick_Spore", 0x25},
    {"S_Scope_Beam", 0x26},
    {"L_Gun_Beam", 0x27},
    {"Hammer_Head", 0x28},
    {"F_Flower_Flame", 0x29},
    {"EvYoshiEgg", 0x2A},
    {"Kuriboh", 0x2B},
    {"Leadead", 0x2C},
    {"Octarock", 0x2D},
    {"Ottosea", 0x2E},
    {"Octarock_Stone", 0x2F},
    {"Mario_Fire", 0x30},
    {"DrMario_Vitamin", 0x31},
    {"Kirby_CBeam", 0x32},
    {"Kirby_Hammer", 0x33},
    {"Fox_Laser", 0x36},
    {"Falco_Laser", 0x37},
    {"Fox_Illusion", 0x38},
    {"Falco_Phantasm", 0x39},
    {"Link_Bomb", 0x3A},
    {"CLink_Bomb", 0x3B},
    {"Link_Boomerang", 0x3C},
    {"CLink_Boomerang", 0x3D},
    {"Link_HShot", 0x3E},
    {"CLink_HShot", 0x3F},
    {"Link_Arrow", 0x40},
    {"CLink_Arrow", 0x41},
    {"Ness_PKFire", 0x42},
    {"Ness_PKFire_Flame", 0x43},
    {"Ness_PKFlush", 0x44},
    {"Ness_PKThunder", 0x45},
    {"Ness_PKThunder1", 0x46},
    {"Ness_PKThunder2", 0x47},
    {"Ness_PKThunder3", 0x48},
    {"Ness_PKThunder4", 0x49},
    {"Fox_Blaster", 0x4A},
    {"Falco_Blaster", 0x4B},
    {"Link_Bow", 0x4C},
    {"CLink_Bow", 0x4D},
    {"Ness_PKFlush_Explode", 0x4E},
    {"Seak_NeedleThrow", 0x4F},
    {"Seak_NeedleHeld", 0x50},
    {"Pikachu_Thunder", 0x51},
    {"Pichu_Thunder", 0x52},
    {"Mario_Cape", 0x53},
    {"DrMario_Sheet", 0x54},
    {"Seak_Vanish", 0x55},
    {"Yoshi_EggThrow", 0x56},
    {"Yoshi_EggLay", 0x57},
    {"Yoshi_Star", 0x58},
    {"Pikachu_TJolt_Ground", 0x59},
    {"Pikachu_TJolt_Air", 0x5A},
    {"Pichu_TJolt_Ground", 0x5B},
    {"Pichu_TJolt_Air", 0x5C},
    {"Samus_Bomb", 0x5D},
    {"Samus_Charge", 0x5E},
    {"Samus_Missile", 0x5F},
    {"Samus_GBeam", 0x60},
    {"Seak_Chain", 0x61},
    {"Peach_Explode", 0x62},
    {"Peach_Turnip", 0x63},
    {"Koopa_Flame", 0x64},
    {"Ness_Bat", 0x65},
    {"Ness_Yoyo", 0x66},
    {"Peach_Parasol", 0x67},
    {"Peach_Toad", 0x68},
    {"Luigi_Fire", 0x69},
    {"IceClimber_Ice", 0x6A},
    {"IceClimber_Blizzard", 0x6B},
    {"Zelda_DinFire", 0x6C},
    {"Zelda_DinFire_Explode", 0x6D},
    {"Mewtwo_Disable", 0x6E},
    {"Peach_ToadSpore", 0x6F},
    {"Mewtwo_ShadowBall", 0x70},
    {"IceClimber_GumStrings", 0x71},
    {"GameWatch_Greenhouse", 0x72},
    {"GameWatch_Manhole", 0x73},
    {"GameWatch_Fire", 0x74},
    {"GameWatch_Parachute", 0x75},
    {"GameWatch_Turtle", 0x76},
    {"GameWatch_Breath", 0x77},
    {"GameWatch_Judge", 0x78},
    {"GameWatch_Panic", 0x79},
    {"GameWatch_Chef", 0x7A},
    {"CLink_Milk", 0x7B},
    {"GameWatch_Rescue", 0x7C},
    {"MasterHand_Laser", 0x7D},
    {"MasterHand_Bullet", 0x7E},
    {"CrazyHand_Laser", 0x7F},
    {"CrazyHand_Bullet", 0x80},
    {"CrazyHand_Bomb", 0x81},
    {"Kirby_MarioFire", 0x82},
    {"Kirby_DrMarioVitamin", 0x83},
    {"Kirby_LuigiFire", 0x84},
    {"Kirby_IceClimberIce", 0x85},
    {"Kirby_PeachToad", 0x86},
    {"Kirby_PeachToadSpore", 0x87},
    {"Kirby_FoxLaser", 0x88},
    {"Kirby_FalcoLaser", 0x89},
    {"Kirby_FoxBlaster", 0x8A},
    {"Kirby_FalcoBlaster", 0x8B},
    {"Kirby_LinkArrow", 0x8C},
    {"Kirby_CLinkArrow", 0x8D},
    {"Kirby_LinkBow", 0x8E},
    {"Kirby_CLinkBow", 0x8F},
    {"Kirby_MewtwoShadowBall", 0x90},
    {"Kirby_NessPKFlush", 0x91},
    {"Kirby_NessPKFlush_Explode", 0x92},
    {"Kirby_PikachuTJolt_Ground", 0x93},
    {"Kirby_PikachuTJolt_Air", 0x94},
    {"Kirby_PichuTJolt_Ground", 0x95},
    {"Kirby_PichuTJolt_Air", 0x96},
    {"Kirby_SamusCharge", 0x97},
    {"Kirby_SeakNeedleThrow", 0x98},
    {"Kirby_SeakNeedleHeld", 0x99},
    {"Kirby_KoopaFlame", 0x9A},
    {"Kirby_GameWatchChef", 0x9B},
    {"Kirby_GameWatchChefPan", 0x9C},
    {"Kirby_YoshiEggLay", 0x9D},
    {"Coin", 0x9F},
    {"Chicorita_Leaf", 0xBF},
    {"Kamex_HydroPump", 0xC0},
    {"Matadogas_Gas1", 0xC1},
    {"Matadogas_Gas2", 0xC2},
    {"Lizardon_Flame1", 0xC3},
    {"Lizardon_Flame2", 0xC4},
    {"Lizardon_Flame3", 0xC5},
    {"Lizardon_Flame4", 0xC6},
    {"Unknown_Swarm", 0xC7},
    {"Lugia_Aeroblast", 0xC8},
    {"Lugia_Aeroblast2", 0xC9},
    {"Lugia_Aeroblast3", 0xCA},
    {"Houou_SacredFire", 0xCB},
    {"Hitodeman_Star", 0xCC},
    {"Lucky_Egg", 0xCD},
    {"Hinoarashi_Flame", 0xCE},
    {"Old_Kuri", 0xD0},
    {"Mato", 0xD1},
    {"Heiho", 0xD2},
    {"Nokonoko", 0xD3},
    {"Patapata", 0xD4},
    {"Likelike", 0xD5},
    {"Old_Lead", 0xD6},
    {"Old_Octa", 0xD7},
    {"Old_Otto", 0xD8},
    {"Whitebea", 0xD9},
    {"Klap", 0xDA},
    {"ZGShell", 0xDB},
    {"ZRShell", 0xDC},
    {"Tincle", 0xDD},
    {"WhispyApple", 0xE1},
    {"WhispyHealApple", 0xE2},
    {"Tools", 0xE6},
    {"Kyasarin", 0xE9},
    {"Arwing_Laser", 0xEA},
    {"GreatFox_Laser", 0xEB},
    {"Kyasarin_Egg", 0xEC},
};
#define NUM_OWNED ((int)(sizeof(s_owned) / sizeof(s_owned[0])))

PcItemBaseClass pc_item_base_class(int kind) {
    if (kind >= 0 && kind < NUM_NAMES)
        return PC_ITEM_COMMON;
    if (pc_item_kind_is_pokemon(kind))
        return s_pokemon[kind - PKIND_START][0] != NULL ? PC_ITEM_POKEMON : PC_ITEM_NONE;
    for (int i = 0; i < NUM_OWNED; ++i)
        if (s_owned[i].kind == kind)
            return PC_ITEM_OWNED;
    return PC_ITEM_NONE;
}

bool pc_item_kind_is_pokemon(int kind) {
    return kind >= PKIND_START && kind < PKIND_START + NUM_POKEMON;
}

static bool valid_base(int kind) {
    return pc_item_base_class(kind) != PC_ITEM_NONE;
}

/* Case-, space-, dash-, dot-, underscore- and apostrophe-insensitive. */
static bool name_eq(const char* a, const char* b) {
    for (;;) {
        while (*a == ' ' || *a == '-' || *a == '.' || *a == '_' || *a == '\'')
            ++a;
        while (*b == ' ' || *b == '-' || *b == '.' || *b == '_' || *b == '\'')
            ++b;
        if (*a == '\0' || *b == '\0')
            return *a == *b;
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return false;
        ++a;
        ++b;
    }
}

int pc_item_from_name(const char* name) {
    if (name == NULL)
        return -1;
    for (int k = 0; k < NUM_NAMES; ++k)
        if (name_eq(name, s_names[k][0]) || name_eq(name, s_names[k][1]))
            return k;
    for (int k = 0; k < NUM_POKEMON; ++k)
        if (s_pokemon[k][0] != NULL &&
            (name_eq(name, s_pokemon[k][0]) || name_eq(name, s_pokemon[k][1])))
            return PKIND_START + k;
    for (int k = 0; k < NUM_OWNED; ++k)
        if (name_eq(name, s_owned[k].name))
            return s_owned[k].kind;
    /* Common aliases. */
    if (name_eq(name, "Motion Sensor Bomb") || name_eq(name, "Proximity Mine"))
        return 0x13;
    return -1;
}

const char* pc_item_display_name(int kind) {
    if (kind >= 0 && kind < NUM_NAMES)
        return s_names[kind][0];
    if (pc_item_kind_is_pokemon(kind) && s_pokemon[kind - PKIND_START][0] != NULL)
        return s_pokemon[kind - PKIND_START][0];
    for (int k = 0; k < NUM_OWNED; ++k)
        if (s_owned[k].kind == kind)
            return s_owned[k].name;
    return "?";
}

static char* dup(const char* s) {
    if (s == NULL)
        return NULL;
    size_t n = strlen(s) + 1;
    char* d = (char*)malloc(n);
    if (d != NULL)
        memcpy(d, s, n);
    return d;
}

int pc_items_add(const PcItemPackDesc* desc, char* err, size_t err_size) {
    if (s_count == MAX_ITEM_PACKS) {
        snprintf(err, err_size, "too many item packs (at most %d)", MAX_ITEM_PACKS);
        return -1;
    }
    if (!valid_base(desc->base_kind)) {
        snprintf(err, err_size, "invalid base item");
        return -1;
    }
    for (int i = 0; i < s_count; ++i) {
        if (strcmp(s_items[i].id, desc->id) == 0) {
            snprintf(err, err_size, "duplicate item id");
            return -1;
        }
    }
    ItemPack* p = &s_items[s_count];
    p->id = dup(desc->id);
    p->name = dup(desc->name);
    p->mod_id = dup(desc->mod_id);
    p->base = desc->base_kind;
    p->file = dup(desc->file[0] == '/' ? desc->file + 1 : desc->file);
    p->symbol = dup(desc->symbol);
    p->frequency = desc->frequency;
    p->owner_pack = desc->owner_pack;
    p->owner_kind = desc->owner_kind;
    p->stage_pack = desc->stage_pack;
    p->stage_kind = desc->stage_kind;
    return s_count++;
}

int pc_items_count(void) {
    return s_count;
}

#define PACK_OK(i) ((i) >= 0 && (i) < s_count)

const char* pc_items_id(int pack) {
    return PACK_OK(pack) ? s_items[pack].id : NULL;
}
const char* pc_items_name(int pack) {
    return PACK_OK(pack) ? s_items[pack].name : NULL;
}
int pc_items_base(int pack) {
    return PACK_OK(pack) ? s_items[pack].base : -1;
}
const char* pc_items_file(int pack) {
    return PACK_OK(pack) ? s_items[pack].file : NULL;
}
const char* pc_items_symbol(int pack) {
    return PACK_OK(pack) ? s_items[pack].symbol : NULL;
}
float pc_items_frequency(int pack) {
    return PACK_OK(pack) ? s_items[pack].frequency : 0.0f;
}
int pc_items_owner_pack(int pack) {
    return PACK_OK(pack) ? s_items[pack].owner_pack : -1;
}
int pc_items_owner_kind(int pack) {
    return PACK_OK(pack) ? s_items[pack].owner_kind : -1;
}
int pc_items_stage_pack(int pack) {
    return PACK_OK(pack) ? s_items[pack].stage_pack : -1;
}
int pc_items_stage_kind(int pack) {
    return PACK_OK(pack) ? s_items[pack].stage_kind : -1;
}
const char* pc_items_sjis_name(int pack) {
    if (!PACK_OK(pack))
        return NULL;
    if (s_items[pack].sjis_name == NULL)
        s_items[pack].sjis_name = pc_mod_ascii_to_sjis(s_items[pack].name);
    return s_items[pack].sjis_name;
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

uint32_t pc_items_hash(uint32_t h) {
    for (int i = 0; i < s_count; ++i) {
        char extra[96];
        /* The weight decides the draw, so peers must agree on it exactly. */
        snprintf(extra, sizeof(extra), "%d:%a:%d:%d:%d:%d", s_items[i].base,
            (double)s_items[i].frequency, s_items[i].owner_pack, s_items[i].owner_kind,
            s_items[i].stage_pack, s_items[i].stage_kind);
        h = fnv(h, s_items[i].id);
        h = fnv(h, extra);
        h = fnv(h, s_items[i].file);
        h = fnv(h, s_items[i].symbol);
    }
    return h;
}
