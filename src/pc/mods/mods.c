/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Mod loader. See mods.h for the lifecycle and docs/modding.md for the format.
 *
 * Layout of a mod:
 *   <mods dir>/<folder>/mod.json   manifest (required)
 *   <mods dir>/<folder>/files/...  overlaid onto the disc, path for path
 *   <mods dir>/<folder>/plugin.dll native plugin (optional, see melee_mod.h)
 *
 * Mods are found in two places: "mods" next to the executable (bundled packs)
 * and "mods" in the user preference directory (where players drop them). The
 * user directory is scanned first, so a user copy shadows a bundled one with
 * the same id. */
#include "mods.h"

#include "json.h"
#include "melee_mod.h"
#include "mod_internal.h"
#include "hud_art.h"
#include "roster.h"
#include "stages.h"
#include "alias.h"
#include "items.h"

#include "../net.h"
#include "../pc.h"

#include <SDL3/SDL.h>
#include <aurora/dvd.h>
#include <dolphin/dvd.h>

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define PLUGIN_EXT ".dll"
#elif defined(__APPLE__)
#define PLUGIN_EXT ".dylib"
#else
#define PLUGIN_EXT ".so"
#endif

/* ---- small helpers ---------------------------------------------------------- */

static char* xstrdup(const char* s) {
    if (s == NULL)
        return NULL;
    size_t n = strlen(s) + 1;
    char* d = (char*)malloc(n);
    if (d)
        memcpy(d, s, n);
    return d;
}

static char* xasprintf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static char* xasprintf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0)
        return NULL;
    char* s = (char*)malloc((size_t)n + 1);
    if (s == NULL)
        return NULL;
    va_start(ap, fmt);
    vsnprintf(s, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return s;
}

static void* grow(void* p, size_t* cap, size_t need, size_t elem) {
    if (need <= *cap)
        return p;
    size_t n = *cap ? *cap * 2 : 8;
    while (n < need)
        n *= 2;
    void* q = realloc(p, n * elem);
    if (q == NULL)
        return NULL;
    *cap = n;
    return q;
}

/* Folder paths from SDL end in a separator; normalise everything to '/'
 * without a trailing one. */
static char* join_path(const char* a, const char* b) {
    size_t la = strlen(a);
    while (la > 0 && (a[la - 1] == '/' || a[la - 1] == '\\'))
        --la;
    char* s = xasprintf("%.*s/%s", (int)la, a, b);
    if (s)
        for (char* c = s; *c; ++c)
            if (*c == '\\')
                *c = '/';
    return s;
}

static bool is_dir(const char* path) {
    SDL_PathInfo info;
    return SDL_GetPathInfo(path, &info) && info.type == SDL_PATHTYPE_DIRECTORY;
}

static bool valid_id(const char* id) {
    if (id == NULL || id[0] == '\0' || strlen(id) > 64)
        return false;
    for (const char* c = id; *c; ++c)
        if (!(islower((unsigned char)*c) || isdigit((unsigned char)*c) || *c == '.' || *c == '_' ||
                *c == '-'))
            return false;
    return true;
}

static uint32_t fnv1a(uint32_t h, const void* data, size_t len) {
    const unsigned char* p = (const unsigned char*)data;
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static uint32_t fnv1a_str(uint32_t h, const char* s) {
    return fnv1a(h, s ? s : "", s ? strlen(s) + 1 : 1);
}

/* ---- model ------------------------------------------------------------------- */

typedef enum ConfigType { CFG_NUMBER, CFG_BOOL, CFG_STRING } ConfigType;

typedef struct ConfigEntry {
    char* key;
    char* label;
    ConfigType type;
    double num_default, num_value, num_min, num_max;
    bool has_min, has_max;
    bool bool_default, bool_value;
    char* str_default;
    char* str_value;
    bool overridden;
} ConfigEntry;

typedef struct ModFile {
    char* disc_path; /* "/PlFxNr.dat" */
    char* host_path;
    uint64_t size;
} ModFile;

struct MeleeMod {
    PcModInfo info;
    char* id;
    char* name;
    char* version;
    char* author;
    char* description;
    char* dir;
    char* error;
    char* plugin; /* resolved plugin path, NULL when none */
    char* files_dir;
    JsonValue* manifest;
    int api_version;
    bool enabled_by_default;
    char** deps;
    size_t dep_count;
    char** conflicts;
    size_t conflict_count;
    ConfigEntry* config;
    size_t config_count;
    ModFile* files;
    size_t file_count, file_cap;
    SDL_SharedObject* lib;
    MeleeModShutdownFn shutdown;
};
typedef struct MeleeMod Mod;

typedef struct AttrPatch {
    Mod* mod;
    int kind; /* FighterKind or MELEE_FIGHTER_ALL */
    int attr;
    MeleeAttrOp op;
    float value;
    bool from_api;
} AttrPatch;

typedef struct Hook {
    int id;
    Mod* mod;
    MeleeModHook hook;
    MeleeModHookFn fn;
    void* user;
} Hook;

typedef struct Overlay {
    ModFile* file;
    Mod* mod;
} Overlay;

static Mod** s_mods;
static size_t s_mod_count, s_mod_cap;
static bool s_scanned, s_activated;
static char* s_user_dir;
static char* s_cfg_path;

/* mods.cfg state, kept across rescans. */
typedef struct SavedEnable {
    char* id;
    bool enabled;
} SavedEnable;
typedef struct SavedConfig {
    char* id;
    char* key;
    char* value; /* JSON literal */
} SavedConfig;
static SavedEnable* s_saved_enable;
static size_t s_saved_enable_count, s_saved_enable_cap;
static SavedConfig* s_saved_config;
static size_t s_saved_config_count, s_saved_config_cap;
/* Per-pack switches ("pack <mod id>/<pack id> 0|1"); packs default to on. */
static SavedEnable* s_saved_pack;
static size_t s_saved_pack_count, s_saved_pack_cap;

static AttrPatch* s_patches;
static size_t s_patch_count, s_patch_cap;
static Hook* s_hooks;
static size_t s_hook_count, s_hook_cap;
static int s_next_hook_id = 1;
static Overlay* s_overlays;
static size_t s_overlay_count, s_overlay_cap;
static AuroraOverlayFile* s_aurora_files;

static int s_scene = -1;
static bool s_in_match;
static uint32_t s_frame;
static uint32_t s_pending_spawns;

/* ---- mods.cfg -------------------------------------------------------------- */

static void saved_clear(void) {
    for (size_t i = 0; i < s_saved_enable_count; ++i)
        free(s_saved_enable[i].id);
    for (size_t i = 0; i < s_saved_config_count; ++i) {
        free(s_saved_config[i].id);
        free(s_saved_config[i].key);
        free(s_saved_config[i].value);
    }
    for (size_t i = 0; i < s_saved_pack_count; ++i)
        free(s_saved_pack[i].id);
    s_saved_enable_count = 0;
    s_saved_config_count = 0;
    s_saved_pack_count = 0;
}

static void saved_set_pack(const char* id, bool enabled) {
    for (size_t i = 0; i < s_saved_pack_count; ++i)
        if (strcmp(s_saved_pack[i].id, id) == 0) {
            s_saved_pack[i].enabled = enabled;
            return;
        }
    SavedEnable* p = (SavedEnable*)grow(
        s_saved_pack, &s_saved_pack_cap, s_saved_pack_count + 1, sizeof(SavedEnable));
    if (p == NULL)
        return;
    s_saved_pack = p;
    s_saved_pack[s_saved_pack_count].id = xstrdup(id);
    s_saved_pack[s_saved_pack_count].enabled = enabled;
    ++s_saved_pack_count;
}

/* Whether the player left pack "<mod id>/<pack id>" switched on. */
static bool pack_enabled(const char* full_id) {
    for (size_t i = 0; i < s_saved_pack_count; ++i)
        if (strcmp(s_saved_pack[i].id, full_id) == 0)
            return s_saved_pack[i].enabled;
    return true;
}

static void saved_set_enable(const char* id, bool enabled) {
    for (size_t i = 0; i < s_saved_enable_count; ++i)
        if (strcmp(s_saved_enable[i].id, id) == 0) {
            s_saved_enable[i].enabled = enabled;
            return;
        }
    SavedEnable* p = (SavedEnable*)grow(
        s_saved_enable, &s_saved_enable_cap, s_saved_enable_count + 1, sizeof(SavedEnable));
    if (p == NULL)
        return;
    s_saved_enable = p;
    s_saved_enable[s_saved_enable_count].id = xstrdup(id);
    s_saved_enable[s_saved_enable_count].enabled = enabled;
    ++s_saved_enable_count;
}

static void saved_set_config(const char* id, const char* key, const char* value) {
    for (size_t i = 0; i < s_saved_config_count; ++i)
        if (strcmp(s_saved_config[i].id, id) == 0 && strcmp(s_saved_config[i].key, key) == 0) {
            free(s_saved_config[i].value);
            s_saved_config[i].value = xstrdup(value);
            return;
        }
    SavedConfig* p = (SavedConfig*)grow(
        s_saved_config, &s_saved_config_cap, s_saved_config_count + 1, sizeof(SavedConfig));
    if (p == NULL)
        return;
    s_saved_config = p;
    s_saved_config[s_saved_config_count].id = xstrdup(id);
    s_saved_config[s_saved_config_count].key = xstrdup(key);
    s_saved_config[s_saved_config_count].value = xstrdup(value);
    ++s_saved_config_count;
}

static void cfg_load(void) {
    saved_clear();
    if (s_cfg_path == NULL)
        return;
    FILE* f = fopen(s_cfg_path, "rb");
    if (f == NULL)
        return;
    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';
        if (line[0] == '#' || line[0] == '\0')
            continue;
        char kind[16], id[160], key[80];
        int used = 0;
        if (sscanf(line, "%15s %159s%n", kind, id, &used) < 2)
            continue;
        const char* rest = line + used;
        while (*rest == ' ')
            ++rest;
        if (strcmp(kind, "enabled") == 0) {
            saved_set_enable(id, atoi(rest) != 0);
        } else if (strcmp(kind, "pack") == 0) {
            saved_set_pack(id, atoi(rest) != 0);
        } else if (strcmp(kind, "config") == 0) {
            int used2 = 0;
            if (sscanf(rest, "%79s%n", key, &used2) < 1)
                continue;
            const char* value = rest + used2;
            while (*value == ' ')
                ++value;
            saved_set_config(id, key, value);
        }
    }
    fclose(f);
}

