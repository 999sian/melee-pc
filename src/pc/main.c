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
#include <errno.h>
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

#if defined(__ANDROID__)
/* Android writes crash backtraces to a protobuf tombstone under /data, which
 * an unrooted device only surfaces through `adb bugreport` -- two minutes per
 * iteration, and the rotation drops them faster than that when the app is
 * crash-looping. So unwind in-process and put the frames straight in logcat.
 * dladdr gives the library and the nearest exported symbol; the offsets are
 * what `addr2line -f -C -e libmelee.so` wants. */
#include <dlfcn.h>
#include <signal.h>
#include <unistd.h>
#include <unwind.h>

typedef struct {
    int n;
} MeleeUnwindState;

static _Unwind_Reason_Code melee_unwind_frame(struct _Unwind_Context* ctx, void* arg) {
    MeleeUnwindState* st = (MeleeUnwindState*)arg;
    uintptr_t pc = _Unwind_GetIP(ctx);
    if (pc == 0 || st->n >= 32) {
        return _URC_END_OF_STACK;
    }
    Dl_info info;
    memset(&info, 0, sizeof info);
    if (dladdr((void*)pc, &info) != 0 && info.dli_fname != NULL) {
        const char* base = strrchr(info.dli_fname, '/');
        __android_log_print(ANDROID_LOG_FATAL, "melee", "  #%02d pc %012zx  %s (%s+%zu)", st->n,
            (size_t)(pc - (uintptr_t)info.dli_fbase), base ? base + 1 : info.dli_fname,
            info.dli_sname ? info.dli_sname : "?",
            info.dli_saddr ? (size_t)(pc - (uintptr_t)info.dli_saddr) : (size_t)0);
    } else {
        __android_log_print(ANDROID_LOG_FATAL, "melee", "  #%02d pc %016zx  ?", st->n, (size_t)pc);
    }
    st->n++;
    return _URC_NO_REASON;
}

/* Handlers displaced by ours. Android installs debuggerd's SIGABRT/SIGSEGV
 * handler at process start, and that is what writes the tombstone -- with
 * the abort message, which is the only place a scudo or FORTIFY diagnosis
 * appears. Resetting to SIG_DFL instead of chaining loses it: the default
 * action just kills the process, so an in-process backtrace is bought at
 * the price of every tombstone (measured: no tombstone for any crash this
 * session, while /data/tombstones still held last week's). */
static struct sigaction s_prev_sa[NSIG];

static void melee_fatal_signal(int sig, siginfo_t* si, void* uc) {
    (void)uc;
    __android_log_print(ANDROID_LOG_FATAL, "melee", "FATAL signal %d at %p (tid %d)", sig,
        si ? si->si_addr : NULL, (int)gettid());
    MeleeUnwindState st = {0};
    _Unwind_Backtrace(melee_unwind_frame, &st);
    /* Hand back to whoever had it, so a real tombstone is still written. */
    if (sig >= 0 && sig < NSIG) {
        sigaction(sig, &s_prev_sa[sig], NULL);
    } else {
        signal(sig, SIG_DFL);
    }
    raise(sig);
}

static void melee_install_crash_handler(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = melee_fatal_signal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    const int sigs[] = {SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE};
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++) {
        sigaction(sigs[i], &sa, &s_prev_sa[sigs[i]]);
    }
}
#else
static void melee_install_crash_handler(void) {}
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

#if defined(__APPLE__)
#include <os/log.h>
#endif

void pc_log_line(const char* fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    const double t = log_now_ms();
#if defined(__APPLE__)
    os_log_with_type(OS_LOG_DEFAULT, OS_LOG_TYPE_DEFAULT, "[Melee] %{public}s", msg);
#elif defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_INFO, "Melee", "%s", msg); /* stderr is not logcat */
#endif
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
#if defined(__APPLE__)
    os_log_with_type(OS_LOG_DEFAULT, level >= LOG_ERROR ? OS_LOG_TYPE_ERROR : OS_LOG_TYPE_DEFAULT,
        "[Aurora:%{public}s] %{public}.*s", module, (int)len, message);
#endif
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
        "usage: %s [--no-card] [--dvd] [disc image (iso/gcm/ciso/rvz/...)]\n"
        "       %s --version | --help\n"
        "No disc argument opens the launcher. MELEE_BACKEND, MELEE_VSYNC, MELEE_LOG_FILE,\n"
        "MELEE_DEBUG and the other MELEE_* knobs are documented in README.md.\n",
        argv0, argv0);
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

/* Android has no way to hand a process an environment: `am start` passes
 * extras, not env, so every MELEE_* knob -- the netplay fixtures, the heap
 * check, the log file -- is unreachable on a phone, which is exactly where
 * they are hardest to do without. Read them from a file next to the disc
 * instead, one NAME=VALUE per line, '#' comments ignored, existing
 * environment always winning so a shell-set value is never overridden.
 * Harmless on desktop: no file, no effect. */
