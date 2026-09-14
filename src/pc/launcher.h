/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <aurora/aurora.h>
#ifdef __cplusplus
extern "C" {
#endif
void pc_launcher_configure(AuroraConfig* config);
/* 1: disc opened, 0: user quit, -1: launcher initialization failed. */
int pc_launcher_run(const char* command_line_disc, SDL_Window* window);
void pc_menu_init(SDL_Window* window);
void pc_menu_update(void);
void pc_menu_toggle(void);
void pc_menu_event(const union SDL_Event* event);
bool pc_menu_is_open(void);
#ifdef __cplusplus
}
#endif