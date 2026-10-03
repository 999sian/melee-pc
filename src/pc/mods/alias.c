/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pc/compat.h"

#include "alias.h"

#include "../pc.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ALIASES 512
#define MAX_ACTIVE 64

typedef struct Alias {
    PcAliasOwner owner;
    int pack;
    char* from;     /* "/name", the form compared against */
    char* to_slash; /* "/name"; to_slash + 1 is the form without the slash */
} Alias;

static Alias s_aliases[MAX_ALIASES];
static size_t s_count;
/* The aliases that are on, as indexes into s_aliases. Written by the game
 * thread at scene boundaries, read when a file is resolved. */
static int s_active[MAX_ACTIVE];
static volatile int s_active_count;
/* Whether each active alias has been used yet this match: logged once, so a
 * mod author can see the swap happen. */
static bool s_used[MAX_ACTIVE];

static char* with_slash(const char* path) {
    const size_t n = strlen(path);
    char* s = (char*)malloc(n + 2);
    if (s == NULL)
        return NULL;
    if (path[0] == '/') {
        memcpy(s, path, n + 1);
    } else {
        s[0] = '/';
        memcpy(s + 1, path, n + 1);
    }
    return s;
}

static bool path_ieq(const char* a, const char* b) {
    for (; *a && *b; ++a, ++b)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return false;
    return *a == *b;
}

bool pc_alias_add(PcAliasOwner owner, int pack, const char* from, const char* to) {
    if (s_count == MAX_ALIASES || from == NULL || to == NULL)
        return false;
    Alias* a = &s_aliases[s_count];
    a->owner = owner;
    a->pack = pack;
    a->from = with_slash(from);
    a->to_slash = with_slash(to);
    if (a->from == NULL || a->to_slash == NULL) {
        free(a->from);
        free(a->to_slash);
        return false;
    }
    ++s_count;
    return true;
}

size_t pc_alias_count(void) {
    return s_count;
}

void pc_alias_activate(int stage_pack, const PcAliasPlayer* players, int count) {
    int active[MAX_ACTIVE];
    int n = 0;
    for (size_t i = 0; i < s_count && n < MAX_ACTIVE; ++i) {
        const Alias* a = &s_aliases[i];
        bool on = false;
        if (a->owner == PC_ALIAS_STAGE) {
            on = a->pack == stage_pack;
        } else {
            /* On when some player runs the pack and every player on the same
             * base runs that same pack. */
            int base = -1;
            for (int p = 0; p < count; ++p)
                if (players[p].pack == a->pack)
                    base = players[p].ckind;
            on = base >= 0;
            for (int p = 0; on && p < count; ++p)
                if (players[p].ckind == base && players[p].pack != a->pack)
                    on = false;
        }
        if (on)
            active[n++] = (int)i;
    }
    s_active_count = 0;
    memcpy(s_active, active, sizeof(int) * (size_t)n);
    memset(s_used, 0, sizeof(s_used));
    s_active_count = n;
    if (n > 0)
        pc_log_line("mods: %d file alias(es) on for this match", n);
}

void pc_alias_clear(void) {
    s_active_count = 0;
}

void pc_alias_save(PcAliasState* out) {
    out->count = s_active_count;
    memcpy(out->index, s_active, sizeof(int) * (size_t)out->count);
}

void pc_alias_restore(const PcAliasState* in) {
    s_active_count = 0;
    memcpy(s_active, in->index, sizeof(int) * (size_t)in->count);
    memset(s_used, 0, sizeof(s_used));
    s_active_count = in->count;
}

const char* pc_file_alias(const char* path) {
    const int n = s_active_count;
    if (n == 0 || path == NULL)
        return path;
    const bool slash = path[0] == '/';
    for (int i = 0; i < n; ++i) {
        const Alias* a = &s_aliases[s_active[i]];
        if (path_ieq(a->from + (slash ? 0 : 1), path)) {
            if (!s_used[i]) {
                s_used[i] = true;
                pc_log_line("mods: loading %s for %s", a->to_slash + 1, a->from + 1);
            }
            return a->to_slash + (slash ? 0 : 1);
        }
    }
    return path;
}

static uint32_t fnv(uint32_t h, const char* s) {
    for (; s && *s; ++s) {
        h ^= (unsigned char)tolower((unsigned char)*s);
        h *= 16777619u;
    }
    h ^= 0xFF;
    h *= 16777619u;
    return h;
}

uint32_t pc_alias_hash(uint32_t h) {
    for (size_t i = 0; i < s_count; ++i) {
        char who[24];
        snprintf(who, sizeof(who), "%d:%d", (int)s_aliases[i].owner, s_aliases[i].pack);
        h = fnv(h, who);
        h = fnv(h, s_aliases[i].from);
        h = fnv(h, s_aliases[i].to_slash);
    }
    return h;
}
