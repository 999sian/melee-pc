/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Unit tests for the mod loader's standalone pieces: the manifest JSON
 * parser, per-pack file aliases, the item-pack registry and the text helper.
 * Built as mods_test (CMakeLists.txt, `ninja unit_tests`). */
#include "pc/mods/alias.h"
#include "pc/mods/image.h"
#include "pc/mods/items.h"
#include "pc/mods/json.h"
#include "pc/mods/mod_internal.h"
#include "pc/mods/stages.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The loader logs through the game's logger; the tests just count lines. */
static int s_log_lines;
void pc_log_line(const char* fmt, ...) {
    (void)fmt;
    ++s_log_lines;
}

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond);               \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

static JsonValue* parse(const char* text) {
    char err[128];
    return json_parse(text, strlen(text), err, sizeof(err));
}

/* ---- json ----------------------------------------------------------------- */

static int test_json_basic(void) {
    JsonValue* v = parse("{\"id\": \"a.b\", \"n\": 2.5, \"on\": true, \"off\": false,"
                         " \"nil\": null, \"list\": [1, 2, 3], \"obj\": {\"k\": \"v\"}}");
    CHECK(v != NULL && v->type == JSON_OBJECT);
    CHECK(v->count == 7);
    CHECK(strcmp(json_string(json_get(v, "id"), ""), "a.b") == 0);
    CHECK(json_number(json_get(v, "n"), 0) == 2.5);
    CHECK(json_bool(json_get(v, "on"), false) == true);
    CHECK(json_bool(json_get(v, "off"), true) == false);
    CHECK(json_get(v, "nil")->type == JSON_NULL);
    const JsonValue* list = json_get(v, "list");
    CHECK(list->type == JSON_ARRAY && list->count == 3);
    CHECK(json_number(json_at(list, 2), 0) == 3);
    CHECK(json_at(list, 3) == NULL);
    int sum = 0;
    JSON_FOREACH(it, list)
    sum += (int)json_number(it, 0);
    CHECK(sum == 6);
    CHECK(strcmp(json_string(json_get(json_get(v, "obj"), "k"), ""), "v") == 0);
    json_free(v);
    return 0;
}

static int test_json_fallbacks(void) {
    JsonValue* v = parse("{\"s\": \"x\", \"n\": 1}");
    CHECK(v != NULL);
    /* Missing keys and type mismatches give the fallback. */
    CHECK(json_get(v, "missing") == NULL);
    CHECK(strcmp(json_string(json_get(v, "missing"), "fb"), "fb") == 0);
    CHECK(strcmp(json_string(json_get(v, "n"), "fb"), "fb") == 0);
    CHECK(json_number(json_get(v, "s"), 7) == 7);
    CHECK(json_bool(json_get(v, "s"), true) == true);
    /* All accessors are NULL-safe. */
    CHECK(json_get(NULL, "x") == NULL);
    CHECK(json_at(NULL, 0) == NULL);
    CHECK(json_number(NULL, 4) == 4);
    json_free(v);
    json_free(NULL);
    return 0;
}

static int test_json_conveniences(void) {
    /* Comments and trailing commas, for hand-written mod.json files. */
    JsonValue* v = parse("// a mod\n{\n  /* block */ \"a\": [1, 2,],\n  \"b\": 3, // tail\n}\n");
    CHECK(v != NULL);
    CHECK(json_get(v, "a")->count == 2);
    CHECK(json_number(json_get(v, "b"), 0) == 3);
    json_free(v);
    return 0;
}

static int test_json_strings(void) {
    JsonValue* v = parse("{\"e\": \"q\\\"b\\\\n\\n\\u00e9\\u20ac\"}");
    CHECK(v != NULL);
    /* \u escapes come out as UTF-8. */
    CHECK(strcmp(json_string(json_get(v, "e"), ""), "q\"b\\n\n\xC3\xA9\xE2\x82\xAC") == 0);
    json_free(v);
    v = parse("[-1.5e2, 0, 1E-1]");
    CHECK(v != NULL);
    CHECK(json_number(json_at(v, 0), 0) == -150.0);
    CHECK(json_number(json_at(v, 2), 0) > 0.0999 && json_number(json_at(v, 2), 0) < 0.1001);
    json_free(v);
    return 0;
}

