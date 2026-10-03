/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Character-pack HUD art (hud_art.c). */
#ifndef PC_MODS_HUD_ART_H
#define PC_MODS_HUD_ART_H

struct HSD_TObj;
struct HSD_ImageDesc;
struct HSD_JObj;

enum PcPackArt {
    PC_ART_STOCK,       /* per costume */
    PC_ART_EMBLEM,      /* series emblem: HUD damage mark, results */
    PC_ART_NAME_IMAGE,  /* results name label */
    PC_ART_WINNER_NAME, /* results winner banner */
};

/* Draw @p pack's art of @p kind on @p tobj (the texture the game just
 * pointed at the base character's frame), or clear the swap when @p pack is
 * -1 or ships no such art. */
void pc_hud_pack_art(struct HSD_TObj* tobj, int pack, enum PcPackArt kind, int costume);
/* The texture-animated TObj under @p jobj (first one with an image table,
 * else the first TObj), or NULL. */
struct HSD_TObj* pc_hud_find_tobj(struct HSD_JObj* jobj);

/* After the HUD points @p tobj at @p player's stock-icon frame: draw the
 * player's pack stock icon there instead, or clear any earlier swap. */
void pc_hud_stock_art(struct HSD_TObj* tobj, int player);
/* tobj.c: the image to draw instead of tobj->imagedesc, or NULL, and the
 * palette to use with it (NULL: the TObj's own). */
struct HSD_ImageDesc* pc_tobj_image_override(struct HSD_TObj* tobj);
struct _HSD_Tlut* pc_tobj_tlut_override(struct HSD_TObj* tobj);
/* Draw @p img (with @p tlut for a paletted image) instead of @p tobj's own
 * image until the next scene; NULL @p img clears it. */
void pc_tobj_override(struct HSD_TObj* tobj, struct HSD_ImageDesc* img, struct _HSD_Tlut* tlut);
/* Scene change: the HUD and its images are gone. */
void pc_hud_art_reset(void);

#endif
