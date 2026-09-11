/*
 * On-disc data model for the PC port.
 *
 * Archives (.dat/.usd) and other disc files are loaded verbatim into MEM1 and
 * stay big-endian in memory, exactly as on GameCube. Two GCC features make
 * this transparent to the game code:
 *
 *  1. Every structure that describes on-disc data is declared with
 *     `DISC_STRUCT`, i.e. __attribute__((scalar_storage_order("big-endian"))).
 *     GCC then byte-swaps every scalar member (and arrays of scalars, and
 *     bit-fields, which are laid out MSB-first like MWCC) on access. Nested
 *     struct/union members are NOT affected: they must themselves be
 *     DISC_STRUCT types (e.g. use DiscVec3 instead of Vec3, and mark nested
 *     unions/structs too).
 *
 *  2. Pointer members of on-disc structures are 32-bit on disc. They are
 *     declared as `DISC_PTR(T)` (a u32 holding a real host address) and read
 *     through `DP(T, slot)` / written through `DP_SET(slot, p)`. All memory a
 *     slot can point at lives below 4GB: MEM1 is mmap'd with MAP_32BIT, the
 *     executable is linked non-PIE, and HSD_ArchiveParse relocates file
 *     offsets to absolute host addresses.
 *
 *  3. Scalars reached through a pointer slot (arrays of floats/ints in the
 *     file) are typed with the Disc* wrappers below (`DiscF32*` etc.) and read
 *     via `.v`, so the compiler swaps them too.
 *
 * Sizes of DISC_STRUCT types equal their GameCube sizes; assert them with
 * DISC_ASSERT_SIZE. Restrictions (compile errors, by design):
 *  - Taking the address of a scalar member of a DISC_STRUCT is an error;
 *    copy to a local instead (and copy back if the callee writes it).
 *  - A slot cannot be dereferenced directly; use DP().
 *
 * On the original (MWCC/GameCube) build everything here collapses to plain
 * types, so annotated headers remain valid for the decomp.
 */
#ifndef PC_DISC_H
#define PC_DISC_H

#include <stdint.h>

#ifdef TARGET_PC

#define DISC_STRUCT __attribute__((scalar_storage_order("big-endian")))
#define DISC_PTR(T) uint32_t
#define DP(T, slot) ((T*) (uintptr_t) (uint32_t) (slot)) /* zero-extend even if the slot is signed */

void pc_disc_ptr_overflow(const void* p, const char* file, int line) __attribute__((noreturn));
#define DP_SET(slot, p)                                                        \
    do {                                                                       \
        const void* _dp_p = (const void*) (p);                                 \
        if ((uintptr_t) _dp_p >> 32)                                           \
            pc_disc_ptr_overflow(_dp_p, __FILE__, __LINE__);                   \
        (slot) = (uint32_t) (uintptr_t) _dp_p;                                 \
    } while (0)

#define DISC_ASSERT_SIZE(T, size) _Static_assert(sizeof(T) == (size), #T " disc size")

/* The game tells ARAM offsets from main-RAM pointers with `addr < 0x80000000`.
 * On PC, MEM1 is mapped at 0x80000000 and the executable is linked at
 * 0x10000000, so anything below the 16MB ARAM size is an ARAM offset. */
#define PC_IS_ARAM_ADDR(a) ((uintptr_t) (a) < 0x01000000u)

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

#else /* GameCube build: identity */

#define DISC_STRUCT
#define DISC_PTR(T) T*
#define DP(T, slot) (slot)
#define DP_SET(slot, p) ((slot) = (p))
#define DISC_ASSERT_SIZE(T, size)
#define PC_IS_ARAM_ADDR(a) ((u32) (a) < 0x80000000u)

typedef struct { float v; } DiscF32;
typedef struct { uint32_t v; } DiscU32;
typedef struct { int32_t v; } DiscS32;
typedef struct { uint16_t v; } DiscU16;
typedef struct { int16_t v; } DiscS16;
typedef struct { float x, y; } DiscVec2;
typedef struct { float x, y, z; } DiscVec3;
typedef struct { float x, y, z, w; } DiscVec4;
typedef struct { int16_t x, y, z; } DiscS16Vec3;
typedef struct { float m[3][4]; } DiscMtx;

#endif

/* Copy helpers between disc and native vectors. */
#define DISC_VEC3_GET(dst, src) ((dst).x = (src).x, (dst).y = (src).y, (dst).z = (src).z)
#define DISC_VEC3_SET(dst, src) ((dst).x = (src).x, (dst).y = (src).y, (dst).z = (src).z)

#endif