static int test_json_errors(void) {
    static const char* bad[] = {
        "",
        "{",
        "{\"a\" 1}",
        "{\"a\": }",
        "[1 2]",
        "{'a': 1}",
        "\"unterminated",
        "{\"a\": tru}",
        "[1,,2]",
        "{} extra",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        char err[128] = "";
        JsonValue* v = json_parse(bad[i], strlen(bad[i]), err, sizeof(err));
        if (v != NULL) {
            fprintf(stderr, "accepted bad JSON: %s\n", bad[i]);
            json_free(v);
            return 1;
        }
        CHECK(err[0] != '\0'); /* a reason ("line N: ...") is always given */
    }
    return 0;
}

/* ---- file aliases ---------------------------------------------------------- */

static int test_alias(void) {
    /* Fighter pack 0 runs as character 2 and swaps its sound bank; map pack 0
     * swaps a stage extra file. */
    CHECK(pc_alias_add(PC_ALIAS_FIGHTER, 0, "audio/us/fox.ssm", "audio/us/bigfox.ssm"));
    CHECK(pc_alias_add(PC_ALIAS_STAGE, 0, "/GrPs1.dat", "GrPx1.dat"));
    CHECK(pc_alias_count() == 2);
    uint32_t h = pc_alias_hash(0);
    CHECK(h != 0);

    /* Nothing is on until a match turns it on. */
    CHECK(strcmp(pc_file_alias("/audio/us/fox.ssm"), "/audio/us/fox.ssm") == 0);

    PcAliasPlayer solo[2] = {{2, 0}, {5, -1}};
    pc_alias_activate(0, solo, 2);
    /* Keeps the caller's form (with or without '/'), case-insensitive. */
    CHECK(strcmp(pc_file_alias("/audio/us/fox.ssm"), "/audio/us/bigfox.ssm") == 0);
    CHECK(strcmp(pc_file_alias("AUDIO/US/FOX.SSM"), "audio/us/bigfox.ssm") == 0);
    CHECK(strcmp(pc_file_alias("GrPs1.dat"), "GrPx1.dat") == 0);
    CHECK(strcmp(pc_file_alias("/GrPs1.dat"), "/GrPx1.dat") == 0);
    CHECK(strcmp(pc_file_alias("/GrPs2.dat"), "/GrPs2.dat") == 0);
    CHECK(pc_file_alias(NULL) == NULL);

    /* Save / restore around a temporary activation. */
    PcAliasState saved;
    pc_alias_save(&saved);
    pc_alias_activate(-1, NULL, 0);
    CHECK(strcmp(pc_file_alias("GrPs1.dat"), "GrPs1.dat") == 0);
    pc_alias_restore(&saved);
    CHECK(strcmp(pc_file_alias("GrPs1.dat"), "GrPx1.dat") == 0);

    /* Another player on the same base character: the pack's files would
     * reach them too, so the character alias stays off (the stage's stays). */
    PcAliasPlayer shared[2] = {{2, 0}, {2, -1}};
    pc_alias_activate(0, shared, 2);
    CHECK(strcmp(pc_file_alias("audio/us/fox.ssm"), "audio/us/fox.ssm") == 0);
    CHECK(strcmp(pc_file_alias("GrPs1.dat"), "GrPx1.dat") == 0);

    /* Two players both on the pack is fine. */
    PcAliasPlayer both[2] = {{2, 0}, {2, 0}};
    pc_alias_activate(-1, both, 2);
    CHECK(strcmp(pc_file_alias("audio/us/fox.ssm"), "audio/us/bigfox.ssm") == 0);
    CHECK(strcmp(pc_file_alias("GrPs1.dat"), "GrPs1.dat") == 0); /* other map */

    pc_alias_clear();
    CHECK(strcmp(pc_file_alias("audio/us/fox.ssm"), "audio/us/fox.ssm") == 0);

    /* The hash depends on the table. */
    CHECK(pc_alias_add(PC_ALIAS_STAGE, 1, "a.dat", "b.dat"));
    CHECK(pc_alias_hash(0) != h);
    return 0;
}