static void cfg_save(void) {
    if (s_cfg_path == NULL)
        return;
    char* tmp = xasprintf("%s.tmp", s_cfg_path);
    if (tmp == NULL)
        return;
    FILE* f = fopen(tmp, "wb");
    if (f == NULL) {
        pc_log_line("mods: cannot write %s", tmp);
        free(tmp);
        return;
    }
    fputs("# melee-pc mod settings. Edited by the launcher; safe to edit by hand.\n", f);
    for (size_t i = 0; i < s_saved_enable_count; ++i)
        fprintf(f, "enabled %s %d\n", s_saved_enable[i].id, s_saved_enable[i].enabled ? 1 : 0);
    for (size_t i = 0; i < s_saved_config_count; ++i)
        fprintf(f, "config %s %s %s\n", s_saved_config[i].id, s_saved_config[i].key,
            s_saved_config[i].value);
    for (size_t i = 0; i < s_saved_pack_count; ++i)
        fprintf(f, "pack %s %d\n", s_saved_pack[i].id, s_saved_pack[i].enabled ? 1 : 0);
    fclose(f);
    remove(s_cfg_path);
    if (rename(tmp, s_cfg_path) != 0)
        pc_log_line("mods: cannot replace %s", s_cfg_path);
    free(tmp);
}

/* ---- scanning ------------------------------------------------------------------ */

static void mod_free(Mod* m) {
    if (m == NULL)
        return;
    free(m->id);
    free(m->name);
    free(m->version);
    free(m->author);
    free(m->description);
    free(m->dir);
    free(m->error);
    free(m->plugin);
    free(m->files_dir);
    json_free(m->manifest);
    for (size_t i = 0; i < m->dep_count; ++i)
        free(m->deps[i]);
    free(m->deps);
    for (size_t i = 0; i < m->conflict_count; ++i)
        free(m->conflicts[i]);
    free(m->conflicts);
    for (size_t i = 0; i < m->config_count; ++i) {
        ConfigEntry* c = &m->config[i];
        free(c->key);
        free(c->label);
        free(c->str_default);
        free(c->str_value);
    }
    free(m->config);
    for (size_t i = 0; i < m->file_count; ++i) {
        free(m->files[i].disc_path);
        free(m->files[i].host_path);
    }
    free(m->files);
    free(m);
}

static void mod_error(Mod* m, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void mod_error(Mod* m, const char* fmt, ...) {
    if (m->error != NULL)
        return; /* keep the first, it is usually the cause */
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    m->error = xstrdup(buf);
    pc_log_line("mods: %s: %s", m->id ? m->id : m->dir, buf);
}

static Mod* find_mod(const char* id) {
    for (size_t i = 0; i < s_mod_count; ++i)
        if (strcmp(s_mods[i]->id, id) == 0)
            return s_mods[i];
    return NULL;
}

typedef struct FileWalk {
    Mod* mod;
    int depth;
} FileWalk;

static SDL_EnumerationResult SDLCALL walk_files(
    void* userdata, const char* dirname, const char* fname);

static void add_mod_file(Mod* m, const char* host, uint64_t size) {
    const char* rel = host + strlen(m->files_dir);
    while (*rel == '/')
        ++rel;
    ModFile* p = (ModFile*)grow(m->files, &m->file_cap, m->file_count + 1, sizeof(ModFile));
    if (p == NULL)
        return;
    m->files = p;
    ModFile* f = &m->files[m->file_count++];
    f->disc_path = xasprintf("/%s", rel);
    f->host_path = xstrdup(host);
    f->size = size;
}

static SDL_EnumerationResult SDLCALL walk_files(
    void* userdata, const char* dirname, const char* fname) {
    FileWalk* w = (FileWalk*)userdata;
    if (fname[0] == '.')
        return SDL_ENUM_CONTINUE; /* .DS_Store, .git, editor droppings */
    char* path = join_path(dirname, fname);
    if (path == NULL)
        return SDL_ENUM_FAILURE;
    SDL_PathInfo info;
    if (SDL_GetPathInfo(path, &info)) {
        if (info.type == SDL_PATHTYPE_DIRECTORY) {
            if (w->depth < 16) {
                ++w->depth;
                SDL_EnumerateDirectory(path, walk_files, w);
                --w->depth;
            }
        } else if (info.type == SDL_PATHTYPE_FILE) {
            if (info.size > 0xFFFFFFFFull)
                mod_error(w->mod, "%s is larger than 4 GiB", path);
            else
                add_mod_file(w->mod, path, info.size);
        }
    }
    free(path);
    return SDL_ENUM_CONTINUE;
}

static char** read_string_list(const JsonValue* v, size_t* count) {
    *count = 0;
    if (v == NULL || v->type != JSON_ARRAY || v->count == 0)
        return NULL;
    char** out = (char**)calloc(v->count, sizeof(char*));
    if (out == NULL)
        return NULL;
    JSON_FOREACH(it, v)
    if (it->type == JSON_STRING)
        out[(*count)++] = xstrdup(it->string);
    return out;
}

static void read_config_schema(Mod* m) {
    const JsonValue* cfg = json_get(m->manifest, "config");
    if (cfg == NULL)
        return;
    if (cfg->type != JSON_OBJECT) {
        mod_error(m, "\"config\" must be an object");
        return;
    }
    m->config = (ConfigEntry*)calloc(cfg->count ? cfg->count : 1, sizeof(ConfigEntry));
    if (m->config == NULL)
        return;
    JSON_FOREACH(it, cfg) {
        ConfigEntry* c = &m->config[m->config_count];
        const JsonValue* def = it->type == JSON_OBJECT ? json_get(it, "default") : it;
        const char* type = it->type == JSON_OBJECT ? json_string(json_get(it, "type"), NULL) : NULL;
        if (type == NULL && def != NULL)
            type = def->type == JSON_BOOL ? "bool" : def->type == JSON_STRING ? "string" : "number";
        if (type == NULL) {
            pc_log_line("mods: %s: config \"%s\" has no default; skipped", m->id, it->key);
            continue;
        }
        c->key = xstrdup(it->key);
        c->label = xstrdup(json_string(json_get(it, "label"), it->key));
        if (strcmp(type, "bool") == 0 || strcmp(type, "boolean") == 0) {
            c->type = CFG_BOOL;
            c->bool_default = c->bool_value = json_bool(def, false);
        } else if (strcmp(type, "string") == 0) {
            c->type = CFG_STRING;
            c->str_default = xstrdup(json_string(def, ""));
            c->str_value = xstrdup(c->str_default);
        } else {
            c->type = CFG_NUMBER;
            c->num_default = c->num_value = json_number(def, 0);
            const JsonValue* mn = json_get(it, "min");
            const JsonValue* mx = json_get(it, "max");
            c->has_min = mn && mn->type == JSON_NUMBER;
            c->has_max = mx && mx->type == JSON_NUMBER;
            c->num_min = json_number(mn, 0);
            c->num_max = json_number(mx, 0);
        }
        ++m->config_count;
    }
}

static ConfigEntry* config_find(Mod* m, const char* key) {
    if (m == NULL || key == NULL)
        return NULL;
    for (size_t i = 0; i < m->config_count; ++i)
        if (strcmp(m->config[i].key, key) == 0)
            return &m->config[i];
    return NULL;
}

static double clamp_config(const ConfigEntry* c, double v) {
    if (c->has_min && v < c->num_min)
        v = c->num_min;
    if (c->has_max && v > c->num_max)
        v = c->num_max;
    return v;
}

/* Applies mods.cfg overrides on top of the manifest defaults. */
static void apply_saved_config(Mod* m) {
    for (size_t i = 0; i < s_saved_config_count; ++i) {
        if (strcmp(s_saved_config[i].id, m->id) != 0)
            continue;
        ConfigEntry* c = config_find(m, s_saved_config[i].key);
        if (c == NULL)
            continue;
        const char* raw = s_saved_config[i].value;
        JsonValue* v = json_parse(raw, strlen(raw), NULL, 0);
        if (v == NULL)
            continue;
        if (c->type == CFG_NUMBER && v->type == JSON_NUMBER) {
            c->num_value = clamp_config(c, v->number);
            c->overridden = true;
        } else if (c->type == CFG_BOOL && v->type == JSON_BOOL) {
            c->bool_value = v->boolean;
            c->overridden = true;
        } else if (c->type == CFG_STRING && v->type == JSON_STRING) {
            free(c->str_value);
            c->str_value = xstrdup(v->string);
            c->overridden = true;
        }
        json_free(v);
    }
}

static void count_tunables(Mod* m) {
    const JsonValue* fighters = json_get(json_get(m->manifest, "tunables"), "fighters");
    size_t n = 0;
    JSON_FOREACH(f, fighters)
    n += f->type == JSON_OBJECT ? f->count : 0;
    m->info.tunable_count = n;
    /* Characters the manifest declares; shown before activation, which is
     * when they are actually registered (register_fighters). */
    const JsonValue* packs = json_get(m->manifest, "fighters");
    m->info.fighter_count = packs && packs->type == JSON_ARRAY ? packs->count : 0;
    const JsonValue* maps = json_get(m->manifest, "stages");
    m->info.stage_count = maps && maps->type == JSON_ARRAY ? maps->count : 0;
    const JsonValue* items = json_get(m->manifest, "items");
    m->info.item_count = items && items->type == JSON_ARRAY ? items->count : 0;
}

static void load_mod_folder(const char* dir) {
    char* manifest_path = join_path(dir, "mod.json");
    if (manifest_path == NULL)
        return;
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(manifest_path, &info) || info.type != SDL_PATHTYPE_FILE) {
        free(manifest_path);
        return; /* not a mod folder */
    }
    Mod* m = (Mod*)calloc(1, sizeof(Mod));
    if (m == NULL) {
        free(manifest_path);
        return;
    }
    m->dir = join_path(dir, "");
    if (m->dir) {
        size_t n = strlen(m->dir);
        if (n && m->dir[n - 1] == '/')
            m->dir[n - 1] = '\0';
    }
    char err[256];
    m->manifest = json_parse_file(manifest_path, err, sizeof(err));
    free(manifest_path);

    const char* folder = strrchr(m->dir, '/');
    folder = folder ? folder + 1 : m->dir;
    if (m->manifest == NULL || m->manifest->type != JSON_OBJECT) {
        m->id = xstrdup(folder);
        m->name = xstrdup(folder);
        m->version = xstrdup("?");
        mod_error(m, "mod.json: %s", m->manifest ? "top level must be an object" : err);
    } else {
        const JsonValue* j = m->manifest;
        m->id = xstrdup(json_string(json_get(j, "id"), folder));
        m->name = xstrdup(json_string(json_get(j, "name"), m->id));
        m->version = xstrdup(json_string(json_get(j, "version"), "0.0.0"));
        m->author = xstrdup(json_string(json_get(j, "author"), ""));
        m->description = xstrdup(json_string(json_get(j, "description"), ""));
        m->info.priority = (int)json_number(json_get(j, "priority"), 0);
        m->info.affects_gameplay = json_bool(json_get(j, "affects_gameplay"), true);
        m->enabled_by_default = json_bool(json_get(j, "enabled_by_default"), true);
        m->api_version = (int)json_number(json_get(j, "api_version"), 1);
        m->deps = read_string_list(json_get(j, "dependencies"), &m->dep_count);
        m->conflicts = read_string_list(json_get(j, "conflicts"), &m->conflict_count);
        if (!valid_id(m->id))
            mod_error(m, "id \"%s\" must be 1-64 characters of a-z, 0-9, '.', '_' or '-'", m->id);
        if (m->api_version > MELEE_MOD_API_VERSION)
            mod_error(
                m, "needs mod API v%d; this build has v%d", m->api_version, MELEE_MOD_API_VERSION);
        read_config_schema(m);

        const char* files = json_string(json_get(j, "files"), "files");
        m->files_dir = join_path(m->dir, files);
        if (m->files_dir && is_dir(m->files_dir)) {
            FileWalk w = {m, 0};
            SDL_EnumerateDirectory(m->files_dir, walk_files, &w);
        }

        const char* plugin = json_string(json_get(j, "plugin"), NULL);
        if (plugin != NULL) {
            /* "plugin": "fastfox" -> fastfox.dll / .so / .dylib, unless the
             * manifest already spelled an extension out. */
            const char* dot = strrchr(plugin, '.');
            char* name = dot && strchr(dot, '/') == NULL ? xstrdup(plugin) :
                                                           xasprintf("%s%s", plugin, PLUGIN_EXT);
            m->plugin = name ? join_path(m->dir, name) : NULL;
            free(name);
            m->info.has_plugin = true;
#if defined(__EMSCRIPTEN__)
            mod_error(m, "native plugins are not supported in the browser build");
#else
            if (m->plugin && (!SDL_GetPathInfo(m->plugin, &info) || info.type != SDL_PATHTYPE_FILE))
                mod_error(m, "plugin %s not found", m->plugin);
#endif
        }
        count_tunables(m);
    }

    if (find_mod(m->id) != NULL) {
        pc_log_line("mods: %s in %s is shadowed by an earlier copy; ignored", m->id, m->dir);
        mod_free(m);
        return;
    }
    Mod** p = (Mod**)grow(s_mods, &s_mod_cap, s_mod_count + 1, sizeof(Mod*));
    if (p == NULL) {
        mod_free(m);
        return;
    }
    s_mods = p;
    s_mods[s_mod_count++] = m;
}

