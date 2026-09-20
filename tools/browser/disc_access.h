/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MELEE_DISC_ACCESS_H
#define MELEE_DISC_ACCESS_H
#include <stdint.h>
#include <string.h>
static inline uint16_t __os_be_u16(const void* p) {
    const uint8_t* b = p;
    return (uint16_t)b[0] << 8 | b[1];
}
static inline uint32_t __os_be_u32(const void* p) {
    const uint8_t* b = p;
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}
static inline uint64_t __os_be_u64(const void* p) {
    return (uint64_t)__os_be_u32(p) << 32 | __os_be_u32((const uint8_t*)p + 4);
}
static inline float __os_be_f32(const void* p) {
    uint32_t u = __os_be_u32(p);
    float f;
    memcpy(&f, &u, 4);
    return f;
}
static inline double __os_be_f64(const void* p) {
    uint64_t u = __os_be_u64(p);
    double f;
    memcpy(&f, &u, 8);
    return f;
}
static inline void __os_be_put_u16(void* p, uint16_t v) {
    uint8_t* b = p;
    b[0] = v >> 8;
    b[1] = v;
}
static inline void __os_be_put_u32(void* p, uint32_t v) {
    uint8_t* b = p;
    for (int i = 3; i >= 0; i--) {
        b[i] = v;
        v >>= 8;
    }
}
static inline void __os_be_put_u64(void* p, uint64_t v) {
    uint8_t* b = p;
    for (int i = 7; i >= 0; i--) {
        b[i] = v;
        v >>= 8;
    }
}
static inline void __os_be_put_f32(void* p, float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    __os_be_put_u32(p, u);
}
static inline void __os_be_put_f64(void* p, double f) {
    uint64_t u;
    memcpy(&u, &f, 8);
    __os_be_put_u64(p, u);
}
static inline uint64_t __os_be_bits_get(const void* p, unsigned o, unsigned n, int sign) {
    const uint8_t* b = p;
    uint64_t v = 0;
    for (unsigned i = 0; i < n; i++)
        v = v << 1 | ((b[(o + i) / 8] >> (7 - (o + i) % 8)) & 1);
    if (sign && n < 64 && (v >> (n - 1)))
        v |= ~((UINT64_C(1) << n) - 1);
    return v;
}
static inline void __os_be_bits_put(void* p, unsigned o, unsigned n, uint64_t v) {
    uint8_t* b = p;
    for (unsigned i = 0; i < n; i++) {
        unsigned k = o + i, mask = 1 << (7 - k % 8);
        b[k / 8] = (b[k / 8] & ~mask) | (((v >> (n - 1 - i)) & 1) ? mask : 0);
    }
}
#endif