/* ---- item packs ------------------------------------------------------------- */

static int test_item_names(void) {
    CHECK(pc_item_from_name("Beam Sword") == 0x0C);
    CHECK(pc_item_from_name("sword") == 0x0C);
    CHECK(pc_item_from_name("beam-sword") == 0x0C);
    CHECK(pc_item_from_name("Home-Run Bat") == 0x0B);
    CHECK(pc_item_from_name("Lip's Stick") == 0x17);
    CHECK(pc_item_from_name("lips stick") == 0x17);
    CHECK(pc_item_from_name("Proximity Mine") == 0x13);
    CHECK(pc_item_from_name("Capsule") == 0x00);
    CHECK(pc_item_from_name("Cloaking Device") == 0x21);
    /* The Poke Ball spawns Pokemon, not itself. */
    CHECK(pc_item_from_name("Poke Ball") < 0);
    CHECK(pc_item_from_name("M_Ball") < 0);
    /* Pokemon, by English or internal name; not Mew or Celebi. */
    CHECK(pc_item_from_name("Chikorita") == 0xA2);
    CHECK(pc_item_from_name("Kabigon") == 0xA3);
    CHECK(pc_item_from_name("Ho-oh") == 0xB3);
    CHECK(pc_item_from_name("Venusaur") == 0xBE);
    CHECK(pc_item_from_name("Mew") < 0);
    CHECK(pc_item_from_name("Celebi") < 0);
    CHECK(pc_item_kind_is_pokemon(0xA2));
    CHECK(!pc_item_kind_is_pokemon(0x0C));
    /* Character, projectile, Pokemon-attack and stage items by internal name. */
    CHECK(pc_item_from_name("Link_Bomb") == 0x3A);
    CHECK(pc_item_from_name("link bomb") == 0x3A);
    CHECK(pc_item_from_name("Fox_Blaster") == 0x4A);
    CHECK(pc_item_from_name("Peach_Turnip") == 0x63);
    CHECK(pc_item_from_name("Lugia_Aeroblast") == 0xC8);
    CHECK(pc_item_from_name("Old_Kuri") == 0xD0);
    CHECK(pc_item_from_name("Kyasarin_Egg") == 0xEC);
    CHECK(pc_item_from_name("L_Gun_Ray") == 0x23);
    CHECK(pc_item_from_name("Unk1") < 0 && pc_item_from_name("Invalid1") < 0);
    CHECK(pc_item_base_class(0x0C) == PC_ITEM_COMMON);
    CHECK(pc_item_base_class(0xA2) == PC_ITEM_POKEMON);
    CHECK(pc_item_base_class(0xB7) == PC_ITEM_NONE); /* Mew */
    CHECK(pc_item_base_class(0x22) == PC_ITEM_NONE); /* Poke Ball */
    CHECK(pc_item_base_class(0x3A) == PC_ITEM_OWNED);
    CHECK(pc_item_base_class(0xD0) == PC_ITEM_OWNED);
    CHECK(pc_item_base_class(0xC8) == PC_ITEM_OWNED);
    CHECK(pc_item_base_class(0xCF) == PC_ITEM_NONE); /* Pokemon_Unk */
    CHECK(pc_item_base_class(0x400) == PC_ITEM_NONE);
    CHECK(strcmp(pc_item_display_name(0x3A), "Link_Bomb") == 0);
    CHECK(pc_item_from_name("nonsense") < 0);
    CHECK(pc_item_from_name(NULL) < 0);
    CHECK(strcmp(pc_item_display_name(0x0C), "Beam Sword") == 0);
    CHECK(strcmp(pc_item_display_name(0xA3), "Snorlax") == 0);
    CHECK(strcmp(pc_item_display_name(-5), "?") == 0);
    return 0;
}