static SDL_EnumerationResult SDLCALL scan_root(
    void* userdata, const char* dirname, const char* fname) {
    (void)userdata;
    if (fname[0] == '.')
        return SDL_ENUM_CONTINUE;
    char* path = join_path(dirname, fname);
    if (path && is_dir(path))
        load_mod_folder(path);
    free(path);
    return SDL_ENUM_CONTINUE;
}

static int mod_order_cmp(const void* a, const void* b) {
    const Mod* ma = *(const Mod* const*)a;
    const Mod* mb = *(const Mod* const*)b;
    if (ma->info.priority != mb->info.priority)
        return ma->info.priority < mb->info.priority ? -1 : 1;
    return strcmp(ma->id, mb->id);
}

/* Stable topological pass after the priority sort: a mod is moved after
 * everything it depends on. Cycles are reported, not looped on. */
static void order_by_dependencies(void) {
    Mod** out = (Mod**)calloc(s_mod_count ? s_mod_count : 1, sizeof(Mod*));
    unsigned char* state = (unsigned char*)calloc(s_mod_count ? s_mod_count : 1, 1);
    if (out == NULL || state == NULL) {
        free(out);
        free(state);
        return;
    }
    size_t placed = 0;
    for (int pass = 0; pass < (int)s_mod_count + 1 && placed < s_mod_count; ++pass) {
        for (size_t i = 0; i < s_mod_count; ++i) {
            if (state[i])
                continue;
            bool ready = true;
            for (size_t d = 0; d < s_mods[i]->dep_count && ready; ++d)
                for (size_t k = 0; k < s_mod_count; ++k)
                    if (!state[k] && k != i && strcmp(s_mods[k]->id, s_mods[i]->deps[d]) == 0)
                        ready = false;
            if (ready) {
                state[i] = 1;
                out[placed++] = s_mods[i];
            }
        }
    }
    for (size_t i = 0; i < s_mod_count; ++i)
        if (!state[i]) {
            mod_error(s_mods[i], "dependency cycle");
            out[placed++] = s_mods[i];
        }
    memcpy(s_mods, out, s_mod_count * sizeof(Mod*));
    free(out);
    free(state);
}

static void refresh_info(Mod* m) {
    m->info.id = m->id;
    m->info.name = m->name;
    m->info.version = m->version;
    m->info.author = m->author ? m->author : "";
    m->info.description = m->description ? m->description : "";
    m->info.dir = m->dir;
    m->info.file_count = m->file_count;
    m->info.error = m->error;
}

static void init_paths(void) {
    if (s_user_dir != NULL)
        return;
    char* pref = SDL_GetPrefPath(NULL, "melee-pc");
    if (pref != NULL) {
        s_user_dir = join_path(pref, "mods");
        s_cfg_path = join_path(pref, "mods.cfg");
        SDL_free(pref);
        if (s_user_dir)
            SDL_CreateDirectory(s_user_dir);
    }
}

void pc_mods_scan(void) {
    if (s_activated)
        return;
    init_paths();
    for (size_t i = 0; i < s_mod_count; ++i)
        mod_free(s_mods[i]);
    s_mod_count = 0;
    cfg_load();

    if (s_user_dir && is_dir(s_user_dir))
        SDL_EnumerateDirectory(s_user_dir, scan_root, NULL);
    const char* base = SDL_GetBasePath();
    if (base != NULL) {
        char* bundled = join_path(base, "mods");
        if (bundled && is_dir(bundled) && (s_user_dir == NULL || strcmp(bundled, s_user_dir) != 0))
            SDL_EnumerateDirectory(bundled, scan_root, NULL);
        free(bundled);
    }
    if (s_mod_count > 1)
        qsort(s_mods, s_mod_count, sizeof(Mod*), mod_order_cmp);
    order_by_dependencies();

    for (size_t i = 0; i < s_mod_count; ++i) {
        Mod* m = s_mods[i];
        m->info.enabled = m->enabled_by_default;
        for (size_t k = 0; k < s_saved_enable_count; ++k)
            if (strcmp(s_saved_enable[k].id, m->id) == 0)
                m->info.enabled = s_saved_enable[k].enabled;
        apply_saved_config(m);
        refresh_info(m);
    }
    s_scanned = true;
    pc_log_line(
        "mods: found %zu mod(s) (user folder: %s)", s_mod_count, s_user_dir ? s_user_dir : "none");
}

