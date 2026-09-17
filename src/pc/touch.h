/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_TOUCH_H
#define PC_TOUCH_H

#include <dolphin/pad.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

union SDL_Event;

void pc_touch_apply(void);
bool pc_touch_get_status(PADStatus* out);
void pc_touch_set_pad(uint16_t buttons, int8_t stickX, int8_t stickY, int8_t cstickX,
    int8_t cstickY, uint8_t triggerL, uint8_t triggerR);
void pc_touch_set_active(bool active);
void pc_touch_event(const union SDL_Event* e);

#ifdef __cplusplus
}
#endif

#endif /* PC_TOUCH_H */