static int test_item_registry(void) {
    char err[200];
    PcItemPackDesc d = {
        .id = "test/giant",
        .name = "Giant Sword",
        .mod_id = "test",
        .base_kind = 0x0C,
        .file = "/ItGiant.dat",
        .symbol = "itArticle",
        .frequency = 0.5f,
        .owner_pack = -1,
        .owner_kind = -1,
        .stage_pack = -1,
        .stage_kind = -1,
    };
    uint32_t h0 = pc_items_hash(1);
    int a = pc_items_add(&d, err, sizeof(err));
    CHECK(a == 0);
    CHECK(pc_items_count() == 1);
    CHECK(strcmp(pc_items_file(a), "ItGiant.dat") == 0); /* leading '/' dropped */
    CHECK(pc_items_base(a) == 0x0C);
    CHECK(pc_items_frequency(a) == 0.5f);
    CHECK(strcmp(pc_items_symbol(a), "itArticle") == 0);
    CHECK(pc_items_hash(1) != h0);

    /* Duplicates and invalid bases are refused with a reason. */
    err[0] = '\0';
    CHECK(pc_items_add(&d, err, sizeof(err)) < 0 && err[0] != '\0');
    PcItemPackDesc bad = d;
    bad.id = "test/ball";
    bad.base_kind = 0x22; /* Poke Ball */
    CHECK(pc_items_add(&bad, err, sizeof(err)) < 0);
    bad.base_kind = 0xB7; /* Mew */
    CHECK(pc_items_add(&bad, err, sizeof(err)) < 0);
    CHECK(pc_items_count() == 1);

    PcItemPackDesc poke = d;
    poke.id = "test/chiko";
    poke.name = "Big Chikorita";
    poke.base_kind = 0xA2;
    CHECK(pc_items_add(&poke, err, sizeof(err)) == 1);

    /* A character item limited to one fighter and stage; the filters are
     * part of the hash too. */
    PcItemPackDesc owned = d;
    owned.id = "test/bigbomb";
    owned.base_kind = pc_item_from_name("Link_Bomb");
    owned.owner_kind = 6;
    owned.stage_kind = 0x1F;
    uint32_t before = pc_items_hash(1);
    CHECK(pc_items_add(&owned, err, sizeof(err)) == 2);
    CHECK(pc_items_owner_kind(2) == 6 && pc_items_stage_kind(2) == 0x1F);
    CHECK(pc_items_owner_pack(2) == -1 && pc_items_stage_pack(2) == -1);
    CHECK(pc_items_hash(1) != before);

    /* The frequency is part of the hash: peers must agree on the draw. */
    uint32_t h1 = pc_items_hash(1);
    CHECK(h1 == pc_items_hash(1));
    CHECK(pc_items_id(5) == NULL && pc_items_base(-1) == -1);

    /* Full-width name for the game's menus. */
    const char* sj = pc_items_sjis_name(0);
    CHECK(sj != NULL);
    CHECK((unsigned char)sj[0] == 0x82 && (unsigned char)sj[1] == 0x66); /* 'G' */
    return 0;
}

/* ---- map packs --------------------------------------------------------------- */

/* stages.c asks the game which ground a stage kind loads; here every stage is
 * its own ground, offset so a mix-up between the two shows. */
int Stage_8022519C(int stkind) {
    return stkind + 100;
}