size_t pc_mods_count(void) {
    return s_mod_count;
}

const PcModInfo* pc_mods_info(size_t index) {
    if (index >= s_mod_count)
        return NULL;
    refresh_info(s_mods[index]);
    return &s_mods[index]->info;
}

bool pc_mods_set_enabled(const char* id, bool enabled) {
    Mod* m = id ? find_mod(id) : NULL;
    if (m == NULL)
        return false;
    m->info.enabled = enabled;
    saved_set_enable(id, enabled);
    cfg_save();
    return true;
}

const char* pc_mods_user_dir(void) {
    init_paths();
    return s_user_dir;
}

/* The packs a manifest declares, in manifest order: characters, map packs,
 * then items. */
static const char* const s_pack_lists[3] = {"fighters", "stages", "items"};
static const char* const s_pack_kinds[3] = {"character", "stage", "item"};

static const JsonValue* pack_at(const Mod* m, size_t index, int* list) {
    for (int l = 0; l < 3; ++l) {
        const JsonValue* arr = json_get(m->manifest, s_pack_lists[l]);
        if (arr == NULL || arr->type != JSON_ARRAY)
            continue;
        if (index < arr->count) {
            *list = l;
            return json_at(arr, index);
        }
        index -= arr->count;
    }
    return NULL;
}

size_t pc_mods_pack_count(size_t mod_index) {
    if (mod_index >= s_mod_count)
        return 0;
    size_t n = 0;
    for (int l = 0; l < 3; ++l) {
        const JsonValue* arr = json_get(s_mods[mod_index]->manifest, s_pack_lists[l]);
        if (arr != NULL && arr->type == JSON_ARRAY)
            n += arr->count;
    }
    return n;
}

bool pc_mods_pack_info(size_t mod_index, size_t index, PcModPack* out) {
    if (mod_index >= s_mod_count)
        return false;
    const Mod* m = s_mods[mod_index];
    int list = 0;
    const JsonValue* p = pack_at(m, index, &list);
    const char* local = json_string(json_get(p, "id"), NULL);
    if (p == NULL || local == NULL)
        return false;
    snprintf(out->id, sizeof(out->id), "%s/%s", m->id, local);
    out->name = json_string(json_get(p, "name"), local);
    out->kind = s_pack_kinds[list];
    out->enabled = pack_enabled(out->id);
    return true;
}

bool pc_mods_set_pack_enabled(const char* full_id, bool enabled) {
    if (full_id == NULL || s_activated)
        return false;
    saved_set_pack(full_id, enabled);
    cfg_save();
    return true;
}

/* ---- virtual disc ------------------------------------------------------------ */

typedef struct OverlayHandle {
    FILE* f;
} OverlayHandle;

static void* overlay_open(void* userdata) {
    const ModFile* file = (const ModFile*)userdata;
    FILE* f = fopen(file->host_path, "rb");
    if (f == NULL) {
        pc_log_line("mods: cannot open %s", file->host_path);
        return NULL;
    }
    OverlayHandle* h = (OverlayHandle*)malloc(sizeof(OverlayHandle));
    if (h == NULL) {
        fclose(f);
        return NULL;
    }
    h->f = f;
    return h;
}

static void overlay_close(void* handle) {
    OverlayHandle* h = (OverlayHandle*)handle;
    if (h == NULL)
        return;
    fclose(h->f);
    free(h);
}

static int64_t overlay_read(void* handle, uint8_t* buf, size_t len) {
    OverlayHandle* h = (OverlayHandle*)handle;
    if (h == NULL)
        return -1;
    size_t got = fread(buf, 1, len, h->f);
    if (got < len && ferror(h->f))
        return -1;
    return (int64_t)got;
}

static int64_t overlay_seek(void* handle, int64_t offset, int32_t whence) {
    OverlayHandle* h = (OverlayHandle*)handle;
    if (h == NULL)
        return -1;
#if defined(_WIN32)
    if (_fseeki64(h->f, offset, whence) != 0)
        return -1;
    return _ftelli64(h->f);
#else
    if (fseeko(h->f, (off_t)offset, whence) != 0)
        return -1;
    return (int64_t)ftello(h->f);
#endif
}

static bool path_ieq(const char* a, const char* b) {
    for (; *a && *b; ++a, ++b)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return false;
    return *a == *b;
}

static Overlay* overlay_find(const char* disc_path) {
    for (size_t i = 0; i < s_overlay_count; ++i)
        if (path_ieq(s_overlays[i].file->disc_path, disc_path))
            return &s_overlays[i];
    return NULL;
}

static void build_overlays(void) {
    /* Load order is ascending, so a later mod simply replaces an earlier
     * mod's entry for the same path. */
    for (size_t i = 0; i < s_mod_count; ++i) {
        Mod* m = s_mods[i];
        if (!m->info.active)
            continue;
        for (size_t k = 0; k < m->file_count; ++k) {
            Overlay* o = overlay_find(m->files[k].disc_path);
            if (o != NULL) {
                pc_log_line(
                    "mods: %s overrides %s from %s", m->id, m->files[k].disc_path, o->mod->id);
                o->file = &m->files[k];
                o->mod = m;
                continue;
            }
            Overlay* p =
                (Overlay*)grow(s_overlays, &s_overlay_cap, s_overlay_count + 1, sizeof(Overlay));
            if (p == NULL)
                return;
            s_overlays = p;
            s_overlays[s_overlay_count].file = &m->files[k];
            s_overlays[s_overlay_count].mod = m;
            ++s_overlay_count;
        }
    }
    if (s_overlay_count == 0)
        return;

    static const AuroraOverlayCallbacks callbacks = {
        overlay_open, overlay_close, overlay_read, overlay_seek};
    aurora_dvd_overlay_callbacks(&callbacks);
    s_aurora_files = (AuroraOverlayFile*)calloc(s_overlay_count, sizeof(AuroraOverlayFile));
    s32* entries = (s32*)calloc(s_overlay_count, sizeof(s32));
    if (s_aurora_files == NULL || entries == NULL) {
        free(entries);
        return;
    }
    for (size_t i = 0; i < s_overlay_count; ++i) {
        s_aurora_files[i].fileName = s_overlays[i].file->disc_path;
        s_aurora_files[i].userData = s_overlays[i].file;
        s_aurora_files[i].size = (size_t)s_overlays[i].file->size;
    }
    aurora_dvd_overlay_files(s_aurora_files, s_overlay_count, entries);
    const s32 base = aurora_dvd_base_entry_count();
    size_t replaced = 0, added = 0, rejected = 0;
    for (size_t i = 0; i < s_overlay_count; ++i) {
        if (entries[i] < 0)
            ++rejected;
        else if (entries[i] < base)
            ++replaced;
        else
            ++added;
    }
    free(entries);
    pc_log_line("mods: virtual disc: %zu file(s) replaced, %zu added, %zu rejected", replaced,
        added, rejected);
}

/* ---- tunables ------------------------------------------------------------------- */

static bool add_patch(Mod* m, int kind, int attr, MeleeAttrOp op, float value, bool from_api) {
    AttrPatch* p = (AttrPatch*)grow(s_patches, &s_patch_cap, s_patch_count + 1, sizeof(AttrPatch));
    if (p == NULL)
        return false;
    s_patches = p;
    s_patches[s_patch_count++] = (AttrPatch){m, kind, attr, op, value, from_api};
    return true;
}

/* A tunable operand: a number, or "$key" naming a number/bool config value. */
static bool resolve_operand(Mod* m, const JsonValue* v, float* out) {
    if (v == NULL)
        return false;
    if (v->type == JSON_NUMBER) {
        *out = (float)v->number;
        return true;
    }
    if (v->type == JSON_BOOL) {
        *out = v->boolean ? 1.0f : 0.0f;
        return true;
    }
    if (v->type == JSON_STRING && v->string[0] == '$') {
        ConfigEntry* c = config_find(m, v->string + 1);
        if (c == NULL || c->type == CFG_STRING)
            return false;
        *out = c->type == CFG_BOOL ? (c->bool_value ? 1.0f : 0.0f) : (float)c->num_value;
        return true;
    }
    return false;
}

/* ---- character packs ------------------------------------------------------ */

static bool on_virtual_disc(const char* name) {
    if (name == NULL || name[0] == '\0')
        return false;
    char path[300];
    snprintf(path, sizeof(path), "%s%s", name[0] == '/' ? "" : "/", name);
    return DVDConvertPathToEntrynum(path) >= 0;
}

static void compile_patch_object(
    Mod* m, const JsonValue* fields, int kind, int also, const char* who);

/* Size of a file on the virtual disc, or -1. */
static long disc_file_size(const char* name) {
    char path[300];
    snprintf(path, sizeof(path), "%s%s", name[0] == '/' ? "" : "/", name);
    const s32 entry = DVDConvertPathToEntrynum(path);
    DVDFileInfo info;
    if (entry < 0 || !DVDFastOpen(entry, &info))
        return -1;
    const long size = (long)info.length;
    DVDClose(&info);
    return size;
}

static bool ends_with_ci(const char* s, const char* suffix) {
    const size_t n = strlen(s), k = strlen(suffix);
    if (k > n)
        return false;
    for (size_t i = 0; i < k; ++i)
        if (tolower((unsigned char)s[n - k + i]) != tolower((unsigned char)suffix[i]))
            return false;
    return true;
}

