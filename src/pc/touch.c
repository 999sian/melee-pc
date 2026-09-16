/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pc/touch.h"
#include "pc/pc.h"
#include <dolphin/pad.h>
#include <string.h>

#if defined(__ANDROID__)
#include <jni.h>
#include <pthread.h>

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

__attribute__((visibility("default"))) JNIEXPORT void JNICALL
Java_dev_melee_TouchControls_nativeSetTouchPad(JNIEnv* env, jclass clazz, jint buttons, jint stickX,
    jint stickY, jint cstickX, jint cstickY, jint triggerL, jint triggerR) {
    (void)env;
    (void)clazz;
    pc_touch_set_pad((uint16_t)buttons, (int8_t)stickX, (int8_t)stickY, (int8_t)cstickX,
        (int8_t)cstickY, (uint8_t)triggerL, (uint8_t)triggerR);
}

__attribute__((visibility("default"))) JNIEXPORT void JNICALL
Java_dev_melee_TouchControls_nativeSetTouchActive(JNIEnv* env, jclass clazz, jboolean active) {
    (void)env;
    (void)clazz;
    pc_touch_set_active(active == JNI_TRUE);
}

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

#endif
