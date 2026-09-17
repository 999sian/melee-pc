/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pc/touch.h"
#include "pc/pc.h"
#include <dolphin/pad.h>
#include <string.h>

#if defined(__ANDROID__) || defined(__APPLE__) || defined(TARGET_OS_IPHONE)
#include <pthread.h>
#include <SDL3/SDL_events.h>

#if defined(__ANDROID__)
#include <jni.h>
#endif

static pthread_mutex_t s_touch_lock = PTHREAD_MUTEX_INITIALIZER;
static PADStatus s_touch_status;
static uint16_t s_latched_buttons;
static bool s_touch_active;

void pc_touch_set_pad(uint16_t buttons, int8_t stickX, int8_t stickY, int8_t cstickX,
    int8_t cstickY, uint8_t triggerL, uint8_t triggerR) {
    pthread_mutex_lock(&s_touch_lock);
    s_touch_status.button = buttons;
    s_latched_buttons |= buttons;
    s_touch_status.stickX = stickX;
    s_touch_status.stickY = stickY;
    s_touch_status.substickX = cstickX;
    s_touch_status.substickY = cstickY;
    s_touch_status.triggerLeft = triggerL;
    s_touch_status.triggerRight = triggerR;
    s_touch_active = true;
    pthread_mutex_unlock(&s_touch_lock);
}

void pc_touch_set_active(bool active) {
    pthread_mutex_lock(&s_touch_lock);
    s_touch_active = active;
    if (!active) {
        memset(&s_touch_status, 0, sizeof(s_touch_status));
        s_latched_buttons = 0;
        PADClearVirtualStatus(0);
    }
    pthread_mutex_unlock(&s_touch_lock);
}

bool pc_touch_get_status(PADStatus* out) {
    pthread_mutex_lock(&s_touch_lock);
    if (!s_touch_active) {
        pthread_mutex_unlock(&s_touch_lock);
        return false;
    }
    *out = s_touch_status;
    out->button |= s_latched_buttons;
    s_latched_buttons = 0;
    pthread_mutex_unlock(&s_touch_lock);
    return true;
}

void pc_touch_apply(void) {
    PADStatus st = {0};
    if (pc_touch_get_status(&st)) {
        PADSetVirtualStatus(0, &st);
    }
}

static struct {
    SDL_FingerID stick_finger;
    float stick_origin_x;
    float stick_origin_y;
    bool has_stick;
} s_touch_tracker = {-1, 0.0f, 0.0f, false};

void pc_touch_event(const SDL_Event* e) {
    if (e == NULL)
        return;
    if (e->type != SDL_EVENT_FINGER_DOWN && e->type != SDL_EVENT_FINGER_MOTION &&
        e->type != SDL_EVENT_FINGER_UP)
    {
        return;
    }

    const float x = e->tfinger.x;
    const float y = e->tfinger.y;
    const SDL_FingerID fid = e->tfinger.fingerID;

    if (e->type == SDL_EVENT_FINGER_DOWN) {
        if (x < 0.45f && !s_touch_tracker.has_stick) {
            s_touch_tracker.stick_finger = fid;
            s_touch_tracker.stick_origin_x = x;
            s_touch_tracker.stick_origin_y = y;
            s_touch_tracker.has_stick = true;
        } else {
            uint16_t btn = 0;
            uint8_t tr_r = 0;
            if (y < 0.25f) {
                if (x < 0.65f)
                    btn = PAD_BUTTON_START;
                else
                    btn = PAD_TRIGGER_Z;
            } else {
                if (x >= 0.75f) {
                    if (y >= 0.60f)
                        btn = PAD_BUTTON_A;
                    else
                        btn = PAD_BUTTON_X;
                } else {
                    if (y >= 0.60f)
                        btn = PAD_BUTTON_B;
                    else
                        btn = PAD_BUTTON_Y;
                }
            }
            if (x > 0.80f && y < 0.35f) {
                tr_r = 255;
            }
            pc_touch_set_pad(s_touch_status.button | btn, s_touch_status.stickX,
                s_touch_status.stickY, s_touch_status.substickX, s_touch_status.substickY,
                s_touch_status.triggerLeft, tr_r ? tr_r : s_touch_status.triggerRight);
        }
    } else if (e->type == SDL_EVENT_FINGER_MOTION) {
        if (s_touch_tracker.has_stick && s_touch_tracker.stick_finger == fid) {
            const float dx = x - s_touch_tracker.stick_origin_x;
            const float dy = y - s_touch_tracker.stick_origin_y;
            const float radius = 0.08f;
            float sx = dx / radius;
            float sy = -dy / radius;
            if (sx > 1.0f)
                sx = 1.0f;
            if (sx < -1.0f)
                sx = -1.0f;
            if (sy > 1.0f)
                sy = 1.0f;
            if (sy < -1.0f)
                sy = -1.0f;
            pc_touch_set_pad(s_touch_status.button, (int8_t)(sx * 80.0f), (int8_t)(sy * 80.0f),
                s_touch_status.substickX, s_touch_status.substickY, s_touch_status.triggerLeft,
                s_touch_status.triggerRight);
        }
    } else if (e->type == SDL_EVENT_FINGER_UP) {
        if (s_touch_tracker.has_stick && s_touch_tracker.stick_finger == fid) {
            s_touch_tracker.has_stick = false;
            s_touch_tracker.stick_finger = -1;
            pc_touch_set_pad(s_touch_status.button, 0, 0, s_touch_status.substickX,
                s_touch_status.substickY, s_touch_status.triggerLeft, s_touch_status.triggerRight);
        } else {
            pc_touch_set_pad(0, s_touch_status.stickX, s_touch_status.stickY,
                s_touch_status.substickX, s_touch_status.substickY, s_touch_status.triggerLeft, 0);
        }
    }
}

#if defined(__ANDROID__)
JNIEXPORT void JNICALL Java_dev_melee_TouchControls_nativeSetTouchPad(JNIEnv* env, jclass clazz,
    jint buttons, jint stickX, jint stickY, jint cstickX, jint cstickY, jint triggerL,
    jint triggerR) {
    (void)env;
    (void)clazz;
    pc_touch_set_pad((uint16_t)buttons, (int8_t)stickX, (int8_t)stickY, (int8_t)cstickX,
        (int8_t)cstickY, (uint8_t)triggerL, (uint8_t)triggerR);
}

JNIEXPORT void JNICALL Java_dev_melee_TouchControls_nativeSetTouchActive(
    JNIEnv* env, jclass clazz, jboolean active) {
    (void)env;
    (void)clazz;
    pc_touch_set_active(active == JNI_TRUE);
}
#endif

#else

bool pc_touch_get_status(PADStatus* out) {
    (void)out;
    return false;
}

void pc_touch_apply(void) {}

void pc_touch_set_pad(uint16_t buttons, int8_t stickX, int8_t stickY, int8_t cstickX,
    int8_t cstickY, uint8_t triggerL, uint8_t triggerR) {
    (void)buttons;
    (void)stickX;
    (void)stickY;
    (void)cstickX;
    (void)cstickY;
    (void)triggerL;
    (void)triggerR;
}

void pc_touch_set_active(bool active) {
    (void)active;
}

void pc_touch_event(const SDL_Event* e) {
    (void)e;
}

#endif
