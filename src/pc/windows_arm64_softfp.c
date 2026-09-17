/* SPDX-License-Identifier: GPL-3.0-or-later */
#if defined(_WIN32) && defined(__aarch64__)
#include <arm_neon.h>
#include <stdint.h>
#include <string.h>

/* In AArch64 AAPCS, a 128-bit FP or vector type is passed in SIMD register q0/q1.
 * GCC emitting softfp for quad-precision float (__float128) expects:
 *   q0 __extendsftf2(float s0)
 *   int __gttf2(q0 a, q1 b)
 *
 * IEEE 754 binary128 layout:
 *   bit 127: sign
 *   bits 126..112: exponent (15 bits, bias 16383)
 *   bits 111..0: fraction (112 bits)
 *
 * In little-endian vector (uint64x2_t):
 *   lane 0 (d0 / low):  bits 63..0
 *   lane 1 (d1 / high): bits 127..64
 */

typedef union {
    uint64x2_t vec;
    struct {
        uint64_t low;
        uint64_t high;
    } parts;
} Quad;

uint64x2_t __extendsftf2(float f) {
    uint32_t f_bits;
    memcpy(&f_bits, &f, sizeof(f_bits));

    uint64_t sign = (f_bits >> 31) & 1ULL;
    uint64_t exp = (f_bits >> 23) & 0xffULL;
    uint64_t mant = f_bits & 0x7fffffULL;

    Quad q;
    if (exp == 0 && mant == 0) {
        q.parts.high = sign << 63;
        q.parts.low = 0;
    } else if (exp == 0xff) {
        /* Inf or NaN */
        q.parts.high = (sign << 63) | (0x7fffULL << 48) | (mant << 25);
        q.parts.low = 0;
    } else {
        uint64_t exp_quad = exp - 127 + 16383;
        q.parts.high = (sign << 63) | (exp_quad << 48) | (mant << 25);
        q.parts.low = 0;
    }
    return q.vec;
}

static int compare_quad(uint64x2_t a_vec, uint64x2_t b_vec) {
    Quad a, b;
    a.vec = a_vec;
    b.vec = b_vec;

    uint64_t a_sign = (a.parts.high >> 63) & 1ULL;
    uint64_t b_sign = (b.parts.high >> 63) & 1ULL;

    /* Check if both are +/- 0.0 */
    int a_is_zero = ((a.parts.high & 0x7fffffffffffffffULL) == 0) && (a.parts.low == 0);
    int b_is_zero = ((b.parts.high & 0x7fffffffffffffffULL) == 0) && (b.parts.low == 0);
    if (a_is_zero && b_is_zero) {
        return 0;
    }

    if (a_sign != b_sign) {
        return a_sign ? -1 : 1;
    }

    /* Same sign */
    int cmp;
    if (a.parts.high > b.parts.high) {
        cmp = 1;
    } else if (a.parts.high < b.parts.high) {
        cmp = -1;
    } else {
        if (a.parts.low > b.parts.low) {
            cmp = 1;
        } else if (a.parts.low < b.parts.low) {
            cmp = -1;
        } else {
            cmp = 0;
        }
    }

    /* If negative, reverse comparison */
    return a_sign ? -cmp : cmp;
}

int __gttf2(uint64x2_t a, uint64x2_t b) {
    return compare_quad(a, b);
}

int __lttf2(uint64x2_t a, uint64x2_t b) {
    return compare_quad(a, b);
}

int __letf2(uint64x2_t a, uint64x2_t b) {
    return compare_quad(a, b);
}

int __getf2(uint64x2_t a, uint64x2_t b) {
    return compare_quad(a, b);
}

int __eqtf2(uint64x2_t a, uint64x2_t b) {
    return compare_quad(a, b) == 0 ? 0 : 1;
}

int __netf2(uint64x2_t a, uint64x2_t b) {
    return compare_quad(a, b) != 0 ? 1 : 0;
}
#endif
