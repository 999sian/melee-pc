#include "lbmemory.h"

#include <Runtime/platform.h>

#include <string.h>

#include <dolphin/ar.h>
#include <dolphin/os/OSAlarm.h>
#include <sysdolphin/baselib/debug.h>
#include <sysdolphin/baselib/devcom.h>

struct MemEntry {
    struct MemEntry* x0_next;
    void* x4_lo;
    void* x8_hi;
};

struct LBMgr {
    OSAlarm alarm; // 0x00
    u8* src;       // 0x28
    u8* dst;       // 0x2C
    u32 size;      // 0x30
    u32 offset;    // 0x34
    uintptr_t cb_arg; // 0x38
    HSD_DevComCallback cb;
};

struct Allocator {
    void* a_arenaLo;
    void* a_arenaHi;
    struct MemEntry x8_mem[0x83];
    Handle* free_mem;
    s32 x630_num_allocs;
    s32 x634_max_num_allocs;
    Handle x638_heap[6];
    Handle* free_heap;
    Handle* x69C;
    struct LBMgr x6A0_mgr;
    u32 x6E0;
    void* x6E4;
    void (*x6E8)(u32);
    u8 x6EC[0x6F0 - 0x6EC];
};

/* 015320 */ static void lbMemory_80015320(int, uintptr_t, void*, bool);

struct Allocator lbMemory_804318B0;
#define _p(x) (lbMemory_804318B0.x)
ASSERT_SIZE(struct MemEntry, 0xC);
ASSERT_SIZE(lbMemory_804318B0, 0x6F0);

#define PUSH_HANDLE(list, handle)                                             \
    do {                                                                      \
        handle->x0_next = *list;                                              \
        *list = handle;                                                       \
    } while (0)
#define POP_HANDLE(list, handle)                                              \
    do {                                                                      \
        handle = *list;                                                       \
        *list = handle->x0_next;                                              \
    } while (0)

Handle* lbMemory_80014E24(void* arenaLo, void* arenaHi)
{
    Handle* h;
    HSD_ASSERT(0x7B, _p(free_heap));

    if (PC_IS_ARAM_ADDR(arenaLo) && PC_IS_ARAM_ADDR(arenaHi)) {
        HSD_ASSERT(0x80, (uintptr_t) arenaLo >= (uintptr_t) _p(a_arenaLo) && (uintptr_t) arenaHi <= (uintptr_t) _p(a_arenaHi));
    }

    POP_HANDLE(&_p(free_heap), h);
    h->x0_next = NULL;
    h->x4_lo = arenaLo;
    h->x8_hi = arenaHi;
    h->xC_prev = NULL;
    return h;
}

void lbMemory_80014EEC(Handle* handle)
{
    Handle* iter;
    Handle* tmp_next;
    HSD_ASSERT(149, handle);
    for (iter = handle->xC_prev; iter != NULL;) {
        tmp_next = iter->x0_next;
        PUSH_HANDLE(&_p(free_mem), iter);
        iter = tmp_next;
        _p(x630_num_allocs) -= 1;
    }
    PUSH_HANDLE(&_p(free_heap), handle);
}

u32 lbMemory_80014F7C(Handle* h)
{
    uintptr_t r0;
    uintptr_t r4 = (uintptr_t) h->x4_lo;
    Handle* iter = (Handle*) &h->xC_prev;
    u32 sum = 0;

loop:
    iter = iter->x0_next;
    r0 = (uintptr_t) ((iter != NULL) ? iter->x4_lo : h->x8_hi);
    sum += (u32) (r0 - r4);
    if (iter != NULL) {
        r4 = (uintptr_t) iter->x4_lo + (uintptr_t) iter->x8_hi;
        goto loop;
    }
    return sum;
}

