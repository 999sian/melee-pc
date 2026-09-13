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

static bool s_key[SDL_SCANCODE_COUNT];
static bool s_active;
static bool s_suppressed[SDL_SCANCODE_COUNT];
extern bool pc_menu_is_open(void);

static const struct {
    SDL_Scancode key;
    u16 button;
} s_button_map[] = {
    { SDL_SCANCODE_X, PAD_BUTTON_A },     { SDL_SCANCODE_Z, PAD_BUTTON_B },
    { SDL_SCANCODE_C, PAD_BUTTON_X },     { SDL_SCANCODE_V, PAD_BUTTON_Y },
    { SDL_SCANCODE_Q, PAD_TRIGGER_L },    { SDL_SCANCODE_E, PAD_TRIGGER_R },
    { SDL_SCANCODE_TAB, PAD_TRIGGER_Z },  { SDL_SCANCODE_RETURN, PAD_BUTTON_START },
    { SDL_SCANCODE_T, PAD_BUTTON_UP },    { SDL_SCANCODE_G, PAD_BUTTON_DOWN },
    { SDL_SCANCODE_F, PAD_BUTTON_LEFT },  { SDL_SCANCODE_H, PAD_BUTTON_RIGHT },
};

static s8 axis(SDL_Scancode neg, SDL_Scancode pos)
{
    /* A real GameCube stick reads about +-80 at full deflection. */
    return (s8) ((s_key[pos] ? 80 : 0) - (s_key[neg] ? 80 : 0));
}

void pc_keyboard_event(const SDL_Event* e)
{
    if (e->type != SDL_EVENT_KEY_DOWN && e->type != SDL_EVENT_KEY_UP) {
        return;
    }
    if (e->key.scancode >= SDL_SCANCODE_COUNT || e->key.repeat) {
        return;
    }
    s_key[e->key.scancode] = e->type == SDL_EVENT_KEY_DOWN;
    s_active = true;
}

void pc_keyboard_apply(void)
{
    PADStatus st = { 0 };
    size_t i;
    if (!s_active) {
        return;
    }
    /* Releases can be lost (focus changes, synthetic X events); SDL's own key
     * state is authoritative, so resync from it every frame. */
    {
        int n = 0;
        const bool* keys = SDL_GetKeyboardState(&n);
        if (n > SDL_SCANCODE_COUNT) {
            n = SDL_SCANCODE_COUNT;
        }
        memcpy(s_key, keys, (size_t) n);
    }
    for (i = 0; i < SDL_SCANCODE_COUNT; i++) {
        if (pc_menu_is_open()) s_suppressed[i] = s_key[i];
        else if (!s_key[i]) s_suppressed[i] = false;
        if (s_suppressed[i]) s_key[i] = false;
    }
    for (i = 0; i < sizeof(s_button_map) / sizeof(s_button_map[0]); i++) {
        if (s_key[s_button_map[i].key]) {
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
    st.triggerLeft = s_key[SDL_SCANCODE_Q] ? 255 : 0;
    st.triggerRight = s_key[SDL_SCANCODE_E] ? 255 : 0;
    PADSetVirtualStatus(0, &st);
}
