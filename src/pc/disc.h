/* SPDX-License-Identifier: GPL-3.0-or-later */
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
 *     PC_MEM1_ALIAS (macOS): nothing below 4GB is mappable, so MEM1 is placed
 *     at a host address whose low 32 bits are exactly 0x80000000. Truncating a
 *     MEM1 pointer then yields its GameCube address (0x80000000 + offset), so
 *     every `slot += (u32) (uintptr_t) base` relocation stays valid, and
 *     pc_resolve_dp() maps 0x8xxxxxxx back by adding the high half of the
 *     MEM1 base. Pointers outside MEM1 (statics, aurora buffers) go through
 *     the external pointer table as before.
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

#ifdef __cplusplus
extern "C" {
#endif

uint32_t pc_register_ext_ptr(const void* p);
void* pc_resolve_ext_ptr(uint32_t id);
void pc_disc_ptr_overflow(const void* p, const char* file, int line) __attribute__((noreturn));

/* Host address of MEM1 (aurora); under PC_MEM1_ALIAS its low 32 bits are
 * 0x80000000. */
extern uintptr_t OSBaseAddress;
#define PC_MEM1_ALIAS_SIZE (96u * 1024 * 1024) /* == PC_MEM1_SIZE */

static inline int pc_is_mem1_ptr(const void* p) {
    return (uintptr_t)p - OSBaseAddress < PC_MEM1_ALIAS_SIZE;
}

/* True when truncating p to 32 bits round-trips through pc_resolve_dp:
 * below 4GB, or inside the aliased MEM1. */
static inline int pc_ptr_fits_slot(const void* p) {
#ifdef PC_MEM1_ALIAS
    if (pc_is_mem1_ptr(p))
        return 1;
#endif
    return !((uintptr_t)p >> 32);
}

static inline uint32_t pc_encode_dp(const void* p) {
    if (!p)
        return 0;
#ifdef PC_MEM1_ALIAS
    if (pc_is_mem1_ptr(p)) {
        return (uint32_t)(uintptr_t)p;
    }
#endif
    if (!((uintptr_t)p >> 32)) {
        return (uint32_t)(uintptr_t)p;
    }
    return 0x02000000u | pc_register_ext_ptr(p);
}

static inline void* pc_resolve_dp(uint32_t slot) {
#ifdef PC_MEM1_ALIAS
    if (slot & 0x80000000u) {
        return (void*)((OSBaseAddress & ~(uintptr_t)0xFFFFFFFFu) | slot);
    }
#endif
    if ((slot & 0xFF000000u) == 0x02000000u) {
        return pc_resolve_ext_ptr(slot & 0x00FFFFFFu);
    }
    return (void*)(uintptr_t)slot;
}

#ifdef __cplusplus
}
#endif

#if defined(__cplusplus) && defined(__clang__)
#include "pc/endian.hpp"
#define DISC_STRUCT
#define DISC_PTR(T) uint32_t
#define DP(T, slot) ((T*)pc_resolve_dp((uint32_t)(uintptr_t)(slot)))
#define DP_SET(slot, p)                                                                            \
    do {                                                                                           \
        (slot) = pc_encode_dp((const void*)(p));                                                   \
    } while (0)

#define DISC_ASSERT_SIZE(T, size) static_assert(sizeof(T) == (size), #T " disc size")

#define PC_IS_ARAM_ADDR(a) ((uintptr_t)(a) < 0x01000000u)

