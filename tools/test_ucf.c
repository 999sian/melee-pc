#define __assert libc_assert
#include <assert.h>
#undef __assert
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/melee/ft/kinds/ftCommon/ftCo_Turn.c"
#include "../src/melee/ft/kinds/ftCommon/ftCo_Escape.c"

// Feeds scripted stick sequences through the UCF decision functions and checks
// that vanilla and UCF disagree exactly on the 1-frame dashback miss / the
// Axe-method shield drop, and agree everywhere else.

ftCommonData* p_ftCommonData;
static uint8_t mock_plco[0x400];
static bool ucf_on;
bool pc_is_ucf_enabled(void) {
    return ucf_on;
}

static void put_u32(int off, uint32_t v) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    v = __builtin_bswap32(v);
#endif
    memcpy(&mock_plco[off], &v, 4);
}

static void put_f32(int off, float f) {
    uint32_t v;
    memcpy(&v, &f, 4);
    put_u32(off, v);
}

#define SMASH_DEADZONE 0.2875f

/// One frame of the port-0 input path: what fighter.c does with the pad.
static void feed(Fighter* fp, int raw_x) {
    s8* h = ftCo_ucf_raw_x[0];
    float prev = fp->input.lstick[0].x;
    /* The game stick is octagon-clamped at the 80-unit rim; the UCF history
     * keeps the raw value (pc_pad_game_raw_x, fighter.c). */
    int clamped = raw_x > 80 ? 80 : raw_x < -80 ? -80 : raw_x;
    float x = clamped / 80.0f;
    h[2] = h[1];
    h[1] = h[0];
    h[0] = (s8)raw_x;
    fp->input.lstick[1].x = prev;
    fp->input.lstick[0].x = x;
    if (x >= SMASH_DEADZONE) {
        fp->active_timer.lstick.x = prev >= SMASH_DEADZONE ? fp->active_timer.lstick.x + 1 : 0;
    } else if (x <= -SMASH_DEADZONE) {
        fp->active_timer.lstick.x = prev <= -SMASH_DEADZONE ? fp->active_timer.lstick.x + 1 : 0;
    } else {
        fp->active_timer.lstick.x = 254;
    }
}

enum { NONE, DASHBACK };

/// Runs a raw stick-X script from Wait facing right and reports whether a
/// dashback came out. Vanilla: Wait's smash-turn check (ftCo_Dash.c). UCF adds
/// tilt-turn frame 2 cancelling into it (ftCo_Turn_IASA).
static int run_dashback(const int* script, int n, bool ucf, bool nana) {
    Fighter* fp = calloc(1, sizeof(Fighter));
    bool in_turn = false;
    int i, result = NONE;
    ucf_on = ucf;
    fp->facing_dir = 1.0f;
    fp->is_sub_fighter = nana;
    memset(ftCo_ucf_raw_x, 0, sizeof(ftCo_ucf_raw_x));
    for (i = 0; i < n && result == NONE; i++) {
        float x;
        feed(fp, script[i]);
        x = fp->input.lstick[0].x;
        if (!in_turn) {
            if (ABS(x) >= p_ftCommonData->dash_smash_stick_threshold &&
                fp->active_timer.lstick.x < p_ftCommonData->dash_smash_window &&
                x * fp->facing_dir < 0)
            {
                result = DASHBACK;
            } else if (x * fp->facing_dir <= p_ftCommonData->x34) {
                in_turn = true;
                fp->cur_anim_frame = 1.0f;
                fp->mv.co.turn.facing_after = -fp->facing_dir;
            }
        } else {
            fp->cur_anim_frame += 1.0f;
            if (ucf && ftCo_UcfDashback(fp)) {
                result = DASHBACK;
            }
        }
    }
    free(fp);
    return result;
}

#define SCRIPT(...) ((const int[]){__VA_ARGS__}), (int)(sizeof((int[]){__VA_ARGS__}) / sizeof(int))

static void check_dashback(const int* script, int n, int vanilla, int ucf) {
    assert(run_dashback(script, n, false, false) == vanilla);
    assert(run_dashback(script, n, true, false) == ucf);
}