Handle* lbMemory_80014FC8(Handle* arg0, size_t size)
{
    void* lo;
    Handle* memp_kouho;
    void* end;
    uintptr_t least_leftover;
    uintptr_t leftover;
    uintptr_t available_space;
    void* start;
    Handle* iter;

    least_leftover = (uintptr_t) -1;
    HSD_ASSERT(0xCC, _p(free_mem));
    size = ((size + 0x1F) & 0xFFFFFFE0);
    start = arg0->x4_lo;
    iter = (Handle*) &arg0->xC_prev;
    memp_kouho = NULL;

    while (1) {
        end = (iter->x0_next != NULL) ? iter->x0_next->x4_lo : arg0->x8_hi;
        available_space = (uintptr_t) end - (uintptr_t) start;
        if (available_space >= size) {
            leftover = available_space - size;
            if (leftover <= least_leftover) {
                least_leftover = leftover;
                lo = start;
                memp_kouho = iter;
            }
        }
        if (iter->x0_next == NULL) {
            break;
        } else {
            iter = iter->x0_next;
            start = (u8*) iter->x4_lo + (uintptr_t) iter->x8_hi;
        }
    }
#ifdef TARGET_PC
    if (memp_kouho == NULL) {
        OSReport("lbMemory: no room for %u bytes in region %p..%p (%u free in"
                 " %s)\n",
                 (unsigned) size, arg0->x4_lo, arg0->x8_hi,
                 lbMemory_80014F7C(arg0),
                 PC_IS_ARAM_ADDR(arg0->x4_lo) ? "ARAM" : "RAM");
    }
#endif
    HSD_ASSERT(0xE9, memp_kouho);
    {
        Handle* result;
        POP_HANDLE(&_p(free_mem), result);

        result->x8_hi = (void*) size;
        result->x4_lo = lo;
        result->x0_next = memp_kouho->x0_next;
        memp_kouho->x0_next = result;

        _p(x630_num_allocs) += 1;
        if (_p(x630_num_allocs) > _p(x634_max_num_allocs)) {
            _p(x634_max_num_allocs) = _p(x630_num_allocs);
        }
        return result;
    }
}
void lbMemFreeToHeap(Handle* h, void* arg1)
{
    Handle* handle = h->xC_prev;
    Handle* r6 = (Handle*) &h->xC_prev;

    while (handle != NULL) {
        if (handle->x4_lo == arg1) {
            r6->x0_next = handle->x0_next;
            PUSH_HANDLE(&_p(free_mem), handle);
            _p(x630_num_allocs) -= 1;
            return;
        }
        r6 = handle;
        handle = handle->x0_next;
    }
    OSReport("[LbMem] Error: lbMemFreeToHeap %x.\n", arg1);
    HSD_ASSERT(283, 0);
}

static void fn_80015184(OSAlarm* alarm, OSContext* context)
{
    struct LBMgr* p;
    u32 temp_r3_2;
    u32 temp_r6;
    u32 var_r30;

    p = &_p(x6A0_mgr);
    HSD_ASSERT(0x127, p->size);
    temp_r6 = p->offset;
    temp_r3_2 = p->size - temp_r6;
    var_r30 = temp_r3_2;
    if (temp_r3_2 > 0x19000U) {
        var_r30 = 0x19000;
    }
    memcpy(p->dst + temp_r6, p->src + temp_r6, var_r30);
    p->offset = p->offset + var_r30;
    if (p->offset == p->size) {
        p->size = 0U;
        p->cb(0, p->cb_arg, 0, 0);
        return;
    }
    OSCreateAlarm(&p->alarm);
    OSSetAlarm(&p->alarm, OSMillisecondsToTicks(3), fn_80015184);
}

u32 lbMemory_8001529C(Handle* h, void (*arg1)(u32), u32 arg2)
{
    void* lo;
    Handle* iter;
    void** r7;

    _p(x6E8) = arg1;
    _p(x6E0) = arg2;
    _p(x6E4) = h->x4_lo;

    r7 = &_p(x6E4);

    for (iter = h->xC_prev; iter != NULL; iter = iter->x0_next) {
        lo = iter->x4_lo;
        if (lo != *r7) {
            lbMemory_80015320(0, (uintptr_t) iter, NULL, false);
            return 1;
        }
        *r7 = (u8*) lo + (uintptr_t) iter->x8_hi;
    }
    return 0;
}

static void start_ram_copy(uintptr_t old, uintptr_t current, u32 size, Handle* next)
{
    struct LBMgr* p = &_p(x6A0_mgr);
    int enabled = OSDisableInterrupts();

    HSD_ASSERT(0x14F, !p->size);
    p->src = (u8*) old;
    p->dst = (u8*) current;
    p->size = size;
    p->offset = 0;
    p->cb_arg = (uintptr_t) next;
    p->cb = lbMemory_80015320;
    OSRestoreInterrupts(enabled);
    OSCreateAlarm(&p->alarm);
    OSSetAlarm(&p->alarm, OSMillisecondsToTicks(3), fn_80015184);
}

