/*
 * On-disc data model for the PC port.
 *
 * Archives (.dat/.usd) are loaded verbatim into MEM1 and stay big-endian in
 * memory, exactly as on GameCube. Two GCC features make this transparent:
 *
 *  1. Every structure that describes on-disc data is declared with
 *     `DISC_STRUCT`, i.e. __attribute__((scalar_storage_order("big-endian"))).
 *     GCC then byte-swaps every scalar member (and arrays of scalars, and
 *     bit-fields, laid out MSB-first like MWCC) on access. Nested struct/union
 *     members are NOT affected: they must themselves be DISC_STRUCT types
 *     (e.g. use DiscVec3 instead of Vec3).
 *
 *  2. Pointer members of on-disc structures are 32-bit on disc. They are
 *     declared as `DISC_PTR(T)` (a u32 holding a real host address) and read
 *     through `DP(T, slot)`. All memory a slot can point at lives below 4GB:
 *     MEM1 is mmap'd with MAP_32BIT, the executable is linked non-PIE, and
 *     HSD_ArchiveParse relocates offsets to absolute host addresses.
 *
 * Because sizes of DISC_STRUCT types equal their GameCube sizes, ASSERT_SIZE on
 * them is meaningful and should be kept.
 *
 * Restrictions (compile errors, by design):
 *  - Taking the address of a scalar member of a DISC_STRUCT is an error;
 *    copy to a local instead.
 *  - A slot cannot be dereferenced directly; use DP().
 */
#ifndef PC_DISC_H
#define PC_DISC_H

#include <stdint.h>

#define DISC_STRUCT __attribute__((scalar_storage_order("big-endian")))

/* A 32-bit pointer slot inside a DISC_STRUCT. */
#define DISC_PTR(T) uint32_t

/* Read a slot as a pointer. */
#define DP(T, slot) ((T*) (uintptr_t) (slot))

/* Store a pointer into a slot (aborts if the address does not fit). */
void pc_disc_ptr_overflow(const void* p, const char* file, int line) __attribute__((noreturn));
#define DP_SET(slot, p)                                                        \
    do {                                                                       \
        const void* _dp_p = (const void*) (p);                                 \
        if ((uintptr_t) _dp_p >> 32)                                           \
            pc_disc_ptr_overflow(_dp_p, __FILE__, __LINE__);                   \
        (slot) = (uint32_t) (uintptr_t) _dp_p;                                 \
    } while (0)

/* Big-endian scalar wrappers for arrays reached through a pointer slot. */
typedef struct DISC_STRUCT { float v; } DiscF32;
typedef struct DISC_STRUCT { uint32_t v; } DiscU32;
typedef struct DISC_STRUCT { int32_t v; } DiscS32;
typedef struct DISC_STRUCT { uint16_t v; } DiscU16;
typedef struct DISC_STRUCT { int16_t v; } DiscS16;

typedef struct DISC_STRUCT { float x, y; } DiscVec2;
typedef struct DISC_STRUCT { float x, y, z; } DiscVec3;
typedef struct DISC_STRUCT { float x, y, z, w; } DiscVec4;
typedef struct DISC_STRUCT { int16_t x, y, z; } DiscS16Vec3;
typedef struct DISC_STRUCT { float m[3][4]; } DiscMtx;

#endif