static int test_stages(void) {
    CHECK(pc_stage_from_name("Battlefield") == 0x1F);
    CHECK(pc_stage_from_name("final destination") == 0x20);
    CHECK(pc_stage_from_name("Pokemon Stadium") == pc_stage_from_name("PStadium"));
    CHECK(pc_stage_from_name("Mushroom Kingdom II") == pc_stage_from_name("inishie2"));
    CHECK(pc_stage_from_name("Mushroom Kingdom") != pc_stage_from_name("Mushroom Kingdom II"));
    CHECK(pc_stage_from_name("Yoshi's Island N64") == pc_stage_from_name("oldyoshi"));
    CHECK(pc_stage_from_name("Yoshi's Island") != pc_stage_from_name("Yoshi's Island N64"));
    CHECK(pc_stage_from_name("Hyrule Temple") > 0);
    CHECK(pc_stage_from_name("Poke Floats") > 0);
    CHECK(pc_stage_from_name("Final Destinations") < 0);
    CHECK(pc_stage_from_name("") < 0 && pc_stage_from_name(NULL) < 0);
    CHECK(strcmp(pc_stage_display_name(0x1F), "battlefield") == 0);

    char err[200];
    PcStagePackDesc d = {.id = "m/field",
        .name = "Test Field",
        .mod_id = "m",
        .base_stkind = 0x1F,
        .file = "GrXb.dat"};
    uint32_t h0 = pc_stages_hash(1);
    CHECK(pc_stages_add(&d, err, sizeof(err)) == 0);
    CHECK(pc_stages_add(&d, err, sizeof(err)) < 0); /* duplicate id */
    PcStagePackDesc bad = d;
    bad.id = "m/bad";
    bad.base_stkind = -1;
    CHECK(pc_stages_add(&bad, err, sizeof(err)) < 0);
    CHECK(pc_stages_count() == 1 && pc_stages_base(0) == 0x1F);
    CHECK(strcmp(pc_stages_name(0), "Test Field") == 0);
    CHECK(pc_stages_hash(1) != h0);

    /* The stage file is swapped only while the pack is active, and only for
     * the base's ground; the path is made absolute. */
    CHECK(pc_stages_file_for(0x1F + 100) == NULL);
    pc_stages_set_active(0);
    CHECK(strcmp(pc_stages_file_for(0x1F + 100), "/GrXb.dat") == 0);
    CHECK(pc_stages_file_for(0x20 + 100) == NULL);
    CHECK(pc_stages_file_for(0x1F) == NULL); /* a StKind is not a GrKind */
    pc_stages_set_active(5);                 /* no such pack: off */
    CHECK(pc_stages_file_for(0x1F + 100) == NULL);
    pc_stages_set_active(-1);
    /* No images given: nothing to show (the SSS then shows the base's). */
    CHECK(pc_stages_icon_gx(0, 64, 56) == NULL);
    return 0;
}

/* ---- images ---------------------------------------------------------------- */

static uint32_t crc32_of(const unsigned char* p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

static void be32(unsigned char* p, uint32_t v) {
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static void chunk(FILE* f, const char* type, const unsigned char* data, uint32_t n) {
    static unsigned char buf[4096 + 8];
    be32(buf, n);
    fwrite(buf, 1, 4, f);
    memcpy(buf, type, 4);
    if (n)
        memcpy(buf + 4, data, n);
    fwrite(buf, 1, n + 4, f);
    be32(buf, crc32_of(buf, n + 4));
    fwrite(buf, 1, 4, f);
}

/* An uncompressed (stored-deflate) RGBA PNG: enough for stb_image. */
static void write_png(const char* path, const unsigned char* rgba, int w, int h) {
    static unsigned char raw[2048], z[2048 + 32];
    size_t n = 0;
    for (int y = 0; y < h; ++y) {
        raw[n++] = 0; /* filter: none */
        memcpy(raw + n, rgba + (size_t)y * w * 4, (size_t)w * 4);
        n += (size_t)w * 4;
    }
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; ++i) {
        a = (a + raw[i]) % 65521;
        b = (b + a) % 65521;
    }
    size_t zn = 0;
    z[zn++] = 0x78;
    z[zn++] = 0x01;
    z[zn++] = 1; /* final stored block */
    z[zn++] = (unsigned char)n;
    z[zn++] = (unsigned char)(n >> 8);
    z[zn++] = (unsigned char)~n;
    z[zn++] = (unsigned char)(~n >> 8);
    memcpy(z + zn, raw, n);
    zn += n;
    be32(z + zn, (b << 16) | a);
    zn += 4;
    unsigned char ihdr[13] = {0};
    be32(ihdr, (uint32_t)w);
    be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8; /* bit depth */
    ihdr[9] = 6; /* RGBA */
    FILE* f = fopen(path, "wb");
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, (uint32_t)zn);
    chunk(f, "IEND", NULL, 0);
    fclose(f);
}

