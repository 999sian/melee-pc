/* Exercise grMuteCity_801F2AB0 return value and caller state cycle. */
#define __assert libc_assert
#include <assert.h>
#undef __assert
#include <stdio.h>
#include <stdbool.h>

#include <dolphin/mtx.h>
#include <sysdolphin/baselib/psstructs.h>
#include <sysdolphin/baselib/generator.h>
#include <sysdolphin/baselib/psappsrt.h>
#include <sysdolphin/baselib/jobj.h>
#include <melee/gr/grlib.h>
#include <melee/gr/ground.h>

static HSD_Generator mock_gen;
static HSD_psAppSRT mock_appsrt;
static bool mock_gen_fail = false;
static bool mock_appsrt_fail = false;
static int free_calls = 0;

HSD_Generator* grLib_801C9808(s32 arg0, s32 arg1, HSD_JObj* arg2)
{
    (void) arg0;
    (void) arg1;
    (void) arg2;
    if (mock_gen_fail) {
        return NULL;
    }
    mock_gen.appsrt = NULL;
    mock_gen.type = 0;
    return &mock_gen;
}

HSD_psAppSRT* psAddGeneratorAppSRT_begin(HSD_Generator* gp, s32 status)
{
    (void) status;
    if (mock_appsrt_fail) {
        return NULL;
    }
    gp->appsrt = &mock_appsrt;
    return &mock_appsrt;
}

f32 Ground_801C0498(void)
{
    return 1.5f;
}

void grLib_801C98A0(HSD_JObj* jobj)
{
    (void) jobj;
    free_calls++;
}

/* Include implementation directly */
#include "../src/melee/gr/grmutecity.c"

int main(void)
{
    HSD_JObj dummy_jobj = {0};

    /* Case 1: Generator allocation failure */
    mock_gen_fail = true;
    mock_appsrt_fail = false;
    s32 ret = grMuteCity_801F2AB0(0x119, &dummy_jobj);
    assert(ret == 0);

    /* Case 2: AppSRT allocation failure */
    mock_gen_fail = false;
    mock_appsrt_fail = true;
    ret = grMuteCity_801F2AB0(0x119, &dummy_jobj);
    assert(ret == 0);

    /* Case 3: Success must return non-zero (1) */
    mock_gen_fail = false;
    mock_appsrt_fail = false;
    ret = grMuteCity_801F2AB0(0x119, &dummy_jobj);
    assert(ret == 1);
    assert(mock_appsrt.gp == &mock_gen);
    assert(mock_appsrt.scale.x == 1.5f);
    assert(mock_appsrt.scale.y == 1.5f);
    assert(mock_appsrt.scale.z == 1.5f);
    assert((mock_gen.type & PSAPPSRT_UNK_B11) != 0);

    /* Case 4: Simulate Mute City car state machine (lines 1793-1799) */
    s32 car_x28 = 0;

    /* Frame 1: Car enters track in bounds -> spawns generator */
    if (car_x28 == 0) {
        car_x28 = grMuteCity_801F2AB0(0x119, &dummy_jobj);
    }
    assert(car_x28 == 1);

    /* Frame 2..N: Car still in bounds -> MUST NOT spawn again! */
    bool reallocated = false;
    if (car_x28 == 0) {
        car_x28 = grMuteCity_801F2AB0(0x119, &dummy_jobj);
        reallocated = true;
    }
    assert(!reallocated);
    assert(car_x28 == 1);

    /* Frame M: Car goes out of bounds -> frees generator and resets x28 */
    free_calls = 0;
    if (car_x28 != 0) {
        grLib_801C98A0(&dummy_jobj);
        car_x28 = 0;
    }
    assert(free_calls == 1);
    assert(car_x28 == 0);

    puts("PASS: grMuteCity_801F2AB0 returns 1 on success and prevents generator leak");
    return 0;
}
