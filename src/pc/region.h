/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Disc region support. The game is the NTSC-U 1.02 build; a PAL disc (GALP01)
 * carries the same data with two differences this module papers over:
 *
 *  - Localized files are named .ukd/.frd/.gmd/.itd/.spd instead of .usd/.dat.
 *    The DVD layer retries misses with the English (UK) extension.
 *  - TyDatai lacks the tables the USA build uses to reconcile its trophy set
 *    with the Japanese one. Empty (terminator-only) tables stand in, which
 *    makes every lookup fall through to the main table.
 */
#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Call once with the disc header's game id, before the game boots. */
void pc_region_set(const char* game_id);
bool pc_region_is_pal(void);
/* Same as pc_region_is_pal(), for per-glyph hot paths. */
extern bool pc_region_pal;

/* Stand-in data for an archive symbol the current region's disc lacks, or
 * NULL when the symbol is genuinely required. */
const void* pc_region_missing_symbol(const char* symbol_name);

/* Map a SIS string index the NTSC-U code hardcodes to the entry holding the
 * same text in the PAL archive whose SIS table symbol is `symbol`. Returns
 * idx unchanged for USA discs and for tables with no known difference. */
int pc_region_sis_index(const char* symbol, int idx);

/* Called by discfont with the sislib kerning table read from the disc's DOL
 * (2 bytes per glyph). On PAL this also rewrites the code's SJIS->glyph table
 * to the PAL atlas layout. */
void pc_region_set_sis_kerning(const unsigned char* kerning, unsigned len);

#ifdef __cplusplus
}
#endif
