#include <aurora/aurora.h>
#include <aurora/dvd.h>
#include <aurora/main.h>
#include <dolphin/ar.h>
#include <dolphin/ax.h>
#include <dolphin/card.h>
#include <dolphin/os.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__ANDROID__)
#define MELEE_EXPORT __attribute__((visibility("default")))
#else
#define MELEE_EXPORT
#endif

#include "pc/pc.h"
#include "pc/launcher.h"

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
    fprintf(stderr, "usage: %s [--no-card] [--dvd] [disc image (iso/gcm/ciso/rvz/...)]\nNo disc argument opens the launcher.\n", argv0);
    exit(2);
}

static void pc_shutdown_once(void)
{
    static bool done;
    if (done) {
        return;
    }
    done = true;
    /* Stop producers before joining DMA and destroying platform resources.
     * An unjoined ARQ worker aborts in std::thread's static destructor. */
    AXQuit();
    aurora_dvd_close();
    ARQReset();
    aurora_shutdown();
}

MELEE_EXPORT int main(int argc, char* argv[])
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
    AuroraConfig config = {
        /* appName doubles as the window title; the save/cache dirs stay
         * pinned so a renamed test window still uses the same memory card. */
        .appName = getenv("MELEE_WINDOW_TITLE") ? getenv("MELEE_WINDOW_TITLE") : "melee-pc",
        .userPath = SDL_GetPrefPath(NULL, "melee-pc"),
        .cachePath = SDL_GetPrefPath(NULL, "melee-pc"),
        .msaa = 1,
        .maxTextureAnisotropy = 16,
        /* MELEE_VSYNC=0 picks Mailbox/Immediate instead of FifoRelaxed; some
         * compositors stop scanning out a FifoRelaxed surface and the window
         * then sits on a stale frame while the game runs on. */
        .vsync = !(getenv("MELEE_VSYNC") && getenv("MELEE_VSYNC")[0] == '0'),
        .logLevel = getenv("MELEE_DEBUG") ? LOG_DEBUG : LOG_INFO,
        .windowWidth = 1280,
        .windowHeight = 960,
        .logCallback = log_callback,
        .mem1Size = PC_MEM1_SIZE,
        .mem2Size = PC_ARAM_SIZE,
    };
    pc_launcher_configure(&config);
    const AuroraInfo info = aurora_initialize(argc, argv, &config);
    /* Closing the window exits from inside the frame loop (pc/vi.c), which
     * would otherwise skip aurora_shutdown() entirely: Dawn's static
     * destructors then tear the device down while aurora still thinks it is
     * live, its device-lost callback reports FATAL and log_callback aborts.
     * Run the shutdown from atexit so every exit path goes through it. */
    atexit(pc_shutdown_once);

    const int launched = pc_launcher_run(disc, info.window);
    if (launched != 1) return launched == 0 ? 0 : 1;

    pc_menu_init(info.window);
    pc_platform_init();
    aurora_card_set_present(card);
    int rc = melee_main();
    pc_shutdown_once();
    return rc;
}
