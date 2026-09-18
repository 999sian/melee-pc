/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Wii U / Switch GameCube controller adapter (WUP-028; Mayflash in Wii U
 * mode presents the same device), read directly so the game gets the
 * controller's raw 8-bit values.
 *
 * SDL3's hidapi GameCube driver rescales every axis from an assumed 128+-88
 * range that then grows to whatever it observes (SDL_hidapi_gamecube.c
 * ResetAxisRange / READ_AXIS -> HIDAPI_RemapVal), and aurora divides that back
 * to 8 bits: a stick at raw +80 reaches the game as +116, a trigger reads 0
 * until half travel, and the scale moves mid-session. Melee's dash, tilt and
 * light-shield thresholds are all in raw units, so the adapter is read here:
 * 37-byte 0x21 reports through SDL's hidapi (SDL_hid_*, no new dependency;
 * SDL routes this VID/PID to its libusb backend on every platform, see
 * SDL_hidapi.c SDL_libusb_required[]), drained from the 1000 Hz input thread,
 * origin-relative like the GC SDK's PADRead, published as virtual pad status.
 * Adapter slot N is always PAD port N.
 *
 * MELEE_GC_ADAPTER=0 hands the adapter back to SDL's driver.
 */
#include "pc/pc.h"

#include <dolphin/pad.h>
/* rumble.h references DiscU16 (on-disc rumble stream); disc.h defines it for
 * this C TU, which is not force-included with compat.h like the game code. */
#include "pc/disc.h"
#include <sysdolphin/baselib/rumble.h>

#include <SDL3/SDL.h>

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define GC_VID 0x057E
#define GC_PID 0x0337
#define GC_SLOTS 4
#define GC_REPORT 37 /* 0x21 + 4 x 9 slot bytes */
#define GC_SLOT_WIRED 0x10
#define GC_SLOT_WIRELESS 0x20
#define GC_SLOT_RUMBLE_POWER 0x04

/* Game-side motor state (sysdolphin/baselib/rumble.c). last_status is what
 * the game last asked PADControlMotor for: 0 hard stop, 1 stop, 2 rumble.
 * aurora's PADControlMotor only talks to SDL gamepads, so the raw path reads
 * the same state here instead of threading a new hook through aurora. */
extern HSD_RumbleData HSD_Rumble_804C22E0[GC_SLOTS];

static bool s_enabled;
static SDL_hid_device* s_dev;
static int s_retry_ms;
static bool s_warned_open;
static uint8_t s_rumble[1 + GC_SLOTS] = {0x11};

/* Poll-thread state. */
static PADStatus s_status[GC_SLOTS];
static bool s_present[GC_SLOTS];
static uint8_t s_origin[GC_SLOTS][6];
static uint64_t s_combo_since[GC_SLOTS]; /* X+Y+Start held since (ns), 0 = not held */

/* Cross-thread snapshot for the HUD: raw[6] | present<<48 | wireless<<49. */
static _Atomic uint64_t s_raw[GC_SLOTS];
static _Atomic uint64_t s_reports;

void pc_gcadapter_init(void) {
    const char* env = getenv("MELEE_GC_ADAPTER");
    s_enabled = !(env != NULL && env[0] == '0');
    if (s_enabled) {
        /* Set before SDL_Init(SDL_INIT_JOYSTICK): SDL's driver must not open
         * the adapter, or its rescaled copy would win the virtual-pad merge. */
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_GAMECUBE, "0");
    }
}

static void clear_slot(int i) {
    s_present[i] = false;
    memset(&s_status[i], 0, sizeof(s_status[i]));
    s_combo_since[i] = 0;
    atomic_store_explicit(&s_raw[i], 0, memory_order_relaxed);
    /* Port 1 (i==0) is owned by keyboard.c's merge; it clears it when quiet. */
    if (i != 0) {
        PADClearVirtualStatus((u32)i);
    }
}

