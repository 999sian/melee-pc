/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Character-pack HUD art: per-player stock icons drawn from a pack's PNGs.
 *
 * The HUD picks a stock icon by animating a texture to the base character's
 * frame, and several players can share a base, so the swap cannot be done by
 * image. Instead each player's own TObj is mapped to a replacement image,
 * and the texture loader (tobj.c) draws that image instead. Rendering only:
 * nothing here is simulation state, so rollback never sees it. */
#include "pc/compat.h"

#include "hud_art.h"
#include "roster.h"

#include <string.h>

#include <dolphin/gx.h>
#include <melee/pl/player.h>
#include <sysdolphin/baselib/dobj.h>
#include <sysdolphin/baselib/jobj.h>
#include <sysdolphin/baselib/memory.h>
#include <sysdolphin/baselib/mobj.h>
#include <sysdolphin/baselib/tobj.h>

#define MAX_OVERRIDES 64
#define MAX_IMAGES 64

static struct {
    HSD_TObj* tobj;
    HSD_ImageDesc* img;
    struct _HSD_Tlut* tlut;
} s_overrides[MAX_OVERRIDES];
static int s_override_count;

static struct {
    int pack, kind, costume, w, h;
    HSD_ImageDesc* desc; /* NULL: tried, pack has no such art */
} s_images[MAX_IMAGES];
static int s_image_count;

void pc_hud_art_reset(void) {
    /* The HUD's TObjs and the scene heap the images live in both go away
     * with the scene. */
    s_override_count = 0;
    s_image_count = 0;
}

HSD_ImageDesc* pc_tobj_image_override(HSD_TObj* tobj) {
    for (int i = 0; i < s_override_count; ++i)
        if (s_overrides[i].tobj == tobj)
            return s_overrides[i].img;
    return 0;
}

struct _HSD_Tlut* pc_tobj_tlut_override(HSD_TObj* tobj) {
    for (int i = 0; i < s_override_count; ++i)
        if (s_overrides[i].tobj == tobj)
            return s_overrides[i].tlut;
    return NULL;
}

void pc_tobj_override(HSD_TObj* tobj, HSD_ImageDesc* img, struct _HSD_Tlut* tlut) {
    for (int i = 0; i < s_override_count; ++i)
        if (s_overrides[i].tobj == tobj) {
            if (img != NULL) {
                s_overrides[i].img = img;
                s_overrides[i].tlut = tlut;
            } else {
                s_overrides[i] = s_overrides[--s_override_count];
            }
            return;
        }
    if (img != NULL && s_override_count < MAX_OVERRIDES) {
        s_overrides[s_override_count].tobj = tobj;
        s_overrides[s_override_count].img = img;
        s_overrides[s_override_count].tlut = tlut;
        ++s_override_count;
    }
}

static void set_override(HSD_TObj* tobj, HSD_ImageDesc* img) {
    pc_tobj_override(tobj, img, NULL);
}

static const void* art_gx(int pack, int kind, int costume, int w, int h) {
    switch (kind) {
    case PC_ART_STOCK:
        return pc_roster_stock_gx(pack, costume, w, h);
    default:
        return NULL;
    }
}

static HSD_ImageDesc* pack_image(int pack, int kind, int costume, int w, int h) {
    for (int i = 0; i < s_image_count; ++i)
        if (s_images[i].pack == pack && s_images[i].kind == kind &&
            s_images[i].costume == costume && s_images[i].w == w && s_images[i].h == h)
            return s_images[i].desc;
    /* 0, not NULL: void* would toggle scalar storage order (disc.h). */
    HSD_ImageDesc* desc = 0;
    const void* gx = art_gx(pack, kind, costume, w, h);
    if (gx != NULL) {
        const size_t size = (size_t)w * h * 4;
        void* buf = HSD_MemAlloc(size);
        desc = (HSD_ImageDesc*)(uintptr_t)HSD_MemAlloc(sizeof(HSD_ImageDesc));
        if (buf != NULL && desc != NULL) {
            memcpy(buf, gx, size);
            memset(desc, 0, sizeof(*desc));
            DP_SET(desc->image_ptr, buf);
            desc->width = (u16)w;
            desc->height = (u16)h;
            desc->format = GX_TF_RGBA8;
        } else {
            desc = 0;
        }
    }
    if (s_image_count < MAX_IMAGES) {
        s_images[s_image_count].pack = pack;
        s_images[s_image_count].kind = kind;
        s_images[s_image_count].costume = costume;
        s_images[s_image_count].w = w;
        s_images[s_image_count].h = h;
        s_images[s_image_count].desc = desc;
        ++s_image_count;
    }
    return desc;
}

void pc_hud_pack_art(HSD_TObj* tobj, int pack, enum PcPackArt kind, int costume) {
    if (tobj == NULL)
        return;
    HSD_ImageDesc* img = 0;
    if (pack >= 0 && tobj->imagedesc != NULL)
        img = pack_image(pack, (int)kind, costume, tobj->imagedesc->width, tobj->imagedesc->height);
    set_override(tobj, img);
}

void pc_hud_stock_art(HSD_TObj* tobj, int player) {
    if (tobj == NULL || player < 0 || player >= 6)
        return;
    pc_hud_pack_art(
        tobj, (int)Player_GetPack(player) - 1, PC_ART_STOCK, (int)Player_GetCostumeId(player));
}

HSD_TObj* pc_hud_find_tobj(HSD_JObj* jobj) {
    HSD_TObj* first = NULL;
    for (; jobj != NULL; jobj = jobj->child) {
        if (!(jobj->flags & (JOBJ_PTCL | JOBJ_SPLINE))) {
            for (HSD_DObj* d = jobj->u.dobj; d != NULL; d = d->next) {
                for (HSD_TObj* t = d->mobj ? d->mobj->tobj : NULL; t != NULL; t = t->next) {
                    if (t->imagetbl != NULL)
                        return t;
                    if (first == NULL)
                        first = t;
                }
            }
        }
        if (first != NULL)
            break;
    }
    return first;
}
