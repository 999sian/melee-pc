#ifndef SYSDOLPHIN_BASELIB_PSDISPTEV_H
#define SYSDOLPHIN_BASELIB_PSDISPTEV_H

#include <Runtime/platform.h>

#include <sysdolphin/baselib/forward.h>

void psSetupTevCommon(void);
void psSetupTevInvalidState(void);
/* Takes the particle itself. It used to take (u32*) pp and read arg0[1] to
 * mean pp->kind, which only holds where pointers are 4 bytes wide: with
 * 64-bit host pointers, kind moves from 0x04 to 0x08 and arg0[1] reads the
 * upper half of pp->next instead. */
void psSetupTev(HSD_Particle* pp);

#endif
