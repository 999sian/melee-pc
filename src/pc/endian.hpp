/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <bit>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "dolphin/types.h"
#include "dolphin/mtx.h"

// Big-Endian to Host conversion
inline uint16_t be16(uint16_t val) {
    return __builtin_bswap16(val);
}
inline int16_t be16s(int16_t val) {
    return (int16_t)__builtin_bswap16((uint16_t)val);
}
inline uint32_t be32(uint32_t val) {
    return __builtin_bswap32(val);
}
inline int32_t be32s(int32_t val) {
    return (int32_t)__builtin_bswap32((uint32_t)val);
}
inline uint64_t be64(uint64_t val) {
    return __builtin_bswap64(val);
}
inline int64_t be64s(int64_t val) {
    return (int64_t)__builtin_bswap64((uint64_t)val);
}

static inline uint16_t RES_U16(uint16_t v) {
    return be16(v);
}
static inline int16_t RES_S16(int16_t v) {
    return be16s(v);
}
static inline uint32_t RES_U32(uint32_t v) {
    return be32(v);
}
static inline int32_t RES_S32(int32_t v) {
    return be32s(v);
}
static inline uint64_t RES_U64(uint64_t v) {
    return be64(v);
}
static inline int64_t RES_S64(int64_t v) {
    return be64s(v);
}
static inline float RES_F32(float v) {
    return std::bit_cast<float, int32_t>(RES_S32(std::bit_cast<int32_t, float>(v)));
}

/*
 * Declares a big-endian type with operator conversions, supporting
 * arbitrary 1, 2, 4, and 8-byte types, floats, and enums via C++20 if constexpr.
 */
template <typename T>
struct BE {
    T inner{};

    constexpr BE() = default;
    constexpr BE(const T& val) { set(val); }

    static constexpr T swap(T val) noexcept {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        return val;
#else
        if constexpr (sizeof(T) == 1) {
            return val;
        } else if constexpr (sizeof(T) == 2) {
            uint16_t u;
            std::memcpy(&u, &val, 2);
            u = __builtin_bswap16(u);
            T out;
            std::memcpy(&out, &u, 2);
            return out;
        } else if constexpr (sizeof(T) == 4) {
            uint32_t u;
            std::memcpy(&u, &val, 4);
            u = __builtin_bswap32(u);
            T out;
            std::memcpy(&out, &u, 4);
            return out;
        } else if constexpr (sizeof(T) == 8) {
            uint64_t u;
            std::memcpy(&u, &val, 8);
            u = __builtin_bswap64(u);
            T out;
            std::memcpy(&out, &u, 8);
            return out;
        } else {
            static_assert(sizeof(T) <= 8, "Unsupported type size for big-endian wrapper");
            return val;
        }
#endif
    }

    constexpr void set(T val) noexcept { inner = swap(val); }

    constexpr T host() const noexcept { return swap(inner); }

    constexpr operator T() const noexcept { return host(); }

    constexpr BE& operator=(T val) noexcept {
        set(val);
        return *this;
    }

    constexpr BE& operator++() noexcept {
        *this = static_cast<T>(host() + 1);
        return *this;
    }
    constexpr T operator++(int) noexcept {
        T orig = host();
        *this = static_cast<T>(orig + 1);
        return orig;
    }
    constexpr BE& operator--() noexcept {
        *this = static_cast<T>(host() - 1);
        return *this;
    }
    constexpr T operator--(int) noexcept {
        T orig = host();
        *this = static_cast<T>(orig - 1);
        return orig;
    }

    constexpr BE& operator+=(T val) noexcept {
        *this = static_cast<T>(host() + val);
        return *this;
    }
    constexpr BE& operator-=(T val) noexcept {
        *this = static_cast<T>(host() - val);
        return *this;
    }
    constexpr BE& operator*=(T val) noexcept {
        *this = static_cast<T>(host() * val);
        return *this;
    }
    constexpr BE& operator/=(T val) noexcept {
        *this = static_cast<T>(host() / val);
        return *this;
    }

    template <typename U = T>
    constexpr auto operator%=(U val) noexcept -> decltype(std::declval<U>() % val, *this) {
        *this = static_cast<T>(host() % val);
        return *this;
    }

