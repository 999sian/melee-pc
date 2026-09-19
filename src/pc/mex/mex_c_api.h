/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_MEX_C_API_H
#define PC_MEX_C_API_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool pc_mex_is_active(void);
static inline bool pc_mex_is_custom_fighter(int ckind) {
    return pc_mex_is_active() && ckind >= 27;
}
int pc_mex_get_css_icon_count(void);
bool pc_mex_populate_css_icons(void* out_icons, int max_icons);
const char* pc_mex_get_fighter_name(int ckind);
const char* pc_mex_get_fighter_filename(int ckind);
const char* pc_mex_get_fighter_data_symbol(int ckind);
int pc_mex_get_fighter_costume_count(int ckind);
const char* pc_mex_get_fighter_costume_file(int ckind, int costume_idx);
int pc_mex_get_fighter_count(void);
const char* pc_mex_get_fighter_rst_anim_file(int ckind);
int pc_mex_get_fighter_announcer_call(int ckind);
const char* pc_mex_get_fighter_anim_file(int ckind);
int pc_mex_get_fighter_anim_count(int ckind);

struct FighterPartsTable;
struct FighterPartsTable* pc_mex_get_fighter_parts_table(int ckind);

#ifdef __cplusplus
}
#endif

#endif /* PC_MEX_C_API_H */
