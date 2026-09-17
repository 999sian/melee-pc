/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <aurora/aurora.h>
#include <aurora/dvd.h>
#include <aurora/main.h>
#include <dolphin/ar.h>
#include <dolphin/ax.h>
#include <dolphin/card.h>
#include <dolphin/os.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdarg.h>
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

#if defined(__ANDROID__)
#include <android/log.h>
#endif

/* A log file sink, so a double-clicked build still leaves a diagnosable
 * trace after its console disappears. MELEE_LOG_FILE picks the path; an
 * empty value disables it. Windows defaults it on because that is where the
 * console is not a reliable place to read errors from. */
static FILE* log_file(void) {
    static FILE* fp;
    static bool tried;
    if (!tried) {
        tried = true;
        const char* path = getenv("MELEE_LOG_FILE");
#if defined(_WIN32)
        if (path == NULL) {
            path = "melee-pc.log";
        }
#endif
        if (path != NULL && path[0] != '\0') {
            fp = fopen(path, "w");
        }
    }
    return fp;
}

/* Milliseconds since the first log record. Without a time base there is no
 * way to line a frame stall up against what the engine was loading. */
static double log_now_ms(void) {
    static Uint64 t0;
    const Uint64 now = SDL_GetTicksNS();
    if (t0 == 0) {
        t0 = now;
    }
    return (double)(now - t0) / 1e6;
}

void pc_log_line(const char* fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    const double t = log_now_ms();
    fprintf(stderr, "[%9.3f] %s\n", t, msg);
    fflush(stderr);
    FILE* lf = log_file();
    if (lf != NULL) {
        fprintf(lf, "[%9.3f] %s\n", t, msg);
        fflush(lf);
    }
}

static void log_callback(
    AuroraLogLevel level, const char* module, const char* message, unsigned int len) {
#if defined(__ANDROID__)
    int prio = ANDROID_LOG_INFO;
    switch (level) {
    case LOG_DEBUG:
        prio = ANDROID_LOG_DEBUG;
        break;
    case LOG_INFO:
        prio = ANDROID_LOG_INFO;
        break;
    case LOG_WARNING:
        prio = ANDROID_LOG_WARN;
        break;
    case LOG_ERROR:
        prio = ANDROID_LOG_ERROR;
        break;
    case LOG_FATAL:
        prio = ANDROID_LOG_FATAL;
        break;
    }
    __android_log_print(prio, "Aurora", "[%s] %.*s", module, (int)len, message);
#else
    static const char* const names[] = {"DEBUG", "INFO", "WARN", "ERROR", "FATAL"};
    FILE* out = level >= LOG_ERROR ? stderr : stdout;
    const double t = log_now_ms();
    fprintf(out, "[%9.3f] [%s] %s: %.*s\n", t, names[level], module, (int)len, message);
    /* stdout is block-buffered when redirected to a file, and the abort()
     * below does not flush it. Without this, `melee.exe > log.txt` drops the
     * lines leading up to a fatal -- exactly the ones worth reading. */
    fflush(out);
    FILE* lf = log_file();
    if (lf != NULL) {
        fprintf(lf, "[%9.3f] [%s] %s: %.*s\n", t, names[level], module, (int)len, message);
        fflush(lf);
    }
#endif
    if (level == LOG_FATAL) {
        fflush(stdout);
        fflush(stderr);
        abort();
    }
}

#if defined(_WIN32)
#include <windows.h>

/* A hard crash never reaches log_callback, so the log would otherwise just
 * stop with no reason. Record the exception, the faulting address, and a
 * backtrace as "module+RVA" per frame -- that names the failing module and
 * feeds straight into addr2line against the matching build. */

/* Resolve an address to "module+RVA", which is what addr2line needs. */
static void describe_addr(void* addr, char* out, size_t out_size) {
    char path[MAX_PATH];
    HMODULE mod = NULL;
    if (GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCSTR)addr, &mod) &&
        GetModuleFileNameA(mod, path, sizeof(path)) != 0)
    {
        const char* base = strrchr(path, '\\');
        base = base != NULL ? base + 1 : path;
        snprintf(out, out_size, "%p %s+0x%llX", addr, base,
            (unsigned long long)((uintptr_t)addr - (uintptr_t)mod));
    } else {
        snprintf(out, out_size, "%p <unknown>", addr);
    }
}