    template <typename U = T>
    constexpr auto operator&=(U val) noexcept -> decltype(std::declval<U>() & val, *this) {
        *this = static_cast<T>(host() & val);
        return *this;
    }

    template <typename U = T>
    constexpr auto operator|=(U val) noexcept -> decltype(std::declval<U>() | val, *this) {
        *this = static_cast<T>(host() | val);
        return *this;
    }

    template <typename U = T>
    constexpr auto operator^=(U val) noexcept -> decltype(std::declval<U>() ^ val, *this) {
        *this = static_cast<T>(host() ^ val);
        return *this;
    }

    template <typename U = T>
    constexpr auto operator<<=(int shift) noexcept -> decltype(std::declval<U>() << shift, *this) {
        *this = static_cast<T>(host() << shift);
        return *this;
    }

    template <typename U = T>
    constexpr auto operator>>=(int shift) noexcept -> decltype(std::declval<U>() >> shift, *this) {
        *this = static_cast<T>(host() >> shift);
        return *this;
    }

    constexpr T raw() const noexcept { return inner; }
    constexpr const void* raw_data() const noexcept { return &inner; }
};

template <typename T>
using be_val = BE<T>;

template <>
struct BE<S16Vec> {
    BE<int16_t> x;
    BE<int16_t> y;
    BE<int16_t> z;

    BE() = default;
    BE(int16_t x, int16_t y, int16_t z) : x(x), y(y), z(z) {}
    BE(const S16Vec& from) : x(from.x), y(from.y), z(from.z) {}

    operator S16Vec() const { return {x, y, z}; }

    static S16Vec swap(S16Vec val) noexcept {
        return {
            BE<int16_t>::swap(val.x),
            BE<int16_t>::swap(val.y),
            BE<int16_t>::swap(val.z),
        };
    }
};

template <>
struct BE<Vec> {
    BE<float> x;
    BE<float> y;
    BE<float> z;

    BE() = default;
    BE(float x, float y, float z) : x(x), y(y), z(z) {}
    BE(const Vec& from) : x(from.x), y(from.y), z(from.z) {}

    operator Vec() const { return {x, y, z}; }

    static Vec swap(Vec val) {
        return {
            BE<float>::swap(val.x),
            BE<float>::swap(val.y),
            BE<float>::swap(val.z),
        };
    }
};

template <>
struct BE<Mtx44> {
    BE<float> contents[4][4];

    const auto& operator[](int x) const { return contents[x]; }
    auto& operator[](int x) { return contents[x]; }
};

template <>
struct BE<Mtx> {
    BE<float> contents[3][4];

    const auto& operator[](int x) const { return contents[x]; }
    auto& operator[](int x) { return contents[x]; }

    void to_host(Mtx& mtx) const {
        for (int i = 0; i < 3; i++) {
            for (int j = 0; j < 4; j++) {
                mtx[i][j] = (float)contents[i][j];
            }
        }
    }
};

typedef float Mtx23[2][3];
template <>
struct BE<Mtx23> {
    BE<float> contents[2][3];

    auto& operator[](int x) { return contents[x]; }

    const auto& operator[](int x) const { return contents[x]; }

    void to_host(Mtx23& mtx) const {
        for (int i = 0; i < 2; i++) {
            for (int j = 0; j < 3; j++) {
                mtx[i][j] = (float)contents[i][j];
            }
        }
    }
};

template <typename T>
inline void be_swap(T& val) {
    val = BE<T>::swap(val);
}

template <typename T, uint32_t N>
inline void be_swap(T (&val)[N]) {
    for (uint32_t i = 0; i < N; i++) {
        be_swap(val[i]);
    }
}

template <typename T>
inline void be_swap(T array[], const uint32_t size) {
    for (uint32_t i = 0; i < size; i++) {
        be_swap(array[i]);
    }
}

template <>
inline void be_swap(Mtx44& val) {
    for (auto& x : val) {
        for (float& y : x) {
            be_swap(y);
        }
    }
}

template <>
inline void be_swap(Mtx& val) {
    for (auto& x : val) {
        for (float& y : x) {
            be_swap(y);
        }
    }
}

#define BE_HOST(T) (T.host())
