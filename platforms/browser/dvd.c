/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <aurora/dvd.h>
#include <dolphin/dvd.h>
#include <emscripten.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
// clang-format off
EM_JS(int, browser_disc_read, (void* dst,unsigned offset,unsigned size), {
 // A suspended miss may be a cache hit when Asyncify replays this call.
 if(Asyncify.state===Asyncify.State.Rewinding)return Asyncify.handleAsync(async()=>0);
 const copy=bytes=>{HEAPU8.set(bytes,dst);return bytes.length;};
 const file=Module.discFile;
 if(!file||offset+size>file.size)return -1;
 const value=Module.readDisc ? Module.readDisc(offset,size) : file.slice(offset,offset+size).arrayBuffer().then(b=>new Uint8Array(b));
 if(value instanceof Uint8Array)return copy(value);
 return Asyncify.handleAsync(async()=>{try{return copy(await value);}catch(e){console.error('disc read',e);return -1;}});
});
// clang-format on
typedef struct DiscCompletion {
    struct DiscCompletion* next;
    DVDFileInfo* file;
    DVDCallback callback;
    int result;
} DiscCompletion;
static DiscCompletion *completion_head, *completion_tail;
void browser_disc_deliver(void) {
    static int delivering;
    if (delivering)
        return;
    delivering = 1;
    DiscCompletion* end = completion_tail;
    while (completion_head) {
        DiscCompletion* c = completion_head;
        completion_head = c->next;
        if (!completion_head)
            completion_tail = NULL;
        c->file->cb.state = c->result < 0 ? DVD_STATE_FATAL_ERROR : DVD_STATE_END;
        if (c->callback)
            c->callback(c->result, c->file);
        int last = c == end;
        free(c);
        if (last)
            break;
    }
    delivering = 0;
}
static uint32_t be32(const unsigned char* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static unsigned char *fst, *dol;
static unsigned fst_size, dol_size, entries;
static DVDDiskID disc_id;
static char cwd[1024] = "/";
static unsigned field(unsigned n, unsigned word) {
    return be32(fst + n * 12 + word * 4);
}
static int isdir(unsigned n) {
    return field(n, 0) >> 24;
}
static const char* name(unsigned n) {
    return (char*)fst + entries * 12 + (field(n, 0) & 0xffffff);
}
bool aurora_dvd_open(const char* path) {
    (void)path;
    unsigned char h[0x440];
    if (browser_disc_read(h, 0, sizeof(h)) != sizeof(h) || memcmp(h, "GALE01", 6) || h[7] != 2)
        return false;
    memcpy(&disc_id, h, 32);
    unsigned d = be32(h + 0x420), f = be32(h + 0x424);
    fst_size = be32(h + 0x428);
    if (f <= d || f - d > 8 * 1024 * 1024 || fst_size > 8 * 1024 * 1024)
        return false;
    dol_size = f - d;
    dol = malloc(dol_size);
    fst = malloc(fst_size);
    if (!dol || !fst)
        return false;
    if (browser_disc_read(dol, d, dol_size) != dol_size ||
        browser_disc_read(fst, f, fst_size) != fst_size)
        return false;
    entries = field(0, 2);
    return entries > 0 && entries * 12 < fst_size;
}
void aurora_dvd_close(void) {
    free(fst);
    free(dol);
    fst = dol = NULL;
}
void DVDInit(void) {}
void DVDReset(void) {}
int DVDResetRequired(void) {
    return 0;
}
void* DVDGetFSTLocation(void) {
    return fst;
}
const u8* DVDGetDOLLocation(s32* n) {
    *n = dol_size;
    return dol;
}
DVDDiskID* DVDGetCurrentDiskID(void) {
    return &disc_id;
}
BOOL DVDCheckDisk(void) {
    return fst != NULL;
}
s32 DVDGetDriveStatus(void) {
    browser_disc_deliver();
    extern void browser_arq_deliver(void);
    browser_arq_deliver();
    extern void pc_audio_pump(void);
    pc_audio_pump();
    extern void browser_yield(void);
    browser_yield();
    return DVD_STATE_END;
}
s32 DVDGetCommandBlockStatus(const DVDCommandBlock* b) {
    return b->state;
}
BOOL DVDSetAutoInvalidation(BOOL v) {
    return v;
}
int DVDSetAutoFatalMessaging(BOOL v) {
    return v;
}
void DVDPause(void) {}
void DVDResume(void) {}
/* aurora/dvd.h surface used by src/pc outside the DVD API proper. */
int aurora_dvd_inflight(void) {
    /* Reads finish inside the call; what can still be pending is a completion
     * callback queued for the next pc_os_run_alarms. */
    int n = 0;
    for (const DiscCompletion* c = completion_head; c; c = c->next)
        n++;
    return n;
}
s32 aurora_dvd_base_entry_count(void) {
    return (s32)entries;
}
void aurora_dvd_set_locale_extension(const char* ext) {
    (void)ext; /* GALE01 only */
}
s32 DVDConvertPathToEntrynum(const char* path) {
    if (!fst || !path)
        return -1;
    char full[2048];
    snprintf(full, sizeof(full), "%s%s", path[0] == '/' ? "" : cwd, path);
    unsigned current = 0;
    char* save = NULL;
    for (char* t = strtok_r(full, "/", &save); t; t = strtok_r(NULL, "/", &save)) {
        if (!strcmp(t, "."))
            continue;
        if (!strcmp(t, "..")) {
            current = field(current, 1);
            continue;
        }
        unsigned i = current + 1, end = field(current, 2);
        for (; i < end;) {
            if (!strcmp(name(i), t))
                break;
            i = isdir(i) ? field(i, 2) : i + 1;
        }
        if (i == end)
            return -1;
        current = i;
    }
    return current;
}
BOOL DVDConvertEntrynumToPath(s32 n, char* out, u32 cap) {
    if (n < 0 || (unsigned)n >= entries || !cap)
        return false;
    if (!n) {
        snprintf(out, cap, "/");
        return true;
    }
    unsigned parent = 0;
    for (unsigned i = 1; i < (unsigned)n; i++)
        if (isdir(i) && field(i, 2) > (unsigned)n)
            parent = i;
    char prefix[1024];
    DVDConvertEntrynumToPath(parent, prefix, sizeof(prefix));
    return snprintf(out, cap, "%s%s%s", prefix, name(n), isdir(n) ? "/" : "") < (int)cap;
}
BOOL DVDGetCurrentDir(char* p, u32 n) {
    return snprintf(p, n, "%s", cwd) < (int)n;
}
BOOL DVDChangeDir(const char* p) {
    int n = DVDConvertPathToEntrynum(p);
    return n >= 0 && isdir(n) && DVDConvertEntrynumToPath(n, cwd, sizeof(cwd));
}
static FILE* open_overlay(unsigned start) {
    if (!fst)
        return NULL;
    for (unsigned n = 1; n < entries; n++)
        if (!isdir(n) && field(n, 1) == start) {
            char path[1100] = "/mod";
            if (!DVDConvertEntrynumToPath(n, path + 4, sizeof(path) - 4))
                return NULL;
            return fopen(path, "rb");
        }
    return NULL;
}
BOOL DVDFastOpen(s32 n, DVDFileInfo* f) {
    if (n < 0 || (unsigned)n >= entries || isdir(n))
        return false;
    memset(f, 0, sizeof(*f));
    f->startAddr = field(n, 1);
    f->length = field(n, 2);
    FILE* overlay = open_overlay(f->startAddr);
    if (overlay) {
        fseek(overlay, 0, SEEK_END);
        f->length = ftell(overlay);
        fclose(overlay);
    }
    return true;
}
BOOL DVDOpen(const char* p, DVDFileInfo* f) {
    return DVDFastOpen(DVDConvertPathToEntrynum(p), f);
}
BOOL DVDClose(DVDFileInfo* f) {
    f->cb.state = DVD_STATE_END;
    return true;
}
s32 DVDReadPrio(DVDFileInfo* f, void* p, s32 n, s32 off, s32 prio) {
    (void)prio;
    if (n < 0 || off < 0 || (unsigned)off > f->length)
        return -1;
    unsigned actual = n;
    if (actual > f->length - off)
        actual = f->length - off;
    FILE* overlay = open_overlay(f->startAddr);
    int got;
    if (overlay) {
        fseek(overlay, off, SEEK_SET);
        got = fread(p, 1, actual, overlay);
        fclose(overlay);
    } else
        got = browser_disc_read(p, f->startAddr + off, actual);
    if (got < 0 || (unsigned)got != actual)
        return -1;
    if (actual < (unsigned)n)
        memset((char*)p + actual, 0, n - actual);
    f->cb.transferredSize = n;
    f->cb.state = DVD_STATE_END;
    return n;
}
BOOL DVDReadAsyncPrio(DVDFileInfo* f, void* p, s32 n, s32 o, DVDCallback cb, s32 prio) {
    int r = DVDReadPrio(f, p, n, o, prio);
    DiscCompletion* c = malloc(sizeof(*c));
    if (!c)
        abort();
    *c = (DiscCompletion){NULL, f, cb, r};
    if (completion_tail)
        completion_tail->next = c;
    else
        completion_head = c;
    completion_tail = c;
    f->cb.state = DVD_STATE_BUSY;
    return r >= 0;
}
s32 DVDGetFileInfoStatus(const DVDFileInfo* f) {
    return f->cb.state;
}
s32 DVDGetTransferredSize(DVDFileInfo* f) {
    return f->cb.transferredSize;
}
int DVDReadAbsAsyncPrio(DVDCommandBlock* b, void* p, s32 n, s32 off, DVDCBCallback cb, s32 prio) {
    (void)prio;
    int r = browser_disc_read(p, off, n);
    b->state = r < 0 ? DVD_STATE_FATAL_ERROR : DVD_STATE_END;
    b->transferredSize = r < 0 ? 0 : r;
    if (cb)
        cb(r, b);
    return r >= 0;
}
int DVDReadAbsAsyncForBS(DVDCommandBlock* b, void* p, s32 n, s32 off, DVDCBCallback cb) {
    return DVDReadAbsAsyncPrio(b, p, n, off, cb, 2);
}
int DVDSeekAbsAsyncPrio(DVDCommandBlock* b, s32 o, DVDCBCallback cb, s32 prio) {
    (void)o;
    (void)prio;
    b->state = 0;
    if (cb)
        cb(0, b);
    return 1;
}
int DVDReadDiskID(DVDCommandBlock* b, DVDDiskID* d, DVDCBCallback cb) {
    memcpy(d, &disc_id, sizeof(*d));
    b->state = 0;
    if (cb)
        cb(0, b);
    return 1;
}
s32 DVDSeekPrio(DVDFileInfo* f, s32 o, s32 prio) {
    (void)prio;
    return o >= 0 && (unsigned)o <= f->length ? 0 : -1;
}
int DVDSeekAsyncPrio(DVDFileInfo* f, s32 o, DVDCallback cb, s32 prio) {
    int r = DVDSeekPrio(f, o, prio);
    if (cb)
        cb(r, f);
    return r == 0;
}
s32 DVDCancel(volatile DVDCommandBlock* b) {
    b->state = DVD_STATE_CANCELED;
    return 0;
}
int DVDCancelAsync(DVDCommandBlock* b, DVDCBCallback cb) {
    DVDCancel(b);
    if (cb)
        cb(0, b);
    return 1;
}
s32 DVDCancelAll(void) {
    return 0;
}
int DVDCancelAllAsync(DVDCBCallback cb) {
    if (cb)
        cb(0, NULL);
    return 1;
}
BOOL DVDFastOpenDir(s32 n, DVDDir* d) {
    if (n < 0 || (unsigned)n >= entries || !isdir(n))
        return false;
    d->entryNum = n;
    d->location = n + 1;
    d->next = field(n, 2);
    return true;
}
int DVDOpenDir(const char* p, DVDDir* d) {
    return DVDFastOpenDir(DVDConvertPathToEntrynum(p), d);
}
int DVDReadDir(DVDDir* d, DVDDirEntry* e) {
    if (d->location >= d->next)
        return 0;
    unsigned n = d->location;
    e->entryNum = n;
    e->isDir = isdir(n);
    e->name = (char*)name(n);
    d->location = e->isDir ? field(n, 2) : n + 1;
    return 1;
}
int DVDCloseDir(DVDDir* d) {
    (void)d;
    return 1;
}
void DVDRewindDir(DVDDir* d) {
    d->location = d->entryNum + 1;
}
int DVDCompareDiskID(const DVDDiskID* a, const DVDDiskID* b) {
    return !memcmp(a, b, 8);
}