struct DiscF32 {
    BE<float> v;
    constexpr DiscF32() = default;
    constexpr DiscF32(float val) : v(val) {}
    constexpr operator float() const { return (float)v; }
};
struct DiscU32 {
    BE<uint32_t> v;
    constexpr DiscU32() = default;
    constexpr DiscU32(uint32_t val) : v(val) {}
    constexpr operator uint32_t() const { return (uint32_t)v; }
};
struct DiscS32 {
    BE<int32_t> v;
    constexpr DiscS32() = default;
    constexpr DiscS32(int32_t val) : v(val) {}
    constexpr operator int32_t() const { return (int32_t)v; }
};
struct DiscU16 {
    BE<uint16_t> v;
    constexpr DiscU16() = default;
    constexpr DiscU16(uint16_t val) : v(val) {}
    constexpr operator uint16_t() const { return (uint16_t)v; }
};
struct DiscS16 {
    BE<int16_t> v;
    constexpr DiscS16() = default;
    constexpr DiscS16(int16_t val) : v(val) {}
    constexpr operator int16_t() const { return (int16_t)v; }
};
struct DiscVec2 {
    BE<float> x, y;
};
struct DiscVec3 {
    BE<float> x, y, z;
};
struct DiscVec4 {
    BE<float> x, y, z, w;
};
struct DiscS16Vec3 {
    BE<int16_t> x, y, z;
};
struct DiscMtx {
    BE<float> m[3][4];
};

#else

#if defined(__clang__)
#define DISC_STRUCT
#else
#define DISC_STRUCT __attribute__((scalar_storage_order("big-endian")))
#endif
#define DISC_PTR(T) uint32_t
#define DP(T, slot) ((T*)pc_resolve_dp((uint32_t)(uintptr_t)(slot)))
#define DP_SET(slot, p)                                                                            \
    do {                                                                                           \
        (slot) = pc_encode_dp((const void*)(p));                                                   \
    } while (0)

#define DISC_ASSERT_SIZE(T, size) _Static_assert(sizeof(T) == (size), #T " disc size")

/* The game tells ARAM offsets from main-RAM pointers with `addr < 0x80000000`.
 * On PC, MEM1 is mapped at 0x80000000 and the executable is linked at
 * 0x10000000, so anything below the 16MB ARAM size is an ARAM offset. */
#define PC_IS_ARAM_ADDR(a) ((uintptr_t)(a) < 0x01000000u)

typedef struct DISC_STRUCT {
    float v;
} DiscF32;
typedef struct DISC_STRUCT {
    uint32_t v;
} DiscU32;
typedef struct DISC_STRUCT {
    int32_t v;
} DiscS32;
typedef struct DISC_STRUCT {
    uint16_t v;
} DiscU16;
typedef struct DISC_STRUCT {
    int16_t v;
} DiscS16;
typedef struct DISC_STRUCT {
    float x, y;
} DiscVec2;
typedef struct DISC_STRUCT {
    float x, y, z;
} DiscVec3;
typedef struct DISC_STRUCT {
    float x, y, z, w;
} DiscVec4;
typedef struct DISC_STRUCT {
    int16_t x, y, z;
} DiscS16Vec3;
typedef struct DISC_STRUCT {
    float m[3][4];
} DiscMtx;

#endif

#else /* GameCube build: identity */

#define DISC_STRUCT
#define DISC_PTR(T) T*
#define DP(T, slot) (slot)
#define DP_SET(slot, p) ((slot) = (p))
#define DISC_ASSERT_SIZE(T, size)
#define PC_IS_ARAM_ADDR(a) ((u32)(a) < 0x80000000u)

typedef struct {
    float v;
} DiscF32;
typedef struct {
    uint32_t v;
} DiscU32;
typedef struct {
    int32_t v;
} DiscS32;
typedef struct {
    uint16_t v;
} DiscU16;
typedef struct {
    int16_t v;
} DiscS16;
typedef struct {
    float x, y;
} DiscVec2;
typedef struct {
    float x, y, z;
} DiscVec3;
typedef struct {
    float x, y, z, w;
} DiscVec4;
typedef struct {
    int16_t x, y, z;
} DiscS16Vec3;
typedef struct {
    float m[3][4];
} DiscMtx;

#endif

/* Copy helpers between disc and native vectors. */
#define DISC_VEC3_GET(dst, src) ((dst).x = (src).x, (dst).y = (src).y, (dst).z = (src).z)
#define DISC_VEC3_SET(dst, src) ((dst).x = (src).x, (dst).y = (src).y, (dst).z = (src).z)

#endif
