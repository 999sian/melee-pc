/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "pc/endian.hpp"

enum class TestEnum16 : uint16_t {
    Alpha = 0x1234,
    Beta = 0x5678,
};

enum class TestEnum32 : uint32_t {
    First = 0xDEADBEEF,
    Second = 0xCAFEBABE,
};

static void test_sizes() {
    static_assert(sizeof(BE<uint8_t>) == 1);
    static_assert(sizeof(BE<int8_t>) == 1);
    static_assert(sizeof(BE<uint16_t>) == 2);
    static_assert(sizeof(BE<int16_t>) == 2);
    static_assert(sizeof(BE<uint32_t>) == 4);
    static_assert(sizeof(BE<int32_t>) == 4);
    static_assert(sizeof(BE<uint64_t>) == 8);
    static_assert(sizeof(BE<int64_t>) == 8);
    static_assert(sizeof(BE<float>) == 4);
    static_assert(sizeof(BE<double>) == 8);
    static_assert(sizeof(BE<TestEnum16>) == 2);
    static_assert(sizeof(BE<TestEnum32>) == 4);
    static_assert(sizeof(be_val<uint32_t>) == 4);
    printf("[PASS] test_sizes\n");
}

static void test_integer_conversions() {
    be_val<uint16_t> val16 = 0x1234;
    assert((uint16_t)val16 == 0x1234);
    assert(val16.host() == 0x1234);
    // Big-endian raw memory check on little-endian host
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    const uint8_t* raw16 = reinterpret_cast<const uint8_t*>(val16.raw_data());
    assert(raw16[0] == 0x12 && raw16[1] == 0x34);
#endif

    be_val<uint32_t> val32 = 0xAABBCCDD;
    assert((uint32_t)val32 == 0xAABBCCDD);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    const uint8_t* raw32 = reinterpret_cast<const uint8_t*>(val32.raw_data());
    assert(raw32[0] == 0xAA && raw32[1] == 0xBB && raw32[2] == 0xCC && raw32[3] == 0xDD);
#endif

    be_val<int32_t> val_s32 = -123456;
    assert((int32_t)val_s32 == -123456);

    be_val<uint64_t> val64 = 0x0123456789ABCDEFULL;
    assert((uint64_t)val64 == 0x0123456789ABCDEFULL);

    printf("[PASS] test_integer_conversions\n");
}

static void test_floating_point() {
    be_val<float> fval = 3.14159265f;
    float host_f = fval;
    assert(std::fabs(host_f - 3.14159265f) < 1e-6f);

    be_val<float> zero_f = 0.0f;
    assert((float)zero_f == 0.0f);

    be_val<float> neg_f = -42.5f;
    assert((float)neg_f == -42.5f);

    printf("[PASS] test_floating_point\n");
}

static void test_enums() {
    be_val<TestEnum16> e16 = TestEnum16::Beta;
    assert(e16 == TestEnum16::Beta);

    be_val<TestEnum32> e32 = TestEnum32::First;
    assert(e32 == TestEnum32::First);
    e32 = TestEnum32::Second;
    assert(e32 == TestEnum32::Second);

    printf("[PASS] test_enums\n");
}

static void test_compound_operators() {
    be_val<int32_t> x = 10;
    x += 5;
    assert(x == 15);
    x -= 3;
    assert(x == 12);
    x *= 2;
    assert(x == 24);
    x /= 4;
    assert(x == 6);
    x %= 4;
    assert(x == 2);

    x++;
    assert(x == 3);
    ++x;
    assert(x == 4);
    x--;
    assert(x == 3);
    --x;
    assert(x == 2);

    be_val<uint32_t> bits = 0x0F;
    bits |= 0xF0;
    assert(bits == 0xFF);
    bits &= 0x0F;
    assert(bits == 0x0F);
    bits ^= 0x05;
    assert(bits == 0x0A);
    bits <<= 2;
    assert(bits == 0x28);
    bits >>= 1;
    assert(bits == 0x14);

    printf("[PASS] test_compound_operators\n");
}

static void test_vectors_and_matrices() {
    BE<Vec> v(1.0f, 2.0f, 3.0f);
    Vec host_v = v;
    assert(host_v.x == 1.0f && host_v.y == 2.0f && host_v.z == 3.0f);

    BE<S16Vec> sv(10, 20, 30);
    S16Vec host_sv = sv;
    assert(host_sv.x == 10 && host_sv.y == 20 && host_sv.z == 30);
    assert(sv.x == 10 && sv.y == 20 && sv.z == 30);

    BE<Mtx> mtx;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 4; ++j) {
            mtx.contents[i][j] = (float)(i * 4 + j);
        }
    }
    Mtx host_mtx;
    mtx.to_host(host_mtx);
    assert(host_mtx[2][3] == 11.0f);

    printf("[PASS] test_vectors_and_matrices\n");
}

int main() {
    test_sizes();
    test_integer_conversions();
    test_floating_point();
    test_enums();
    test_compound_operators();
    test_vectors_and_matrices();
    printf("All endian tests passed successfully!\n");
    return 0;
}
