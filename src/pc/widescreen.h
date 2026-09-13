#pragma once
#include <stdbool.h>
struct HSD_CObj;
#ifdef __cplusplus
extern "C" {
#endif
void pc_widescreen_set_mode(int mode);
void pc_widescreen_set_scene(bool supported);
void pc_widescreen_apply_camera(struct HSD_CObj* camera, float projection[4][4]);
void pc_widescreen_apply_screen(void);
float pc_widescreen_scale(void);
#ifdef __cplusplus
}
#endif