static void try_open(void) {
    /* Serialise against SDL's own hidapi enumeration (same udev/libusb
     * contexts), which runs under the joystick lock. */
    SDL_LockJoysticks();
    s_dev = SDL_hid_open(GC_VID, GC_PID, NULL);
    SDL_hid_device_info* info = s_dev == NULL ? SDL_hid_enumerate(GC_VID, GC_PID) : NULL;
    SDL_UnlockJoysticks();
    if (s_dev == NULL) {
        if (info != NULL && !s_warned_open) {
            s_warned_open = true;
            pc_log_line("GC adapter: WUP-028 attached but could not be opened: %s"
#if !defined(_WIN32) && !defined(__APPLE__)
                        " (udev rule: SUBSYSTEM==\"usb\", ATTRS{idVendor}==\"057e\", "
                        "ATTRS{idProduct}==\"0337\", MODE=\"0666\")"
#endif
                ,
                SDL_GetError());
        }
        SDL_hid_free_enumeration(info);
        return;
    }
    /* Start streaming (same init as SDL's driver). */
    const uint8_t init = 0x13;
    if (SDL_hid_write(s_dev, &init, 1) != 1) {
        pc_log_line("GC adapter: init write failed: %s", SDL_GetError());
        SDL_hid_close(s_dev);
        s_dev = NULL;
        return;
    }
    memset(s_rumble + 1, 0, GC_SLOTS);
    s_warned_open = false;
    pc_log_line(
        "GC adapter: WUP-028 opened, raw 1000 Hz path active (MELEE_GC_ADAPTER=0 to use SDL)");
}

static void close_dev(const char* why) {
    pc_log_line("GC adapter: %s", why);
    SDL_hid_close(s_dev);
    s_dev = NULL;
    for (int i = 0; i < GC_SLOTS; i++) {
        clear_slot(i);
    }
}

static s8 rel8(uint8_t v, uint8_t origin) {
    const int d = (int)v - (int)origin;
    return (s8)(d < -128 ? -128 : d > 127 ? 127 : d);
}

static u8 relu8(uint8_t v, uint8_t origin) {
    return v > origin ? (u8)(v - origin) : 0;
}

/* One 9-byte slot: [status][buttons][buttons][sx][sy][cx][cy][L][R]. */
static void parse_slot(int i, const uint8_t* s, uint64_t now_ns) {
    const bool present = (s[0] & (GC_SLOT_WIRED | GC_SLOT_WIRELESS)) != 0;
    if (!present) {
        if (s_present[i]) {
            pc_log_line("GC adapter: port %d controller removed", i + 1);
            clear_slot(i);
        }
        return;
    }
    if (!s_present[i]) {
        /* A real controller captures its origin at power-on; the adapter does
         * not forward it, so take the first report, as Dolphin does. */
        memcpy(s_origin[i], s + 3, 6);
        s_present[i] = true;
        pc_log_line("GC adapter: port %d %s controller, origin %u,%u c %u,%u L %u R %u", i + 1,
            s[0] & GC_SLOT_WIRELESS ? "wireless" : "wired", s[3], s[4], s[5], s[6], s[7], s[8]);
    }

    u16 b = 0;
    if (s[1] & 0x01)
        b |= PAD_BUTTON_A;
    if (s[1] & 0x02)
        b |= PAD_BUTTON_B;
    if (s[1] & 0x04)
        b |= PAD_BUTTON_X;
    if (s[1] & 0x08)
        b |= PAD_BUTTON_Y;
    if (s[1] & 0x10)
        b |= PAD_BUTTON_LEFT;
    if (s[1] & 0x20)
        b |= PAD_BUTTON_RIGHT;
    if (s[1] & 0x40)
        b |= PAD_BUTTON_DOWN;
    if (s[1] & 0x80)
        b |= PAD_BUTTON_UP;
    if (s[2] & 0x01)
        b |= PAD_BUTTON_START;
    if (s[2] & 0x02)
        b |= PAD_TRIGGER_Z;
    if (s[2] & 0x04)
        b |= PAD_TRIGGER_R;
    if (s[2] & 0x08)
        b |= PAD_TRIGGER_L;

    /* X+Y+Start for 3 s recentres, the combo the controller itself honours. */
    const u16 combo = PAD_BUTTON_X | PAD_BUTTON_Y | PAD_BUTTON_START;
    if ((b & combo) == combo) {
        if (s_combo_since[i] == 0) {
            s_combo_since[i] = now_ns;
        } else if (now_ns - s_combo_since[i] >= 3000000000ull) {
            memcpy(s_origin[i], s + 3, 6);
            s_combo_since[i] = now_ns;
            pc_log_line("GC adapter: port %d recentred", i + 1);
        }
    } else {
        s_combo_since[i] = 0;
    }

    PADStatus* st = &s_status[i];
    st->button = b;
    st->stickX = rel8(s[3], s_origin[i][0]);
    st->stickY = rel8(s[4], s_origin[i][1]);
    st->substickX = rel8(s[5], s_origin[i][2]);
    st->substickY = rel8(s[6], s_origin[i][3]);
    st->triggerLeft = relu8(s[7], s_origin[i][4]);
    st->triggerRight = relu8(s[8], s_origin[i][5]);
    st->err = PAD_ERR_NONE;

    uint64_t snap = 0;
    for (int k = 0; k < 6; k++) {
        snap |= (uint64_t)s[3 + k] << (8 * k);
    }
    snap |= 1ull << 48;
    if (s[0] & GC_SLOT_WIRELESS) {
        snap |= 1ull << 49;
    }
    atomic_store_explicit(&s_raw[i], snap, memory_order_relaxed);

    /* Port 1 is merged with keyboard/touch by pc_keyboard_apply (it owns
     * that slot); the other three go straight to aurora. */
    if (i != 0) {
        PADSetVirtualStatus((u32)i, st);
    }
}

