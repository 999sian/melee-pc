#ifndef MELEE_MN_MNONLINE_H
#define MELEE_MN_MNONLINE_H

#include "forward.h"

#include <sysdolphin/baselib/forward.h>

/* PC only: the VS Mode > Online submenu (MENU_KIND_ONLINE). SdMenu has no
 * label textures or SIS strings for it, so mnmain.c hides the matanim label
 * of every slot that has an mnOnline_Label and draws that text instead. */

/* 22D594-style think proc, see mn_803EB6B0[MENU_KIND_ONLINE]. */
void mnOnline_Think(HSD_GObj*);

/* Literal label / bottom-bar text for PC-only entries; NULL = stock. */
const char* mnOnline_Label(MenuKind, int selection);
const char* mnOnline_Description(MenuKind, int selection);

/* One-shot description override set by the think proc (A on a stub). */
const char* mnOnline_TakeNotice(void);

#endif