static LONG WINAPI crash_handler(EXCEPTION_POINTERS* info) {
    void* frames[32];
    const USHORT count = CaptureStackBackTrace(0, 32, frames, NULL);
    const EXCEPTION_RECORD* rec = info->ExceptionRecord;

    FILE* streams[] = {stderr, log_file()};
    for (size_t i = 0; i < sizeof(streams) / sizeof(*streams); i++) {
        FILE* s = streams[i];
        if (s == NULL) {
            continue;
        }
        char where[MAX_PATH + 64];
        describe_addr((void*)rec->ExceptionAddress, where, sizeof(where));
        fprintf(
            s, "[FATAL] crash: exception 0x%08X at %s\n", (unsigned int)rec->ExceptionCode, where);
        /* For an access violation the second parameter is the address that
         * was touched; 0 vs garbage distinguishes a null deref from a wild
         * pointer, which is the first thing worth knowing. */
        if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
            const char* op = "access to";
            if (rec->ExceptionInformation[0] == 0) {
                op = "read from";
            } else if (rec->ExceptionInformation[0] == 1) {
                op = "write to";
            } else if (rec->ExceptionInformation[0] == 8) {
                op = "execute at";
            }
            fprintf(s, "[FATAL] crash: %s address 0x%llX\n", op,
                (unsigned long long)rec->ExceptionInformation[1]);
        }
        for (USHORT f = 0; f < count; f++) {
            describe_addr(frames[f], where, sizeof(where));
            fprintf(s, "[FATAL]   #%02u %s\n", (unsigned)f, where);
        }
        fflush(s);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

static void usage(const char* argv0) {
    fprintf(stderr,
        "usage: %s [--no-card] [--dvd] [disc image (iso/gcm/ciso/rvz/...)]\nNo disc argument opens "
        "the launcher.\n",
        argv0);
    exit(2);
}

#include "pc/input_poll.h"

static void pc_shutdown_once(void) {
    static bool done;
    if (done) {
        return;
    }
    done = true;
    /* Stop producers before joining DMA and destroying platform resources.
     * An unjoined ARQ worker aborts in std::thread's static destructor. */
    pc_input_poll_shutdown();
    AXQuit();
    aurora_dvd_close();
    pc_textures_shutdown();
    ARQReset();
    aurora_shutdown();
}

static const struct {
    const char* name;
    AuroraBackend backend;
} k_backends[] = {
    {"auto", BACKEND_AUTO},
    {"d3d11", BACKEND_D3D11},
    {"d3d12", BACKEND_D3D12},
    {"metal", BACKEND_METAL},
    {"vulkan", BACKEND_VULKAN},
    {"opengl", BACKEND_OPENGL},
    {"gles", BACKEND_OPENGLES},
    {"webgpu", BACKEND_WEBGPU},
    {"null", BACKEND_NULL},
};

/* Local, so this costs no header question: strcasecmp lives in <strings.h> on
 * POSIX and is declared in <string.h> on MinGW only when __STRICT_ANSI__ is
 * off, which depends on the -std the target happens to use. */
static int ieq(const char* a, const char* b) {
    for (; *a != '\0' && *b != '\0'; a++, b++) {
        int ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') {
            ca += 'a' - 'A';
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb += 'a' - 'A';
        }
        if (ca != cb) {
            return 0;
        }
    }
    return *a == *b;
}

static const char* backend_name(AuroraBackend b) {
    for (size_t i = 0; i < sizeof(k_backends) / sizeof(*k_backends); i++) {
        if (k_backends[i].backend == b) {
            return k_backends[i].name;
        }
    }
    return "?";
}

/* MELEE_BACKEND pins the graphics backend instead of taking aurora's preferred
 * order. That order is per-platform -- on Windows D3D12 comes before Vulkan,
 * and on Linux only Vulkan is built -- so the same code takes a different path
 * on each, and a fault that appears on one and not the other cannot be bisected
 * without being able to pin it. `MELEE_BACKEND=vulkan` on Windows is the direct
 * test for "is this the D3D12 path?". An unknown value lists the valid ones and
 * falls back to BACKEND_AUTO rather than failing the run. */