static void update_rumble(const uint8_t* slots) {
    /* Adapter motor byte: 0 stop, 1 rumble, 2 brake. Game state last_status:
     * 0 hard stop, 1 stop, 2 rumble. Only a wired pad on a slot with the
     * second USB cable powered can rumble. */
    static const uint8_t k_motor[3] = {2, 0, 1};
    bool changed = false;
    for (int i = 0; i < GC_SLOTS; i++) {
        const uint8_t* s = slots + 9 * i;
        uint8_t v = 0;
        if (s_present[i] && (s[0] & GC_SLOT_RUMBLE_POWER) && !(s[0] & GC_SLOT_WIRELESS) &&
            HSD_Rumble_804C22E0[i].last_status < 3)
        {
            v = k_motor[HSD_Rumble_804C22E0[i].last_status];
        }
        if (s_rumble[1 + i] != v) {
            s_rumble[1 + i] = v;
            changed = true;
        }
    }
    if (changed && SDL_hid_write(s_dev, s_rumble, sizeof(s_rumble)) < 0) {
        pc_log_line("GC adapter: rumble write failed: %s", SDL_GetError());
    }
}

void pc_gcadapter_poll(void) {
    if (!s_enabled) {
        return;
    }
    if (s_dev == NULL) {
        /* Hot-plug: look again once a second. */
        if (++s_retry_ms >= 1000) {
            s_retry_ms = 0;
            try_open();
        }
        return;
    }

    static uint8_t last[GC_REPORT];
    uint8_t pkt[64];
    int n;
    bool got = false;
    while ((n = SDL_hid_read_timeout(s_dev, pkt, sizeof(pkt), 0)) > 0) {
        if (n >= GC_REPORT && pkt[0] == 0x21) {
            memcpy(last, pkt, GC_REPORT);
            got = true;
            atomic_fetch_add_explicit(&s_reports, 1, memory_order_relaxed);
        }
    }
    if (n < 0) {
        close_dev("adapter removed");
        return;
    }
    if (got) {
        const uint64_t now = SDL_GetTicksNS();
        for (int i = 0; i < GC_SLOTS; i++) {
            parse_slot(i, last + 1 + 9 * i, now);
        }
        update_rumble(last + 1);
    }
}

bool pc_gcadapter_status(int port, PADStatus* out) {
    if (port < 0 || port >= GC_SLOTS || !s_present[port]) {
        return false;
    }
    *out = s_status[port];
    return true;
}

bool pc_gcadapter_raw(int port, uint8_t raw[6], bool* wireless) {
    if (port < 0 || port >= GC_SLOTS) {
        return false;
    }
    const uint64_t snap = atomic_load_explicit(&s_raw[port], memory_order_relaxed);
    if (!(snap & (1ull << 48))) {
        return false;
    }
    for (int k = 0; k < 6; k++) {
        raw[k] = (uint8_t)(snap >> (8 * k));
    }
    if (wireless != NULL) {
        *wireless = (snap & (1ull << 49)) != 0;
    }
    return true;
}

uint64_t pc_gcadapter_report_count(void) {
    return atomic_load_explicit(&s_reports, memory_order_relaxed);
}
