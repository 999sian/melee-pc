#ifndef _DOLPHIN_ARQ_H_
#define _DOLPHIN_ARQ_H_

// melee-pc: upstream moved the ARQ API into this header with GameCube-width
// (u32) ARQRequest fields. melee-pc keeps its LP64 ARQ API (uintptr_t
// owner/source/dest, ARQCallback taking ARQRequest*) in <dolphin/ar.h>
// because ARAM emulation passes host pointers through the request. This
// shim keeps upstream's include path valid without duplicating the types.
#include <dolphin/ar.h>

#endif // _DOLPHIN_ARQ_H_