/* "replace_files": { "<disc file>": "<file to load instead>" } on a fighter
 * or stage: swaps other disc files while that pack is in the match. */
static void register_aliases(
    const JsonValue* obj, PcAliasOwner owner, int pack, const char* full_id) {
    const JsonValue* map = json_get(obj, "replace_files");
    if (map == NULL)
        return;
    if (map->type != JSON_OBJECT) {
        pc_log_line("mods: %s: \"replace_files\" must be an object", full_id);
        return;
    }
    int added = 0;
    JSON_FOREACH(e, map) {
        const char* to = json_string(e, NULL);
        if (to == NULL) {
            pc_log_line("mods: %s: replace_files \"%s\" needs a file name", full_id, e->key);
            continue;
        }
        /* Loaded once and kept across scenes, so a per-match swap never
         * reaches them; replace them with a plain file in files/ instead. */
        static const char* const resident[] = {
            "EfMnData.dat",
            "EfCoData.dat",
            "ItCo.dat",
            "ItCo.usd",
            "IfAll.dat",
            "IfAll.usd",
            "LbRb.dat",
            "audio/main.ssm",
        };
        bool is_resident = false;
        for (size_t r = 0; r < sizeof(resident) / sizeof(resident[0]); ++r)
            if (path_ieq(e->key + (e->key[0] == '/'), resident[r]))
                is_resident = true;
        if (is_resident) {
            pc_log_line("mods: %s: replace_files cannot swap %s, which stays loaded between "
                        "matches; replace it with a file in files/ instead",
                full_id, e->key);
            continue;
        }
        const long from_size = disc_file_size(e->key);
        const long to_size = disc_file_size(to);
        if (from_size < 0 || to_size < 0) {
            pc_log_line("mods: %s: replace_files %s -> %s: %s is not on the virtual disc", full_id,
                e->key, to, from_size < 0 ? e->key : to);
            continue;
        }
        /* Sound banks load into a fixed budget sized for the game's own banks
         * (lbaudio_ax: "FGM load size is over"), so a bigger one cannot load. */
        if (ends_with_ci(e->key, ".ssm") && to_size > from_size) {
            pc_log_line("mods: %s: replace_files %s -> %s: a sound bank cannot be bigger than "
                        "the one it replaces (%ld > %ld bytes)",
                full_id, e->key, to, to_size, from_size);
            continue;
        }
        if (pc_alias_add(owner, pack, e->key, to))
            ++added;
    }
    if (added > 0)
        pc_log_line("mods: %s: %d file(s) replaced while it is in a match", full_id, added);
}

/* "fighters": [{ "id", "name", "base", "data", "data_symbol", "animations",
 * "costumes": ["file", {"file", "joint", "matanim"}...], "tunables": {...} }]
 * Registered after the overlay, so their files can come from any mod. */
/* Everything one fighter half declares: files, costumes and their art. The
 * strings stay owned by the manifest except the joined art paths. */
typedef struct FighterParse {
    PcRosterCostume costumes[PC_ROSTER_MAX_COSTUMES];
    char* art_paths[PC_ROSTER_MAX_COSTUMES * 2];
    size_t art_count;
    PcRosterFighter desc;
    const char* missing; /* first file that is not on the virtual disc */
} FighterParse;

static void parse_fighter_half(Mod* m, const JsonValue* f, FighterParse* out) {
    memset(out, 0, sizeof(*out));
    const char* portrait_all = json_string(json_get(f, "portrait"), NULL);
    const char* stock_all = json_string(json_get(f, "stock_icon"), NULL);
    size_t count = 0;
    JSON_FOREACH(c, json_get(f, "costumes")) {
        if (count == PC_ROSTER_MAX_COSTUMES)
            break;
        PcRosterCostume* pc = &out->costumes[count++];
        const char* portrait = portrait_all;
        const char* stock = stock_all;
        if (c->type == JSON_STRING) {
            pc->file = c->string;
        } else {
            pc->file = json_string(json_get(c, "file"), NULL);
            pc->joint = json_string(json_get(c, "joint"), NULL);
            pc->matanim = json_string(json_get(c, "matanim"), NULL);
            portrait = json_string(json_get(c, "portrait"), portrait);
            stock = json_string(json_get(c, "stock_icon"), stock);
        }

        if (portrait != NULL) {
            out->art_paths[out->art_count] = join_path(m->dir, portrait);
            pc->portrait_path = out->art_paths[out->art_count++];
        }
        if (stock != NULL) {
            out->art_paths[out->art_count] = join_path(m->dir, stock);
            pc->stock_path = out->art_paths[out->art_count++];
        }
        if (out->missing == NULL && !on_virtual_disc(pc->file))
            out->missing = pc->file ? pc->file : "(a costume without \"file\")";
    }
    out->desc.data_file = json_string(json_get(f, "data"), NULL);
    out->desc.data_symbol = json_string(json_get(f, "data_symbol"), NULL);
    out->desc.anim_file = json_string(json_get(f, "animations"), NULL);
    out->desc.costume_count = count;
    out->desc.costumes = out->costumes;
    if (!on_virtual_disc(out->desc.data_file))
        out->missing = out->desc.data_file ? out->desc.data_file : "(no \"data\")";
    else if (!on_virtual_disc(out->desc.anim_file))
        out->missing = out->desc.anim_file ? out->desc.anim_file : "(no \"animations\")";
}

static void free_fighter_half(FighterParse* p) {
    for (size_t a = 0; a < p->art_count; ++a)
        free(p->art_paths[a]); /* the registry copied what it keeps */
    p->art_count = 0;
}

/* "fighters": [{ "id", "name", "base", "icon", "portrait", "stock_icon",
 * "data", "data_symbol", "animations", "costumes": [...], "tunables",
 * "partner": { "data", "data_symbol", "animations", "costumes" } }]
 * Registered after the overlay, so their files can come from any mod. */
static void register_fighters(Mod* m) {
    const JsonValue* list = json_get(m->manifest, "fighters");
    if (list == NULL)
        return;
    if (list->type != JSON_ARRAY) {
        pc_log_line("mods: %s: \"fighters\" must be an array", m->id);
        return;
    }
    JSON_FOREACH(f, list) {
        const char* local = json_string(json_get(f, "id"), NULL);
        const char* base_name = json_string(json_get(f, "base"), NULL);
        if (local == NULL || base_name == NULL) {
            pc_log_line("mods: %s: every fighter needs \"id\" and \"base\"", m->id);
            continue;
        }
        char full_id[160];
        snprintf(full_id, sizeof(full_id), "%s/%s", m->id, local);
        if (!pack_enabled(full_id)) {
            pc_log_line("mods: %s: switched off in the launcher", full_id);
            continue;
        }
        int also = -1;
        /* "Ice Climbers" names Popo (also = Nana): Nana comes from "partner". */
        const int base = pc_mod_fighter_from_name(base_name, &also);
        if (base < 0) {
            pc_log_line("mods: %s: unknown base \"%s\"", full_id, base_name);
            continue;
        }
        FighterParse main_half, other_half;
        parse_fighter_half(m, f, &main_half);
        const JsonValue* partner = json_get(f, "partner");
        if (partner != NULL)
            parse_fighter_half(m, partner, &other_half);
        else
            memset(&other_half, 0, sizeof(other_half));
        const char* missing = main_half.missing ? main_half.missing : other_half.missing;
        const char* icon = json_string(json_get(f, "icon"), NULL);
        char* icon_path = icon ? join_path(m->dir, icon) : NULL;
        const char* announcer = json_string(json_get(f, "announcer"), NULL);
        char* announcer_path = announcer ? join_path(m->dir, announcer) : NULL;
        const char* theme = json_string(json_get(f, "victory_theme"), NULL);
        char* theme_path = theme ? join_path(m->dir, theme) : NULL;
        /* Results-screen and HUD art: series emblem, name label, winner banner. */
        static const char* const art_keys[3] = {"emblem", "name_image", "winner_name_image"};
        char* art[3];
        for (int k = 0; k < 3; ++k) {
            const char* rel = json_string(json_get(f, art_keys[k]), NULL);
            art[k] = rel ? join_path(m->dir, rel) : NULL;
        }
        PcRosterFighter desc = main_half.desc;
        desc.icon_path = icon_path;
        desc.announcer_path = announcer_path;
        desc.victory_theme_path = theme_path;
        desc.emblem_path = art[0];
        desc.name_image_path = art[1];
        desc.winner_name_path = art[2];
        desc.series_kind = -1;
        if (json_get(f, "series") != NULL) {
            int also_series = -1;
            const char* series = json_string(json_get(f, "series"), "");
            desc.series_kind = pc_mod_fighter_from_name(series, &also_series);
            if (desc.series_kind < 0) {
                pc_log_line("mods: %s: unknown \"series\" character \"%s\"; using the base's",
                    full_id, series);
                desc.series_kind = -1;
            }
        }
        desc.id = full_id;
        desc.name = json_string(json_get(f, "name"), local);
        desc.base_kind = base;
        desc.mod_id = m->id;
        desc.partner = partner != NULL ? &other_half.desc : NULL;
        int pack = -1;
        if (missing != NULL) {
            pc_log_line(
                "mods: %s: %s is not on the virtual disc; fighter skipped", full_id, missing);
        } else {
            char err[200];
            pack = pc_roster_add_fighter(&desc, err, sizeof(err));
            if (pack < 0)
                pc_log_line("mods: %s: %s", full_id, err);
        }
        free(icon_path);
        free(announcer_path);
        free(theme_path);
        for (int k = 0; k < 3; ++k)
            free(art[k]);
        free_fighter_half(&main_half);
        free_fighter_half(&other_half);
        if (pack < 0)
            continue;
        pc_log_line("mods: fighter %s (\"%s\", base %s, %d costume(s)%s) -> pack %d", full_id,
            desc.name, pc_mod_fighter_name(base), pc_roster_fighter_costumes(pack),
            pc_roster_partner(pack) >= 0 ? ", with partner" : "", pack + 1);
        register_aliases(f, PC_ALIAS_FIGHTER, pack, full_id);
        const JsonValue* tunables = json_get(f, "tunables");
        if (tunables != NULL)
            compile_patch_object(m, tunables, pc_roster_asset_kind(pack), -1, full_id);
        const JsonValue* partner_tunables = partner ? json_get(partner, "tunables") : NULL;
        if (partner_tunables != NULL && pc_roster_partner(pack) >= 0)
            compile_patch_object(
                m, partner_tunables, pc_roster_asset_kind(pc_roster_partner(pack)), -1, full_id);
    }
}