static int test_image(void) {
    /* 4x4 image, each texel (x, y) = R x*16, G y*16, B 200, A 255 - x. */
    unsigned char rgba[4 * 4 * 4];
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            unsigned char* p = rgba + (y * 4 + x) * 4;
            p[0] = (unsigned char)(x * 16);
            p[1] = (unsigned char)(y * 16);
            p[2] = 200;
            p[3] = (unsigned char)(255 - x);
        }
    const char* path = "mods_test_image.png";
    write_png(path, rgba, 4, 4);

    PcImage img = {0};
    img.path = (char*)path;
    /* Sizes must be multiples of 4 (GX tiles). */
    CHECK(pc_image_gx(&img, "t", 6, 4) == NULL);
    CHECK(!img.failed);

    const unsigned char* gx = pc_image_gx(&img, "t", 4, 4);
    CHECK(gx != NULL);
    /* One tile: 16 A,R pairs then 16 G,B pairs, row-major texels. */
    for (int t = 0; t < 16; ++t) {
        int x = t % 4, y = t / 4;
        CHECK(gx[t * 2] == 255 - x && gx[t * 2 + 1] == x * 16);
        CHECK(gx[32 + t * 2] == y * 16 && gx[32 + t * 2 + 1] == 200);
    }
    /* Cached at the same size; resampled (nearest) at another. */
    CHECK(pc_image_gx(&img, "t", 4, 4) == gx);
    const unsigned char* big = pc_image_gx(&img, "t", 8, 8);
    CHECK(big != NULL && img.w == 8 && img.h == 8);
    /* Texel (6, 0) of 8x8: tile 1, t = 2, samples source x = 3. */
    CHECK(big[64 + 2 * 2] == 252 && big[64 + 2 * 2 + 1] == 48);
    /* Texel (0, 5): tile 2, t = 4, samples source y = 2. */
    CHECK(big[128 + 32 + 4 * 2] == 32);
    free(img.gx);
    remove(path);

    PcImage missing = {.path = (char*)"does/not/exist.png"};
    CHECK(pc_image_gx(&missing, "t", 4, 4) == NULL && missing.failed);
    CHECK(pc_image_gx(&missing, "t", 4, 4) == NULL); /* not retried */
    PcImage none = {0};
    CHECK(pc_image_gx(&none, "t", 4, 4) == NULL && !none.failed);
    return 0;
}

/* ---- text ----------------------------------------------------------------- */

static int test_sjis(void) {
    char* s = pc_mod_ascii_to_sjis("Az 9.-'!?&");
    CHECK(s != NULL);
    static const unsigned char want[] = {
        0x82,
        0x60,
        /* A */ 0x82,
        0x9A,
        /* z */ ' ',
        0x82,
        0x58, /* 9 */
        0x81,
        0x44,
        /* . */ 0x81,
        0x7C,
        /* - */ 0x81,
        0x66, /* ' */
        0x81,
        0x49,
        /* ! */ 0x81,
        0x48,
        /* ? */ 0x81,
        0x95,
        /* & */ 0,
    };
    CHECK(memcmp(s, want, sizeof(want)) == 0);
    free(s);
    s = pc_mod_ascii_to_sjis("#");
    CHECK((unsigned char)s[0] == 0x81 && (unsigned char)s[1] == 0x40 && s[2] == 0);
    free(s);
    s = pc_mod_ascii_to_sjis("");
    CHECK(s != NULL && s[0] == 0);
    free(s);
    return 0;
}

int main(void) {
    struct {
        const char* name;
        int (*fn)(void);
    } tests[] = {
        {"json_basic", test_json_basic},
        {"json_fallbacks", test_json_fallbacks},
        {"json_conveniences", test_json_conveniences},
        {"json_strings", test_json_strings},
        {"json_errors", test_json_errors},
        {"alias", test_alias},
        {"item_names", test_item_names},
        {"item_registry", test_item_registry},
        {"sjis", test_sjis},
        {"stages", test_stages},
        {"image", test_image},
    };
    int failed = 0;
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        int r = tests[i].fn();
        printf("%-20s %s\n", tests[i].name, r == 0 ? "ok" : "FAILED");
        failed += r != 0;
    }
    return failed != 0;
}
