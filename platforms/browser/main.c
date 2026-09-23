/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pc/discfont.h"
#include "pc/launcher.h"
#include "pc/pc.h"
#include <SDL3/SDL.h>
#include <aurora/aurora.h>
#include <aurora/dvd.h>
#include <dolphin/card.h>
#include <emscripten.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
extern int melee_main(void);
void pc_log_line(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vfprintf(stderr, fmt, a);
    va_end(a);
    fputc('\n', stderr);
}
static void log_message(AuroraLogLevel level, const char* module, const char* message, unsigned n) {
    fprintf(stderr, "[%s] %.*s\n", module, n, message);
    if (level == LOG_FATAL)
        abort();
}
/* The hosting page owns settings and the disc picker. */
void pc_menu_init(SDL_Window* w) {
    (void)w;
}
void pc_menu_update(void) {}
void pc_menu_toggle(void) {}
void pc_menu_event(const SDL_Event* e) {
    (void)e;
}
bool pc_menu_is_open(void) {
    return false;
}
int main(int argc, char** argv) {
    mkdir("/saves", 0777);
    mkdir("/cache", 0777);
    if (!aurora_dvd_open("disc")) {
        fprintf(stderr, "Unsupported or unreadable disc\n");
        return 1;
    }
    if (!pc_load_disc_fonts("disc"))
        return 2;
    AuroraConfig c = {.appName = "Melee",
        .userPath = "/saves",
        .cachePath = "/cache",
        .msaa = 1,
        .maxTextureAnisotropy = 1,
        .vsync = true,
        .logLevel = LOG_INFO,
        .windowWidth = 960,
        .windowHeight = 720,
        .logCallback = log_message,
        .desiredBackend = BACKEND_WEBGPU,
        .mem1Size = PC_MEM1_SIZE,
        .mem2Size = PC_ARAM_SIZE};
    /* The page owns the canvas, so it owns the render size too. */
    c.windowWidth = EM_ASM_INT({ return Module.canvas.width; });
    c.windowHeight = EM_ASM_INT({ return Module.canvas.height; });
    AuroraInfo info = aurora_initialize(argc, argv, &c);
    (void)info;
    extern void browser_prepare_graphics(void);
    browser_prepare_graphics();
    pc_platform_init();
    aurora_card_set_present(true);
    return melee_main();
}