static bool shield_drop_blocked(
    float x, float y, float cstick_y, int timer_x, int floor_index, u32 floor_flags) {
    Fighter* fp = calloc(1, sizeof(Fighter));
    bool r;
    fp->input.lstick[0].x = x;
    fp->input.lstick[0].y = y;
    fp->input.cstick[0].y = cstick_y;
    fp->active_timer.lstick.x = timer_x;
    fp->coll_data.floor.index = floor_index;
    fp->coll_data.floor.flags = floor_flags;
    r = ftCo_UcfBlocksSpotDodge(fp);
    free(fp);
    return r;
}

int main(void) {
    memset(mock_plco, 0, sizeof(mock_plco));
    put_f32(0x34, -SMASH_DEADZONE);  // tilt turn threshold (x34)
    put_f32(0x3C, 0.8f);             // dash_smash_stick_threshold
    put_u32(0x40, 2);                // dash_smash_window
    put_f32(0x314, -0.7f);           // spot dodge stick threshold
    put_u32(0x318, 2);               // spot dodge window
    put_u32(0x320, 2);               // roll window (x320)
    p_ftCommonData = (ftCommonData*)mock_plco;

    // The known miss: one intermediate poll (-50 = -0.625) enters a tilt turn,
    // the rim arrives a frame late. Vanilla stays in the tilt turn; UCF sees
    // 0 -> -80 over two frames (delta 80 > 75) and dashes back.
    check_dashback(SCRIPT(0, 0, -50, -80), NONE, DASHBACK);
    // A flick that ends past the rim: -5 -> -85 is 80 raw units (a fast
    // flick) but only 75 once clamped, so UCF fed the clamped stick missed
    // it. The raw history catches it.
    check_dashback(SCRIPT(0, -5, -50, -85), NONE, DASHBACK);
    // Nana never takes the UCF path.
    assert(run_dashback(SCRIPT(0, 0, -50, -80), true, true) == NONE);
    // Clean one-frame flick: both dash back on the crossing frame.
    check_dashback(SCRIPT(0, 0, -80), DASHBACK, DASHBACK);
    // Slow tilt: never reaches 0.8 on turn frame 2.
    check_dashback(SCRIPT(0, -30, -50, -80), NONE, NONE);
    // Fast finish from a pre-lean: -20 -> -80 is only 60 units, tilt intent.
    check_dashback(SCRIPT(-20, -20, -40, -80), NONE, NONE);
    // Exactly 75 units is not enough (strict >).
    check_dashback(SCRIPT(-5, -5, -40, -80), NONE, NONE);
    check_dashback(SCRIPT(-4, -4, -40, -80), NONE, DASHBACK);
    // Rim reached on turn frame 3: too late for UCF too.
    check_dashback(SCRIPT(0, -50, -60, -80), NONE, NONE);

    // Shield drop: Axe method on a platform, stick rolled down the rim to
    // (0.7, -0.7125): vanilla spot dodges (y <= -0.7), UCF blocks it.
    assert(shield_drop_blocked(0.7f, -0.7125f, 0.0f, 5, 0, LINE_FLAG_PLATFORM));
    // Past -0.8: spot dodge as in vanilla.
    assert(!shield_drop_blocked(0.5f, -0.85f, 0.0f, 5, 0, LINE_FLAG_PLATFORM));
    // On solid ground, or airborne (no floor).
    assert(!shield_drop_blocked(0.7f, -0.7125f, 0.0f, 5, 0, 0));
    assert(!shield_drop_blocked(0.7f, -0.7125f, 0.0f, 5, -1, LINE_FLAG_PLATFORM));
    // Roll still possible (stick held sideways < roll window).
    assert(!shield_drop_blocked(0.7f, -0.7125f, 0.0f, 1, 0, LINE_FLAG_PLATFORM));
    // Inside the rim: (0.5, -0.7125) -> 41^2 + 58^2 = 5045 < 6400.
    assert(!shield_drop_blocked(0.5f, -0.7125f, 0.0f, 5, 0, LINE_FLAG_PLATFORM));
    // C-stick spot dodge always wins.
    assert(!shield_drop_blocked(0.7f, -0.7125f, -0.9f, 5, 0, LINE_FLAG_PLATFORM));

    puts("PASS: UCF dashback and shield drop match UCF 0.8x rules; vanilla untouched");
    return 0;
}
