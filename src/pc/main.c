#include <aurora/aurora.h>
#include <aurora/dvd.h>
#include <aurora/main.h>
#include <dolphin/card.h>
#include <dolphin/os.h>

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pc/pc.h"

int melee_main(void);

static void log_callback(AuroraLogLevel level, const char* module, const char* message, unsigned int len)
{
    static const char* const names[] = { "DEBUG", "INFO", "WARN", "ERROR", "FATAL" };
    FILE* out = level >= LOG_ERROR ? stderr : stdout;
    fprintf(out, "[%s] %s: %.*s\n", names[level], module, (int) len, message);
    if (level == LOG_FATAL) {
        fflush(out);
        abort();
    }
}

static void usage(const char* argv0)
{
    fprintf(stderr, "usage: %s [--no-card] [--dvd] <disc image (iso/gcm/ciso/rvz/...)>\n", argv0);
    exit(2);
}

int main(int argc, char* argv[])
{
    const char* disc = NULL;
    bool card = true;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dvd") == 0 && i + 1 < argc) {
            disc = argv[++i];
        } else if (strcmp(argv[i], "--no-card") == 0) {
            card = false;
        } else if (argv[i][0] != '-') {
            disc = argv[i];
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
        }
    }
    if (disc == NULL) {
        usage(argv[0]);
    }

    const AuroraConfig config = {
        /* appName doubles as the window title; the save/cache dirs stay
         * pinned so a renamed test window still uses the same memory card. */
        .appName = getenv("MELEE_WINDOW_TITLE") ? getenv("MELEE_WINDOW_TITLE") : "melee-pc",
        .userPath = SDL_GetPrefPath(NULL, "melee-pc"),
        .cachePath = SDL_GetPrefPath(NULL, "melee-pc"),
        .msaa = 1,
        .maxTextureAnisotropy = 16,
        .vsync = true,
        .logLevel = getenv("MELEE_DEBUG") ? LOG_DEBUG : LOG_INFO,
        .windowWidth = 1280,
        .windowHeight = 960,
        .logCallback = log_callback,
        .mem1Size = PC_MEM1_SIZE,
        .mem2Size = PC_ARAM_SIZE,
    };
    aurora_initialize(argc, argv, &config);

    if (!aurora_dvd_open(disc)) {
        fprintf(stderr, "failed to open disc image: %s\n", disc);
        return 1;
    }

    pc_platform_init();
    aurora_card_set_present(card);
    int rc = melee_main();
    aurora_dvd_close();
    aurora_shutdown();
    return rc;
}
