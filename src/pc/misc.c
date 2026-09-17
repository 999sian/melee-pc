/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Leftover SDK / runtime symbols with no aurora equivalent. */
#include <dolphin/gx.h>
#include <dolphin/mcc.h>
#include <dolphin/os.h>
#include <dolphin/pad.h>

#include <stdint.h>
#include <string.h>

#if defined(__APPLE__)
#include <math.h>
void sincosf(float x, float* s, float* c) {
    __sincosf(x, s, c);
}
void sincos(double x, double* s, double* c) {
    __sincos(x, s, c);
}
#endif

/* Stack bounds, used only by the debug stack-usage report. */
unsigned char _stack_end[1];
unsigned char _stack_addr[1];

/* MSL: float.c (+INF / +MAX bit patterns) */
int MSL_TrigF_80400770[] = {0x7FFFFFFF};
int MSL_TrigF_80400774[] = {0x7F800000};

/* Runtime: double -> unsigned long long */
u64 __cvt_dbl_usll(double x) {
    if (x <= 0.0) {
        return 0;
    }
    if (x >= 18446744073709551616.0) {
        return UINT64_MAX;
    }
    return (u64)x;
}

/* Progressive-scan render mode (GXFrameBuf.c) */
GXRenderModeObj GXNtsc480Prog = {
    VI_TVMODE_NTSC_PROG,
    640,
    480,
    480,
    40,
    0,
    640,
    480,
    VI_XFBMODE_SF,
    0,
    0,
    {6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6},
    {0, 0, 21, 22, 21, 0, 0},
};

void GXSetCopyClamp(GXFBClamp clamp) {
    (void)clamp;
}

static u32 s_sound_mode = 1; /* stereo */

u32 OSGetSoundMode(void) {
    return s_sound_mode;
}

void OSSetSoundMode(u32 mode) {
    s_sound_mode = mode;
}

BOOL OSGetResetSwitchState(void) {
    return 0;
}

void PADSetSamplingRate(u32 msec) {
    (void)msec;
}

/* MCC / FIO: host-PC debug communication over EXI. Never connected. */
int MCCInit(enum MCC_EXI exiChannel, u8 timeout, MCC_CBSysEvent cb) {
    (void)exiChannel;
    (void)timeout;
    (void)cb;
    return 0;
}
void MCCExit(void) {}
int MCCEnumDevices(MCC_CBEnumDevices cb) {
    (void)cb;
    return 0;
}
u8 MCCGetFreeBlocks(enum MCC_MODE mode) {
    (void)mode;
    return 0;
}
u8 MCCGetLastError(void) {
    return 0;
}
int MCCGetConnectionStatus(enum MCC_CHANNEL chID, enum MCC_CONNECT* connect) {
    (void)chID;
    (void)connect;
    return 0;
}
int MCCOpen(enum MCC_CHANNEL chID, u8 blockSize, MCC_CBEvent cb) {
    (void)chID;
    (void)blockSize;
    (void)cb;
    return 0;
}
int MCCClose(enum MCC_CHANNEL chID) {
    (void)chID;
    return 0;
}
int MCCNotify(enum MCC_CHANNEL chID, u32 data) {
    (void)chID;
    (void)data;
    return 0;
}
int MCCRead(enum MCC_CHANNEL chID, u32 offset, void* data, long size,
    enum MCC_SYNC_STATE async) {  // NOLINT
    (void)chID;
    (void)offset;
    (void)data;
    (void)size;
    (void)async;
    return 0;
}
int MCCWrite(enum MCC_CHANNEL chID, u32 offset, void* data, long size,
    enum MCC_SYNC_STATE async) {  // NOLINT
    (void)chID;
    (void)offset;
    (void)data;
    (void)size;
    (void)async;
    return 0;
}
int MCCStreamOpen(enum MCC_CHANNEL chID, u8 blockSize) {
    (void)chID;
    (void)blockSize;
    return 0;
}
int FIOInit(enum MCC_EXI exiChannel, enum MCC_CHANNEL chID, u8 blockSize) {
    (void)exiChannel;
    (void)chID;
    (void)blockSize;
    return 0;
}
void FIOExit(void) {}
int FIOQuery(void) {
    return 0;
}
int FIOFopen(const char* filename, u32 mode) {
    (void)filename;
    (void)mode;
    return -1;
}
int FIOFclose(int handle) {
    (void)handle;
    return 0;
}
u32 FIOFwrite(int handle, void* data, u32 size) {
    (void)handle;
    (void)data;
    (void)size;
    return 0;
}

/* PowerPC intrinsics */
void __dcbz(void* base, u32 offset) {
    memset((u8*)base + offset, 0, 32);
}

u32 __cntlzw(u32 x) {
    return x ? (u32)__builtin_clz(x) : 32;
}