static void pc_env_file_bootstrap(void) {
    const char* paths[] = {
#if defined(__ANDROID__)
        "/sdcard/Android/data/dev.melee.game/files/melee-env.txt",
#endif
        "melee-env.txt",
    };
    for (size_t i = 0; i < sizeof paths / sizeof paths[0]; i++) {
        FILE* f = fopen(paths[i], "r");
        if (f == NULL) {
#if defined(__ANDROID__)
            /* Straight to logcat: this runs before the aurora log callback is
             * installed, so pc_log_line() would be dropped and the knobs would
             * look silently inert. */
            __android_log_print(
                ANDROID_LOG_INFO, "melee", "env: no %s (errno %d)", paths[i], errno);
#endif
            continue;
        }
        char line[512];
        while (fgets(line, sizeof line, f) != NULL) {
            char* s = line;
            while (*s == ' ' || *s == '\t') {
                s++;
            }
            if (*s == '#' || *s == '\n' || *s == '\0') {
                continue;
            }
            char* eq = strchr(s, '=');
            if (eq == NULL) {
                continue;
            }
            *eq = '\0';
            char* val = eq + 1;
            size_t n = strlen(val);
            while (n > 0 && (val[n - 1] == '\n' || val[n - 1] == '\r' || val[n - 1] == ' ')) {
                val[--n] = '\0';
            }
            setenv(s, val, 0); /* 0: never clobber a real environment value */
        }
        fclose(f);
#if defined(__ANDROID__)
        __android_log_print(ANDROID_LOG_INFO, "melee", "env: applied %s", paths[i]);
#endif
        pc_log_line("env: applied %s", paths[i]);
        return;
    }
}

MELEE_EXPORT int main(int argc, char* argv[]) {
    melee_install_crash_handler(); /* before anything can crash */
    pc_env_file_bootstrap();       /* before anything calls getenv() */
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
        const char* d = NULL;
        if (strcmp(argv[i], "--dvd") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s: --dvd needs a disc path\n", argv[0]);
                usage(argv[0]);
            }
            d = argv[++i];
        } else if (strcmp(argv[i], "--no-card") == 0) {
            card = false;
        } else if (strcmp(argv[i], "--version") == 0) {
            printf("melee-pc %s\n", pc_app_version());
            return 0;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "%s: unknown option %s\n", argv[0], argv[i]);
            usage(argv[0]);
        } else {
            d = argv[i];
        }
        if (d != NULL) {
            if (disc != NULL) {
                fprintf(stderr, "%s: more than one disc given (%s, %s)\n", argv[0], disc, d);
                usage(argv[0]);
            }
            /* A `content://` URI is not a filesystem path. On Android the
             * disc arrives as one from the file picker, and it is opened
             * through SDL_IOStream (`pc_open_nod_disc`, src/pc/disc_open.h);
             * `fopen` cannot open one, so probing with it rejected every
             * disc a phone can actually hand us. */
            const bool uri = strncmp(d, "content://", 10) == 0;
            FILE* f = uri ? NULL : fopen(d, "rb");
            if (d[0] == '\0' || (!uri && f == NULL)) {
                fprintf(stderr, "%s: cannot open disc %s\n", argv[0], d);
#if defined(__ANDROID__)
                /* exit() runs static destructors while the Java UI thread is
                 * still drawing, which aborts it inside minikin with
                 * "FORTIFY: pthread_mutex_lock called on a destroyed mutex":
                 * a crash report for what is really a bad path. Carry on
                 * into the launcher with no disc, exactly as if the app had
                 * been started without one. */
                continue;
#else
                exit(2);
#endif
            }
            if (f != NULL) {
                fclose(f);
            }
            disc = d;
        }
    }

    AuroraConfig config = {
        /* appName doubles as the window title; the save/cache dirs stay
         * pinned so a renamed test window still uses the same memory card. */
        .appName = getenv("MELEE_WINDOW_TITLE") ? getenv("MELEE_WINDOW_TITLE") : "melee-pc",
        .userPath = SDL_GetPrefPath(NULL, "melee-pc"),
        /* MELEE_CACHE_DIR: two instances on one machine (netplay testing)
         * must not share the pipeline-cache SQLite file. */
        .cachePath = getenv("MELEE_CACHE_DIR") ? getenv("MELEE_CACHE_DIR") :
                                                 SDL_GetPrefPath(NULL, "melee-pc"),
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
    /* Before aurora_initialize -> SDL_Init(JOYSTICK): claims the GC adapter's
     * hidapi hint so SDL's rescaling driver leaves it for our raw path. */
    pc_gcadapter_init();
    pc_launcher_configure(&config);

    const AuroraInfo info = aurora_initialize(argc, argv, &config);
    /* Record which backend was actually selected and the adapter it landed on.
     * Without this the log cannot say whether a run went through D3D12 or
     * Vulkan, or on which GPU/driver, which is the first thing worth knowing
     * about a fault that only reproduces on one machine. The origin has to be
     * derived from info.backend, not from the request alone: a run pinned to
     * d3d11 that aurora quietly fell back to d3d12 would otherwise log
     * "d3d12 (pinned)", which is the opposite of the truth. */
    char origin[64];
    if (config.desiredBackend == BACKEND_AUTO) {
        snprintf(origin, sizeof(origin), " (auto)");
    } else if (info.backend == config.desiredBackend) {
        snprintf(origin, sizeof(origin), " (pinned)");
    } else {
        snprintf(
            origin, sizeof(origin), " (fallback from %s)", backend_name(config.desiredBackend));
    }
    pc_log_line("graphics backend: %s%s, adapter: %s [%04x:%04x], driver: %s",
        backend_name(info.backend), origin, info.adapterName, info.adapterVendorId,
        info.adapterDeviceId, info.adapterDriver);
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