/* "stages": [{ "id", "name", "base", "file", "icon", "preview", "name_image",
 * "music" }] -- new stage-select slots running a base stage's code with
 * their own stage file. */
static void register_stages(Mod* m) {
    const JsonValue* list = json_get(m->manifest, "stages");
    if (list == NULL)
        return;
    if (list->type != JSON_ARRAY) {
        pc_log_line("mods: %s: \"stages\" must be an array", m->id);
        return;
    }
    JSON_FOREACH(st, list) {
        const char* local = json_string(json_get(st, "id"), NULL);
        const char* base_name = json_string(json_get(st, "base"), NULL);
        const char* file = json_string(json_get(st, "file"), NULL);
        if (local == NULL || base_name == NULL || file == NULL) {
            pc_log_line("mods: %s: every stage needs \"id\", \"base\" and \"file\"", m->id);
            continue;
        }
        char full_id[160];
        snprintf(full_id, sizeof(full_id), "%s/%s", m->id, local);
        if (!pack_enabled(full_id)) {
            pc_log_line("mods: %s: switched off in the launcher", full_id);
            continue;
        }
        const int base = pc_stage_from_name(base_name);
        if (base < 0) {
            pc_log_line("mods: %s: unknown base stage \"%s\"", full_id, base_name);
            continue;
        }
        if (!on_virtual_disc(file)) {
            pc_log_line("mods: %s: %s is not on the virtual disc; stage skipped", full_id, file);
            continue;
        }
        char* paths[4] = {NULL, NULL, NULL, NULL};
        const char* keys[4] = {"icon", "preview", "name_image", "music"};
        for (int k = 0; k < 4; ++k) {
            const char* rel = json_string(json_get(st, keys[k]), NULL);
            paths[k] = rel ? join_path(m->dir, rel) : NULL;
        }
        PcStagePackDesc desc = {
            .id = full_id,
            .name = json_string(json_get(st, "name"), local),
            .mod_id = m->id,
            .base_stkind = base,
            .file = file,
            .icon_path = paths[0],
            .preview_path = paths[1],
            .name_image_path = paths[2],
            .music_path = paths[3],
        };
        char err[200];
        const int pack = pc_stages_add(&desc, err, sizeof(err));
        for (int k = 0; k < 4; ++k)
            free(paths[k]);
        if (pack < 0) {
            pc_log_line("mods: %s: %s", full_id, err);
            continue;
        }
        pc_log_line("mods: stage %s (\"%s\", base %s) -> map pack %d", full_id, desc.name,
            pc_stage_display_name(base), pack + 1);
        register_aliases(st, PC_ALIAS_STAGE, pack, full_id);
    }
}

/* A pack named in a manifest: "<pack id>" of this mod, or "<mod id>/<pack id>". */
static int find_roster_pack(const Mod* m, const char* name) {
    char full[200];
    snprintf(full, sizeof(full), "%s/%s", m->id, name);
    int pack = pc_roster_find_fighter(full);
    return pack >= 0 ? pack : pc_roster_find_fighter(name);
}

static int find_stage_pack(const Mod* m, const char* name) {
    char full[200];
    snprintf(full, sizeof(full), "%s/%s", m->id, name);
    for (int i = 0; i < pc_stages_count(); ++i)
        if (strcmp(pc_stages_id(i), full) == 0 || strcmp(pc_stages_id(i), name) == 0)
            return i;
    return -1;
}

/* "fighter" / "stage" on a character, projectile or stage item: which spawns
 * of the base the pack replaces. False (logged) when a name is unknown. */
static bool resolve_item_filters(
    const Mod* m, const JsonValue* it, const char* full_id, int base, PcItemPackDesc* desc) {
    const char* fighter = json_string(json_get(it, "fighter"), NULL);
    const char* stage = json_string(json_get(it, "stage"), NULL);
    desc->owner_pack = desc->owner_kind = desc->stage_pack = desc->stage_kind = -1;
    if ((fighter || stage) && pc_item_base_class(base) != PC_ITEM_OWNED) {
        pc_log_line("mods: %s: \"fighter\" and \"stage\" only apply to character, projectile "
                    "and stage items; ignored",
            full_id);
        return true;
    }
    if (fighter != NULL) {
        int also = -1;
        desc->owner_pack = find_roster_pack(m, fighter);
        if (desc->owner_pack < 0) {
            const int kind = pc_mod_fighter_from_name(fighter, &also);
            if (kind < 0) {
                pc_log_line("mods: %s: unknown fighter \"%s\" (a character or a character "
                            "pack id)",
                    full_id, fighter);
                return false;
            }
            desc->owner_kind = kind;
        }
    }
    if (stage != NULL) {
        desc->stage_pack = find_stage_pack(m, stage);
        if (desc->stage_pack < 0) {
            desc->stage_kind = pc_stage_from_name(stage);
            if (desc->stage_kind < 0) {
                pc_log_line(
                    "mods: %s: unknown stage \"%s\" (a VS stage or a map pack id)", full_id, stage);
                return false;
            }
        }
    }
    return true;
}

/* "items": [{ "id", "name", "base", "file", "symbol", "frequency", "fighter",
 * "stage" }] -- new items running a base item's code with their own Article. */
static void register_items(Mod* m) {
    const JsonValue* list = json_get(m->manifest, "items");
    if (list == NULL)
        return;
    if (list->type != JSON_ARRAY) {
        pc_log_line("mods: %s: \"items\" must be an array", m->id);
        return;
    }
    JSON_FOREACH(it, list) {
        const char* local = json_string(json_get(it, "id"), NULL);
        const char* base_name = json_string(json_get(it, "base"), NULL);
        const char* file = json_string(json_get(it, "file"), NULL);
        if (local == NULL || base_name == NULL || file == NULL) {
            pc_log_line("mods: %s: every item needs \"id\", \"base\" and \"file\"", m->id);
            continue;
        }
        char full_id[160];
        snprintf(full_id, sizeof(full_id), "%s/%s", m->id, local);
        if (!pack_enabled(full_id)) {
            pc_log_line("mods: %s: switched off in the launcher", full_id);
            continue;
        }
        const int base = pc_item_from_name(base_name);
        if (base < 0) {
            pc_log_line("mods: %s: unknown base item \"%s\" (a common item, a Pokemon, or a "
                        "character, projectile or stage item by its internal name)",
                full_id, base_name);
            continue;
        }
        if (!on_virtual_disc(file)) {
            pc_log_line("mods: %s: %s is not on the virtual disc; item skipped", full_id, file);
            continue;
        }
        const double freq = json_number(json_get(it, "frequency"), 1.0);
        if (!(freq >= 0.0 && freq <= 100.0)) {
            pc_log_line("mods: %s: \"frequency\" must be between 0 and 100", full_id);
            continue;
        }
        PcItemPackDesc desc = {
            .id = full_id,
            .name = json_string(json_get(it, "name"), local),
            .mod_id = m->id,
            .base_kind = base,
            .file = file,
            .symbol = json_string(json_get(it, "symbol"), "itArticle"),
            .frequency = (float)freq,
        };
        if (!resolve_item_filters(m, it, full_id, base, &desc))
            continue;
        char err[200];
        const int pack = pc_items_add(&desc, err, sizeof(err));
        if (pack < 0) {
            pc_log_line("mods: %s: %s", full_id, err);
            continue;
        }
        pc_log_line("mods: item %s (\"%s\", base %s, frequency %.2f) -> item pack %d", full_id,
            desc.name, pc_item_display_name(base), freq, pack + 1);
    }
}

static void compile_tunables(Mod* m) {
    const JsonValue* tunables = json_get(m->manifest, "tunables");
    const JsonValue* fighters = json_get(tunables, "fighters");
    JSON_FOREACH(f, fighters) {
        int also = -1;
        int kind = pc_mod_fighter_from_name(f->key, &also);
        if (kind == PC_MOD_FIGHTER_UNKNOWN) {
            pc_log_line("mods: %s: unknown fighter \"%s\" in tunables", m->id, f->key);
            continue;
        }
        compile_patch_object(m, f, kind, also, f->key);
    }
}

