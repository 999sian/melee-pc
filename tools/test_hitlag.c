#define __assert libc_assert
#include <assert.h>
#undef __assert
#include <stdio.h>
#include <string.h>
#include "../src/melee/ft/ftcommon.c"

ftCommonData* p_ftCommonData;
static uint8_t mock_plco[0x500];

int main(void) {
    memset(mock_plco, 0, sizeof(mock_plco));

    // Fill ftCommonData hitlag parameters (big-endian on disc):
    // +0x194: max hitlag frames = 20.0f (0x41a00000)
    // +0x198: hitlag scale = 0.33333334f (0x3eaaaaab)
    // +0x19C: hitlag base = 3.0f (0x40400000)
    // +0x1A0: crouch hitlag multiplier = 0.666667f (0x3f2aaaab)
    uint32_t val_194 = 0x41a00000;
    uint32_t val_198 = 0x3eaaaaab;
    uint32_t val_19c = 0x40400000;
    uint32_t val_1a0 = 0x3f2aaaab;

    #if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    val_194 = __builtin_bswap32(val_194);
    val_198 = __builtin_bswap32(val_198);
    val_19c = __builtin_bswap32(val_19c);
    val_1a0 = __builtin_bswap32(val_1a0);
    #endif

    memcpy(&mock_plco[0x194], &val_194, 4);
    memcpy(&mock_plco[0x198], &val_198, 4);
    memcpy(&mock_plco[0x19c], &val_19c, 4);
    memcpy(&mock_plco[0x1a0], &val_1a0, 4);

    p_ftCommonData = (struct ftCommonData*) mock_plco;

    // Vanilla formula: floor(dmg * 0.33333334 + 3.0) * mul
    // 1% damage: floor(1 * 0.33333334 + 3.0) = 3
    float hl1 = ftCommon_CalcHitlag(1, ftCo_MS_Wait, 1.0f);
    assert(hl1 == 3.0f);

    // 3% damage (jab): floor(3 * 0.33333334 + 3.0) = 4
    float hl3 = ftCommon_CalcHitlag(3, ftCo_MS_Wait, 1.0f);
    assert(hl3 == 4.0f);

    // 9% damage: floor(9 * 0.33333334 + 3.0) = 6
    float hl9 = ftCommon_CalcHitlag(9, ftCo_MS_Wait, 1.0f);
    assert(hl9 == 6.0f);

    // 18% damage (strong hit): floor(18 * 0.33333334 + 3.0) = 9
    float hl18 = ftCommon_CalcHitlag(18, ftCo_MS_Wait, 1.0f);
    assert(hl18 == 9.0f);

    // 24% damage (smash attack): floor(24 * 0.33333334 + 3.0) = 11
    float hl24 = ftCommon_CalcHitlag(24, ftCo_MS_Wait, 1.0f);
    assert(hl24 == 11.0f);

    // Crouch cancel: 18% move while crouching
    // (int)(9 * 0.666667) = 6
    float hl18_crouch = ftCommon_CalcHitlag(18, ftCo_MS_Squat, 1.0f);
    assert(hl18_crouch == 6.0f);

    // Verify damage scaling: 18% hitlag MUST be greater than 1% hitlag
    assert(hl18 > hl1);

    puts("PASS: ftCommon_CalcHitlag scales accurately with damage and respects crouch modifier");
    return 0;
}
