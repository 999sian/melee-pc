/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <aurora/dvd.h>
#include <dolphin/dvd.h>
#include <emscripten.h>
#include <stdint.h>
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
/* Only the entry points this target links are implemented: the game opens by
 * entry number and reads asynchronously (lbfile.c, devcom.c); src/pc needs the
 * DOL, the disc ID and the entry count. An unreferenced DVD call is a link
 * error (-sERROR_ON_UNDEFINED_SYMBOLS), so a future caller cannot be missed. */
void DVDInit(void) {}
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
/* The game polls this while it waits on a read, which makes it the place to
 * deliver completions and return to the event loop. */
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
    char full[1024];
    if (strlen(path) >= sizeof(full))
        return -1;
    strcpy(full, path);
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
BOOL DVDFastOpen(s32 n, DVDFileInfo* f) {
    if (n < 0 || (unsigned)n >= entries || isdir(n))
        return false;
    memset(f, 0, sizeof(*f));
    f->startAddr = field(n, 1);
    f->length = field(n, 2);
    return true;
}
BOOL DVDClose(DVDFileInfo* f) {
    f->cb.state = DVD_STATE_END;
    return true;
}
static s32 read_file(DVDFileInfo* f, void* p, s32 n, s32 off) {
    if (n < 0 || off < 0 || (unsigned)off > f->length)
        return -1;
    unsigned actual = n;
    if (actual > f->length - off)
        actual = f->length - off;
    int got = browser_disc_read(p, f->startAddr + off, actual);
    if (got < 0 || (unsigned)got != actual)
        return -1;
    if (actual < (unsigned)n)
        memset((char*)p + actual, 0, n - actual);
    f->cb.transferredSize = n;
    return n;
}
/* The read itself completes here (a cache miss suspends the wasm); only the
 * callback is deferred, to the next browser_disc_deliver. */
BOOL DVDReadAsyncPrio(DVDFileInfo* f, void* p, s32 n, s32 o, DVDCallback cb, s32 prio) {
    (void)prio;
    int r = read_file(f, p, n, o);
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