static void compile_patch_object(
    Mod* m, const JsonValue* fields, int kind, int also, const char* who) {
    {
        JSON_FOREACH(field, fields) {
            int attr = pc_mod_attr_index(field->key);
            if (attr < 0) {
                pc_log_line("mods: %s: unknown fighter attribute \"%s\"", m->id, field->key);
                continue;
            }
            MeleeAttrOp op = MELEE_ATTR_SET;
            const JsonValue* operand = field;
            if (field->type == JSON_OBJECT) {
                if ((operand = json_get(field, "mul")) != NULL)
                    op = MELEE_ATTR_MUL;
                else if ((operand = json_get(field, "add")) != NULL)
                    op = MELEE_ATTR_ADD;
                else
                    operand = json_get(field, "set");
            }
            float value;
            if (!resolve_operand(m, operand, &value)) {
                pc_log_line("mods: %s: %s.%s needs a number, {\"set\"|\"mul\"|\"add\": n}, or "
                            "\"$config_key\"",
                    m->id, who, field->key);
                continue;
            }
            add_patch(m, kind, attr, op, value, false);
            if (also >= 0)
                add_patch(m, also, attr, op, value, false);
        }
    }
}

static float apply_op(float cur, MeleeAttrOp op, float v) {
    switch (op) {
    case MELEE_ATTR_MUL:
        return cur * v;
    case MELEE_ATTR_ADD:
        return cur + v;
    default:
        return v;
    }
}

void pc_mods_patch_fighter_attrs(int kind, void* co_attrs) {
    if (!s_activated || co_attrs == NULL)
        return;
    for (size_t i = 0; i < s_patch_count; ++i) {
        const AttrPatch* p = &s_patches[i];
        if (p->kind != MELEE_FIGHTER_ALL && p->kind != kind)
            continue;
        float cur = pc_mod_attr_get(co_attrs, p->attr);
        pc_mod_attr_set(co_attrs, p->attr, apply_op(cur, p->op, p->value));
    }
}

/* ---- hooks --------------------------------------------------------------------- */

static void dispatch(MeleeModHook hook, intptr_t arg) {
    /* By index against a snapshot of the count: a hook may register more
     * hooks (they run from the next dispatch) or unregister (fn -> NULL). */
    const size_t n = s_hook_count;
    for (size_t i = 0; i < n; ++i) {
        Hook* h = &s_hooks[i];
        if (h->fn != NULL && h->hook == hook)
            h->fn(hook, arg, h->user);
    }
}

static bool fight_scene(int kind) {
    return kind == MELEE_SCENE_VS || kind == MELEE_SCENE_SUDDEN_DEATH ||
           kind == MELEE_SCENE_TRAINING;
}

void pc_mods_on_scene(int scene_kind) {
    if (!s_activated)
        return;
    const int previous = s_scene;
    s_scene = scene_kind;
    pc_hud_art_reset();
    if (s_in_match) {
        s_in_match = false;
        dispatch(MELEE_HOOK_MATCH_END, previous);
    }
    s_pending_spawns = 0;
    dispatch(MELEE_HOOK_SCENE_CHANGE, scene_kind);
    if (fight_scene(scene_kind)) {
        s_in_match = true;
        dispatch(MELEE_HOOK_MATCH_START, scene_kind);
    }
}

void pc_mods_on_fighter_spawn(int port) {
    if (s_activated && port >= 0 && port < PC_MOD_MAX_PORTS)
        s_pending_spawns |= 1u << port;
}

void pc_mods_on_frame(void) {
    if (!s_activated)
        return;
    ++s_frame;
    if (s_hook_count == 0) {
        s_pending_spawns = 0;
        return;
    }
    if (s_pending_spawns != 0) {
        uint32_t spawns = s_pending_spawns;
        s_pending_spawns = 0;
        for (int port = 0; port < PC_MOD_MAX_PORTS; ++port)
            if (spawns & (1u << port))
                dispatch(MELEE_HOOK_FIGHTER_SPAWN, port);
    }
    dispatch(MELEE_HOOK_FRAME, (intptr_t)s_frame);
}

/* ---- plugin API ------------------------------------------------------------------ */

static void api_log(MeleeModHandle self, const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    pc_log_line("[%s] %s", self ? self->id : "mod", buf);
}

static const char* api_mod_id(MeleeModHandle self) {
    return self ? self->id : NULL;
}

static const char* api_mod_dir(MeleeModHandle self) {
    return self ? self->dir : NULL;
}

static bool api_mod_is_enabled(const char* id) {
    Mod* m = id ? find_mod(id) : NULL;
    return m != NULL && m->info.active;
}

static double api_config_number(MeleeModHandle self, const char* key, double fallback) {
    ConfigEntry* c = config_find(self, key);
    if (c == NULL)
        return fallback;
    if (c->type == CFG_BOOL)
        return c->bool_value ? 1.0 : 0.0;
    return c->type == CFG_NUMBER ? c->num_value : fallback;
}

static bool api_config_bool(MeleeModHandle self, const char* key, bool fallback) {
    ConfigEntry* c = config_find(self, key);
    if (c == NULL)
        return fallback;
    if (c->type == CFG_NUMBER)
        return c->num_value != 0;
    return c->type == CFG_BOOL ? c->bool_value : fallback;
}

static const char* api_config_string(MeleeModHandle self, const char* key, const char* fallback) {
    ConfigEntry* c = config_find(self, key);
    return c && c->type == CFG_STRING ? c->str_value : fallback;
}

static bool api_config_set_number(MeleeModHandle self, const char* key, double value) {
    ConfigEntry* c = config_find(self, key);
    if (c == NULL || c->type != CFG_NUMBER)
        return false;
    c->num_value = clamp_config(c, value);
    c->overridden = true;
    char buf[64];
    snprintf(buf, sizeof(buf), "%.17g", c->num_value);
    saved_set_config(self->id, key, buf);
    cfg_save();
    return true;
}

static bool api_config_set_bool(MeleeModHandle self, const char* key, bool value) {
    ConfigEntry* c = config_find(self, key);
    if (c == NULL || c->type != CFG_BOOL)
        return false;
    c->bool_value = value;
    c->overridden = true;
    saved_set_config(self->id, key, value ? "true" : "false");
    cfg_save();
    return true;
}

static bool api_config_set_string(MeleeModHandle self, const char* key, const char* value) {
    ConfigEntry* c = config_find(self, key);
    if (c == NULL || c->type != CFG_STRING || value == NULL)
        return false;
    free(c->str_value);
    c->str_value = xstrdup(value);
    c->overridden = true;
    /* Stored as a JSON string literal so it round-trips through cfg_load. */
    size_t n = strlen(value);
    char* lit = (char*)malloc(n * 6 + 3);
    if (lit == NULL)
        return false;
    char* o = lit;
    *o++ = '"';
    for (const unsigned char* s = (const unsigned char*)value; *s; ++s) {
        if (*s == '"' || *s == '\\') {
            *o++ = '\\';
            *o++ = (char)*s;
        } else if (*s < 0x20) {
            o += sprintf(o, "\\u%04x", *s);
        } else {
            *o++ = (char)*s;
        }
    }
    *o++ = '"';
    *o = '\0';
    saved_set_config(self->id, key, lit);
    free(lit);
    cfg_save();
    return true;
}

static int api_register_hook(
    MeleeModHandle self, MeleeModHook hook, MeleeModHookFn fn, void* user) {
    if (fn == NULL || (int)hook < 0 || hook >= MELEE_HOOK_COUNT)
        return 0;
    Hook* p = (Hook*)grow(s_hooks, &s_hook_cap, s_hook_count + 1, sizeof(Hook));
    if (p == NULL)
        return 0;
    s_hooks = p;
    const int id = s_next_hook_id++;
    s_hooks[s_hook_count++] = (Hook){id, self, hook, fn, user};
    return id;
}

static void api_unregister_hook(MeleeModHandle self, int id) {
    for (size_t i = 0; i < s_hook_count; ++i)
        if (s_hooks[i].id == id && s_hooks[i].mod == self)
            s_hooks[i].fn = NULL;
}

static bool api_set_fighter_attr(
    MeleeModHandle self, int32_t kind, const char* field, MeleeAttrOp op, float value) {
    const int attr = pc_mod_attr_index(field);
    const bool pack_kind =
        kind >= PC_MOD_PACK_FIRST && kind < PC_MOD_PACK_FIRST + pc_roster_fighter_count();
    if (attr < 0 ||
        (kind != MELEE_FIGHTER_ALL && !pack_kind && (kind < 0 || kind >= PC_MOD_FIGHTER_KINDS)) ||
        (int)op < MELEE_ATTR_SET || op > MELEE_ATTR_ADD)
        return false;
    return add_patch(self, kind, attr, op, value, true);
}

static void api_clear_fighter_attrs(MeleeModHandle self) {
    size_t w = 0;
    for (size_t i = 0; i < s_patch_count; ++i)
        if (!(s_patches[i].mod == self && s_patches[i].from_api))
            s_patches[w++] = s_patches[i];
    s_patch_count = w;
}

static int32_t api_fighter_attr_count(void) {
    return pc_mod_attr_count();
}

static const char* api_fighter_attr_name(int32_t index) {
    return pc_mod_attr_name(index);
}

static int32_t api_scene_kind(void) {
    return s_scene;
}

static bool api_in_match(void) {
    return s_in_match;
}

static bool api_is_netplay(void) {
    return pc_net_active();
}

static uint32_t api_frame_count(void) {
    return s_frame;
}

static bool api_get_player(int32_t port, MeleePlayerState* out) {
    if (out == NULL)
        return false;
    if (!s_in_match) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    return pc_mod_game_get_player(port, out);
}

