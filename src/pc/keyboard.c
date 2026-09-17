/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Keyboard -> controller port 1.
 *
 * Gamepads are handled by aurora's SDL_Gamepad mapping; this covers the
 * no-controller case. Mapping:
 *   Arrows / WASD  main stick      IJKL     C stick
 *   X = A          Z = B           C = X    V = Y
 *   Q = L          E = R           Tab = Z  Enter = Start
 *   D-pad: T/G/F/H
 */
#include <aurora/event.h>
#include <string.h>
#include <dolphin/pad.h>

#include "pc/pc.h"
#include "pc/touch.h"

static bool s_key[SDL_SCANCODE_COUNT];
static bool s_key_latched[SDL_SCANCODE_COUNT];
static bool s_active;
static bool s_suppressed[SDL_SCANCODE_COUNT];
extern bool pc_menu_is_open(void);

static const struct {
    SDL_Scancode key;
    u16 button;
} s_button_map[] = {
    {SDL_SCANCODE_X, PAD_BUTTON_A},
    {SDL_SCANCODE_Z, PAD_BUTTON_B},
    {SDL_SCANCODE_C, PAD_BUTTON_X},
    {SDL_SCANCODE_V, PAD_BUTTON_Y},
    {SDL_SCANCODE_Q, PAD_TRIGGER_L},
    {SDL_SCANCODE_E, PAD_TRIGGER_R},
    {SDL_SCANCODE_TAB, PAD_TRIGGER_Z},
    {SDL_SCANCODE_RETURN, PAD_BUTTON_START},
    {SDL_SCANCODE_T, PAD_BUTTON_UP},
    {SDL_SCANCODE_G, PAD_BUTTON_DOWN},
    {SDL_SCANCODE_F, PAD_BUTTON_LEFT},
    {SDL_SCANCODE_H, PAD_BUTTON_RIGHT},
};

static s8 axis(SDL_Scancode neg, SDL_Scancode pos) {
    /* A real GameCube stick reads about +-80 at full deflection. */
    const bool neg_on = s_key[neg] || s_key_latched[neg];
    const bool pos_on = s_key[pos] || s_key_latched[pos];
    return (s8)((pos_on ? 80 : 0) - (neg_on ? 80 : 0));
}

static SDL_Mutex* s_key_mutex;

void pc_keyboard_event(const SDL_Event* e) {
    if (e->type != SDL_EVENT_KEY_DOWN && e->type != SDL_EVENT_KEY_UP) {
        return;
    }
    if (e->key.scancode >= SDL_SCANCODE_COUNT || e->key.repeat) {
        return;
    }
    if (s_key_mutex == NULL) {
        s_key_mutex = SDL_CreateMutex();
    }
    if (s_key_mutex != NULL) {
        SDL_LockMutex(s_key_mutex);
    }
    if (e->type == SDL_EVENT_KEY_DOWN) {
        s_key[e->key.scancode] = true;
        s_key_latched[e->key.scancode] = true;
    } else {
        s_key[e->key.scancode] = false;
    }
    s_active = true;
    if (s_key_mutex != NULL) {
        SDL_UnlockMutex(s_key_mutex);
    }
}

void pc_keyboard_apply(void) {
    if (s_key_mutex == NULL) {
        s_key_mutex = SDL_CreateMutex();
    }
    if (s_key_mutex != NULL) {
        SDL_LockMutex(s_key_mutex);
    }
    PADStatus st = {0};
    bool any_active = false;
    if (s_active) {
        size_t i;
        int n = 0;
        const bool* keys = SDL_GetKeyboardState(&n);
        if (n > SDL_SCANCODE_COUNT) {
            n = SDL_SCANCODE_COUNT;
        }
        memcpy(s_key, keys, (size_t)n);
        for (i = 0; i < SDL_SCANCODE_COUNT; i++) {
            if (pc_menu_is_open())
                s_suppressed[i] = s_key[i];
            else if (!s_key[i])
                s_suppressed[i] = false;
            if (s_suppressed[i])
                s_key[i] = false;
        }
        for (i = 0; i < sizeof(s_button_map) / sizeof(s_button_map[0]); i++) {
            SDL_Scancode key = s_button_map[i].key;
            if (s_key[key] || s_key_latched[key]) {
                st.button |= s_button_map[i].button;
            }
        }
        st.stickX = axis(SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT);
        if (st.stickX == 0) {
            st.stickX = axis(SDL_SCANCODE_A, SDL_SCANCODE_D);
        }
        st.stickY = axis(SDL_SCANCODE_DOWN, SDL_SCANCODE_UP);
        if (st.stickY == 0) {
            st.stickY = axis(SDL_SCANCODE_S, SDL_SCANCODE_W);
        }
        st.substickX = axis(SDL_SCANCODE_J, SDL_SCANCODE_L);
        st.substickY = axis(SDL_SCANCODE_K, SDL_SCANCODE_I);
        st.triggerLeft = (s_key[SDL_SCANCODE_Q] || s_key_latched[SDL_SCANCODE_Q]) ? 255 : 0;
        st.triggerRight = (s_key[SDL_SCANCODE_E] || s_key_latched[SDL_SCANCODE_E]) ? 255 : 0;
        memset(s_key_latched, 0, sizeof(s_key_latched));
        any_active = true;
    }

    PADStatus touch_st = {0};
    if (pc_touch_get_status(&touch_st)) {
        st.button |= touch_st.button;
        if (touch_st.stickX != 0 || touch_st.stickY != 0) {
            st.stickX = touch_st.stickX;
            st.stickY = touch_st.stickY;
        }
        if (touch_st.substickX != 0 || touch_st.substickY != 0) {
            st.substickX = touch_st.substickX;
            st.substickY = touch_st.substickY;
        }
        if (touch_st.triggerLeft > st.triggerLeft) {
            st.triggerLeft = touch_st.triggerLeft;
        }
        if (touch_st.triggerRight > st.triggerRight) {
            st.triggerRight = touch_st.triggerRight;
        }
        any_active = true;
    }

    if (any_active) {
        PADSetVirtualStatus(0, &st);
    }
    if (s_key_mutex != NULL) {
        SDL_UnlockMutex(s_key_mutex);
    }
}
