/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <bit>
#include <cstdint>

#include "dolphin/types.h"
#include "dolphin/mtx.h"

// Big-Endian to Host conversion
inline uint16_t be16(uint16_t val) { return __builtin_bswap16(val); }
inline int16_t  be16s(int16_t val)  { return (int16_t)__builtin_bswap16((uint16_t)val); }
inline uint32_t be32(uint32_t val) { return __builtin_bswap32(val); }
inline int32_t  be32s(int32_t val)  { return (int32_t)__builtin_bswap32((uint32_t)val); }
inline uint64_t be64(uint64_t val) { return __builtin_bswap64(val); }
inline int64_t  be64s(int64_t val)  { return (int64_t)__builtin_bswap64((uint64_t)val); }

static inline uint16_t RES_U16(uint16_t v) { return be16(v); }
static inline int16_t  RES_S16(int16_t v)  { return be16s(v); }
static inline uint32_t RES_U32(uint32_t v) { return be32(v); }
static inline int32_t  RES_S32(int32_t v)  { return be32s(v); }
static inline uint64_t RES_U64(uint64_t v) { return be64(v); }
static inline int64_t  RES_S64(int64_t v)  { return be64s(v); }
static inline float    RES_F32(float v) {
    return std::bit_cast<float, int32_t>(RES_S32(std::bit_cast<int32_t, float>(v)));
}

/*
 * Declares a big-endian type with operator conversions, modeled off Dusklight.
 */
template<class T>
struct BE {
    T inner;
    BE() = default;
    BE(const T& from) {
        inner = swap(from);
    }

    T operator--(int) {
        T orig = inner;
        *this -= 1;
        return swap(orig);
    }

    T operator++(int) {
        T orig = inner;
        *this += 1;
        return swap(orig);
    }

    operator T() const {
        return swap(inner);
    }

    T host() const {
        return swap(inner);
    }

    static T swap(T val);
};

#define BE_ASSIGN_OP(op)                           \
template<typename TA, typename TB>                 \
constexpr BE<TA>& operator op(BE<TA>& a, TB b) {   \
    TA aCopy = a;                                  \
    aCopy op b;                                    \
    a = aCopy;                                     \
    return a;                                      \
}

BE_ASSIGN_OP(&=);
BE_ASSIGN_OP(|=);
BE_ASSIGN_OP(+=);
BE_ASSIGN_OP(-=);
BE_ASSIGN_OP(/=);
BE_ASSIGN_OP(^=);

#undef BE_ASSIGN_OP

template<> inline uint16_t BE<uint16_t>::swap(uint16_t val) { return RES_U16(val); }
template<> inline int16_t  BE<int16_t>::swap(int16_t val)   { return RES_S16(val); }
template<> inline uint32_t BE<uint32_t>::swap(uint32_t val) { return RES_U32(val); }
template<> inline int32_t  BE<int32_t>::swap(int32_t val)   { return RES_S32(val); }
template<> inline uint64_t BE<uint64_t>::swap(uint64_t val) { return RES_U64(val); }
template<> inline int64_t  BE<int64_t>::swap(int64_t val)   { return RES_S64(val); }
template<> inline float    BE<float>::swap(float val)       { return RES_F32(val); }

template<>
inline S16Vec BE<S16Vec>::swap(S16Vec val) {
    return {
        BE<int16_t>::swap(val.x),
        BE<int16_t>::swap(val.y),
        BE<int16_t>::swap(val.z),
    };
}

template<>
struct BE<Vec> {
    BE<float> x;
    BE<float> y;
    BE<float> z;

    BE() = default;
    BE(float x, float y, float z) : x(x), y(y), z(z) {}
    BE(const Vec& from) : x(from.x), y(from.y), z(from.z) {}

    operator Vec() const {
        return { x, y, z };
    }

    static Vec swap(Vec val) {
        return {
            BE<float>::swap(val.x),
            BE<float>::swap(val.y),
            BE<float>::swap(val.z),
        };
    }
};