static void lbMemory_80015320(int arg0, uintptr_t _handle, void* arg2,
                              bool cancelflag)
{
    void* null_or_old;
    Handle* handle = (Handle*) _handle;
    void** currentp;
    void* old;
    uintptr_t current;
    void* copy_src;
    void* loaded_old;

    currentp = &_p(x6E4);
    current = (uintptr_t) _p(x6E4);
    null_or_old = NULL;

    HSD_ASSERT(0x188, !cancelflag);

    if (handle != null_or_old) {
        loaded_old = handle->x4_lo;
        if ((old = loaded_old) != (void*) current) {
            null_or_old = old;
            handle->x4_lo = (void*) current;
            *currentp = (u8*) handle->x4_lo + (uintptr_t) handle->x8_hi;
            copy_src = null_or_old;

            if (PC_IS_ARAM_ADDR(handle->x4_lo)) {
                HSD_DevComRequest(0, (uintptr_t) copy_src, current,
                                  OSRoundUp32B((uintptr_t) handle->x8_hi), 0x1B, 1,
                                  lbMemory_80015320, handle->x0_next);
                return;
            } else {
                start_ram_copy((uintptr_t) copy_src, current,
                               OSRoundUp32B((uintptr_t) handle->x8_hi), handle->x0_next);
                return;
            }
        }

        *currentp = (u8*) old + (uintptr_t) handle->x8_hi;
        lbMemory_80015320(0, (uintptr_t) handle->x0_next, null_or_old, false);
        return;
    }

    _p(x6E8)(_p(x6E0));
}

void lbMemory_800154BC(uintptr_t* arenaLo, uintptr_t* arenaHi)
{
    *arenaLo = (uintptr_t) _p(a_arenaLo);
    *arenaHi = (uintptr_t) _p(a_arenaHi);
}

Handle* lbMemory_800154D4(void* arenaLo, void* arenaHi)
{
    _p(x69C) = lbMemory_80014E24(arenaLo, arenaHi);
    return _p(x69C);
}

void lbMemory_800155A4(void)
{
    Handle* handle = _p(x69C);
    Handle* iter;

    Handle** r5;

    HSD_ASSERT(149, handle);
    r5 = &_p(free_mem);
    for (iter = handle->xC_prev; iter != NULL;) {
        Handle* tmp_next = iter->x0_next;
        PUSH_HANDLE(r5, iter);
        iter = tmp_next;
        _p(x630_num_allocs) -= 1;
    }
    PUSH_HANDLE(&_p(free_heap), handle);
    _p(x69C) = NULL;
}

void lbMemory_8001564C(void)
{
    u32 freed_size;
    int i;

    _p(a_arenaLo) = (void*) (uintptr_t) ARAlloc(0x20);
    ARFree(&freed_size);
    _p(a_arenaHi) =
        (void*) (uintptr_t) ((ARGetSize() > 0x01000000U) ? 0x01000000U : ARGetSize());

    _p(free_mem) = (Handle*) &_p(x8_mem)[0];
    for (i = 0; i < (int) ARRAY_SIZE(_p(x8_mem)) - 1; i++) {
        _p(x8_mem)[i].x0_next = &_p(x8_mem)[i + 1];
    }
    _p(x8_mem)[i].x0_next = NULL;

    _p(x634_max_num_allocs) = 0;
    _p(x630_num_allocs) = 0;
    _p(free_heap) = &_p(x638_heap)[0];
    for (i = 0; i < (int) ARRAY_SIZE(_p(x638_heap)) - 1; i++) {
        _p(x638_heap)[i].x0_next = &_p(x638_heap)[i + 1];
    }
    _p(x638_heap)[i].x0_next = NULL;
    _p(x69C) = NULL;
    {
        void* hi = _p(a_arenaHi);
        void* lo = _p(a_arenaLo);
        lbMemory_800154D4(lo, hi);
    }
    _p(x6A0_mgr).size = 0;
}