static bool api_get_player_attr(int32_t port, const char* field, float* out) {
    const int attr = pc_mod_attr_index(field);
    void* attrs = s_in_match ? pc_mod_game_player_attrs(port) : NULL;
    if (attr < 0 || attrs == NULL || out == NULL)
        return false;
    *out = pc_mod_attr_get(attrs, attr);
    return true;
}

static bool api_set_player_attr(int32_t port, const char* field, float value) {
    const int attr = pc_mod_attr_index(field);
    void* attrs = s_in_match && !pc_net_active() ? pc_mod_game_player_attrs(port) : NULL;
    if (attr < 0 || attrs == NULL)
        return false;
    pc_mod_attr_set(attrs, attr, value);
    return true;
}

static bool api_set_player_percent(int32_t port, float percent) {
    if (!s_in_match || pc_net_active())
        return false;
    return pc_mod_game_set_percent(port, percent);
}

static const char* api_file_provider(const char* disc_path) {
    if (disc_path == NULL)
        return NULL;
    char* norm = disc_path[0] == '/' ? NULL : xasprintf("/%s", disc_path);
    Overlay* o = overlay_find(norm ? norm : disc_path);
    free(norm);
    return o ? o->mod->id : NULL;
}

static const MeleeModAPI s_api = {
    .api_version = MELEE_MOD_API_VERSION,
    .size = sizeof(MeleeModAPI),
    .game_version = NULL, /* filled in by pc_mods_activate */
    .log = api_log,
    .mod_id = api_mod_id,
    .mod_dir = api_mod_dir,
    .mod_is_enabled = api_mod_is_enabled,
    .config_number = api_config_number,
    .config_bool = api_config_bool,
    .config_string = api_config_string,
    .config_set_number = api_config_set_number,
    .config_set_bool = api_config_set_bool,
    .config_set_string = api_config_set_string,
    .register_hook = api_register_hook,
    .unregister_hook = api_unregister_hook,
    .set_fighter_attr = api_set_fighter_attr,
    .clear_fighter_attrs = api_clear_fighter_attrs,
    .fighter_attr_count = api_fighter_attr_count,
    .fighter_attr_name = api_fighter_attr_name,
    .scene_kind = api_scene_kind,
    .in_match = api_in_match,
    .is_netplay = api_is_netplay,
    .frame_count = api_frame_count,
    .get_player = api_get_player,
    .get_player_attr = api_get_player_attr,
    .set_player_attr = api_set_player_attr,
    .set_player_percent = api_set_player_percent,
    .file_provider = api_file_provider,
};
static MeleeModAPI s_api_runtime;

/* ---- activation ------------------------------------------------------------------- */

static void resolve_enabled_set(void) {
    for (size_t i = 0; i < s_mod_count; ++i) {
        Mod* m = s_mods[i];
        m->info.active = m->info.enabled && m->error == NULL;
    }
    /* Dependencies and conflicts can cascade, so iterate to a fixed point. */
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t i = 0; i < s_mod_count; ++i) {
            Mod* m = s_mods[i];
            if (!m->info.active)
                continue;
            for (size_t d = 0; d < m->dep_count; ++d) {
                Mod* dep = find_mod(m->deps[d]);
                if (dep == NULL || !dep->info.active) {
                    mod_error(m, "needs \"%s\", which is %s", m->deps[d],
                        dep == NULL ? "not installed" : "not enabled");
                    m->info.active = false;
                    changed = true;
                    break;
                }
            }
            for (size_t c = 0; m->info.active && c < m->conflict_count; ++c) {
                Mod* other = find_mod(m->conflicts[c]);
                if (other != NULL && other->info.active) {
                    mod_error(m, "conflicts with \"%s\"; disable one of them", other->id);
                    m->info.active = false;
                    changed = true;
                }
            }
        }
    }
}

static void load_plugin(Mod* m) {
#if defined(__EMSCRIPTEN__)
    (void)m;
#else
    if (m->plugin == NULL)
        return;
    m->lib = SDL_LoadObject(m->plugin);
    if (m->lib == NULL) {
        mod_error(m, "cannot load plugin: %s", SDL_GetError());
        m->info.active = false;
        return;
    }
    MeleeModInitFn init = (MeleeModInitFn)SDL_LoadFunction(m->lib, "melee_mod_init");
    m->shutdown = (MeleeModShutdownFn)SDL_LoadFunction(m->lib, "melee_mod_shutdown");
    if (init == NULL) {
        mod_error(m, "plugin does not export melee_mod_init");
    } else {
        const int rc = init(&s_api_runtime, m);
        if (rc == 0) {
            pc_log_line("mods: %s: plugin loaded", m->id);
            return;
        }
        mod_error(m, "plugin init returned %d", rc);
    }
    /* Failed: drop anything it registered before failing, then unload. */
    for (size_t i = 0; i < s_hook_count; ++i)
        if (s_hooks[i].mod == m)
            s_hooks[i].fn = NULL;
    api_clear_fighter_attrs(m);
    SDL_UnloadObject(m->lib);
    m->lib = NULL;
    m->shutdown = NULL;
    m->info.active = false;
#endif
}

void pc_mods_activate(void) {
    if (s_activated)
        return;
    if (!s_scanned)
        pc_mods_scan();
    s_api_runtime = s_api;
    s_api_runtime.game_version = pc_app_version();

    resolve_enabled_set();
    /* Activated before plugins load, so hooks and patches see a consistent
     * world; a plugin that fails is deactivated again in load_plugin. */
    s_activated = true;
    build_overlays();
    for (size_t i = 0; i < s_mod_count; ++i)
        if (s_mods[i]->info.active)
            register_fighters(s_mods[i]);
    for (size_t i = 0; i < s_mod_count; ++i)
        if (s_mods[i]->info.active)
            register_stages(s_mods[i]);
    for (size_t i = 0; i < s_mod_count; ++i)
        if (s_mods[i]->info.active)
            register_items(s_mods[i]);
    for (size_t i = 0; i < s_mod_count; ++i)
        if (s_mods[i]->info.active)
            compile_tunables(s_mods[i]);
    for (size_t i = 0; i < s_mod_count; ++i)
        if (s_mods[i]->info.active)
            load_plugin(s_mods[i]);

    size_t active = 0;
    for (size_t i = 0; i < s_mod_count; ++i) {
        Mod* m = s_mods[i];
        refresh_info(m);
        if (m->info.active) {
            ++active;
            pc_log_line("mods: active: %s %s (%s) - %zu file(s), %zu tunable(s)%s", m->id,
                m->version, m->name, m->file_count, m->info.tunable_count,
                m->lib ? ", plugin" : "");
        }
    }
    pc_log_line("mods: %zu of %zu mod(s) active, %zu attribute patch(es), gameplay hash %08x",
        active, s_mod_count, s_patch_count, pc_mods_gameplay_hash());
    dispatch(MELEE_HOOK_BOOT, 0);
}

void pc_mods_shutdown(void) {
    if (!s_activated)
        return;
    for (size_t i = s_mod_count; i-- > 0;) {
        Mod* m = s_mods[i];
        if (m->lib == NULL)
            continue;
        if (m->shutdown)
            m->shutdown();
        /* Deliberately not unloaded: the process is exiting and a plugin may
         * have left callbacks registered with SDL or the C runtime. */
    }
    s_hook_count = 0;
}

size_t pc_mods_active_count(void) {
    size_t n = 0;
    for (size_t i = 0; i < s_mod_count; ++i)
        n += s_mods[i]->info.active;
    return n;
}

uint32_t pc_mods_gameplay_hash(void) {
    if (!s_activated)
        return 0;
    uint32_t h = 2166136261u;
    bool any = false;
    for (size_t i = 0; i < s_mod_count; ++i) {
        const Mod* m = s_mods[i];
        if (!m->info.active || !m->info.affects_gameplay)
            continue;
        any = true;
        h = fnv1a_str(h, m->id);
        h = fnv1a_str(h, m->version);
        const uint32_t files = (uint32_t)m->file_count; /* same width on every platform */
        h = fnv1a(h, &files, sizeof(files));
        h = fnv1a(h, m->lib ? "P" : "-", 1);
    }
    for (size_t i = 0; i < s_overlay_count; ++i) {
        if (!s_overlays[i].mod->info.affects_gameplay)
            continue;
        h = fnv1a_str(h, s_overlays[i].file->disc_path);
        const uint64_t size = s_overlays[i].file->size;
        h = fnv1a(h, &size, sizeof(size));
    }
    for (size_t i = 0; i < s_patch_count; ++i) {
        const AttrPatch* p = &s_patches[i];
        int32_t fields[3] = {p->kind, p->attr, (int32_t)p->op};
        h = fnv1a(h, fields, sizeof(fields));
        h = fnv1a(h, &p->value, sizeof(p->value));
        any = true;
    }
    /* 0 is reserved for "no gameplay mods", which keeps unmodded peers
     * wire-compatible with builds that predate the mod loader. */
    if (pc_roster_fighter_count() > 0) {
        h = pc_roster_hash(h);
        any = true;
    }
    if (pc_stages_count() > 0) {
        h = pc_stages_hash(h);
        any = true;
    }
    if (pc_alias_count() > 0) {
        h = pc_alias_hash(h);
        any = true;
    }
    if (pc_items_count() > 0) {
        h = pc_items_hash(h);
        any = true;
    }
    return any ? (h ? h : 1) : 0;
}