static AuroraBackend backend_from_env(void) {
    const char* want = getenv("MELEE_BACKEND");
    if (want == NULL || want[0] == '\0') {
#if defined(__APPLE__)
        return BACKEND_METAL;
#else
        return BACKEND_AUTO;
#endif
    }
    for (size_t i = 0; i < sizeof(k_backends) / sizeof(*k_backends); i++) {
        if (ieq(want, k_backends[i].name)) {
            return k_backends[i].backend;
        }
    }
    fprintf(stderr, "MELEE_BACKEND: unknown backend '%s'; valid values are", want);
    for (size_t i = 0; i < sizeof(k_backends) / sizeof(*k_backends); i++) {
        fprintf(stderr, "%s %s", i ? "," : "", k_backends[i].name);
    }
    fprintf(stderr, "\nMELEE_BACKEND: falling back to auto\n");
    return BACKEND_AUTO;
}

MELEE_EXPORT int main(int argc, char* argv[]) {
#if defined(_WIN32)
    SetUnhandledExceptionFilter(crash_handler);
#endif

    /* Pre-initialize GameCube OS memory immediately so that MEM1 (96 MB) is
     * committed strictly below 4GB at process startup before SDL, graphics
     * drivers, and fullscreen swapchains fragment low virtual memory. */
    OSInit();

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

#if defined(TARGET_OS_IPHONE) || defined(__APPLE__)
    static char resolved_disc_path[1024];
    if (disc == NULL) {
        const char* home = getenv("HOME");
        const char* candidates[] = {"Documents/melee.ciso", "Documents/melee.iso",
            "Documents/game.ciso", "Documents/game.iso", "melee.ciso", "melee.iso", NULL};
        for (int p = 0; candidates[p] != NULL; p++) {
            if (home != NULL && home[0] != '\0') {
                snprintf(
                    resolved_disc_path, sizeof(resolved_disc_path), "%s/%s", home, candidates[p]);
                FILE* f = fopen(resolved_disc_path, "rb");
                if (f != NULL) {
                    fclose(f);
                    disc = resolved_disc_path;
                    pc_log_line("Auto-detected disc in container: %s", disc);
                    break;
                }
            }
            FILE* f = fopen(candidates[p], "rb");
            if (f != NULL) {
                fclose(f);
                disc = candidates[p];
                pc_log_line("Auto-detected disc in cwd: %s", disc);
                break;
            }
        }
    }
#endif
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
#if defined(__ANDROID__)
        .logLevel = LOG_DEBUG,
#else
        .logLevel = getenv("MELEE_DEBUG") ? LOG_DEBUG : LOG_INFO,
#endif
        .windowWidth = 1280,
        .windowHeight = 960,
        .logCallback = log_callback,
        .desiredBackend = backend_from_env(),
        .mem1Size = PC_MEM1_SIZE,
        .mem2Size = PC_ARAM_SIZE,
    };
    pc_launcher_configure(&config);

    const AuroraInfo info = aurora_initialize(argc, argv, &config);
    /* Record which backend was actually selected. Without this the log cannot
     * say whether a run went through D3D12 or Vulkan, which is the first thing
     * worth knowing about a fault that only reproduces on one platform. */
    pc_log_line("graphics backend: %s%s", backend_name(info.backend),
        config.desiredBackend == BACKEND_AUTO ? " (auto)" : " (MELEE_BACKEND)");
    /* Closing the window exits from inside the frame loop (pc/vi.c), which
     * would otherwise skip aurora_shutdown() entirely: Dawn's static
     * destructors then tear the device down while aurora still thinks it is
     * live, its device-lost callback reports FATAL and log_callback aborts.
     * Run the shutdown from atexit so every exit path goes through it. */
    atexit(pc_shutdown_once);

    const int launched = pc_launcher_run(disc, info.window);
    if (launched != 1)
        return launched == 0 ? 0 : 1;

    pc_menu_init(info.window);
    pc_platform_init();
    aurora_card_set_present(card);
    int rc = melee_main();
    pc_shutdown_once();
    return rc;
}
