#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
void pc_widescreen_set_mode(int mode);
void pc_widescreen_set_scene(bool supported);
void pc_widescreen_update(void);
float pc_widescreen_scale(void);
#ifdef __cplusplus
}
#endif
