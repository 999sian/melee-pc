#include "lbfile.h"

#include <placeholder.h>
#include <string.h>

#include "lb_0195.h"
#include "lbdvd.h"
#include "lbheap.h"
#include "lblanguage.h"
#include <dolphin/dvd.h>
#include <sysdolphin/baselib/debug.h>
#include <sysdolphin/baselib/devcom.h>

#ifdef TARGET_PC
#include "pc/file_cache.h"
#endif

static bool cancel;

static void lbFile_8001615C(int dcreq, uintptr_t args, void* buf, bool cancelflag)
{
    HSD_ASSERT(71, !cancelflag);
    cancel = true;
}

/// @todo Non-inlined function forces loop in ::lbFile_800161C4 to yield to
///       interrupts. Pragma solution likely fake.
#ifdef __MWERKS__
#pragma push
#pragma dont_inline on
#endif
static bool discIsDone(void)
{
    lb_800195D0();
    return cancel;
}
#ifdef __MWERKS__
#pragma pop
#endif

static void waitForDisc(void)
{
    do {
    } while (!discIsDone());
}

void lbFile_800161C4(int file, uintptr_t src, uintptr_t dst, size_t size,
                     int type, int pri)
{
    cancel = false;
    HSD_DevComRequest(file, src, dst, size, type, pri, lbFile_8001615C, 0);
    waitForDisc();
}

#define MAX_FILENAME_LENGTH 0x20
const int FILE_EXTENSION_LENGTH = 4; // ".usd" or ".dat"
const int MAX_BASENAME_LENGTH = MAX_FILENAME_LENGTH - FILE_EXTENSION_LENGTH;

/// append file extension (if needed)
char* lbFileGetFullName(const char* basename)
{
    static char result[MAX_FILENAME_LENGTH];
    const char* cur = basename;
    int pos = 0;

    while (*cur != '\0' && *cur != '.') {
        // no room for file extension?
        if (pos > MAX_BASENAME_LENGTH) {
            OSReport("Error : file name too long %s.", basename);
            HSD_ASSERT(0x99, NULL);
        }
        result[pos++] = *cur++;
    }
    // keep any existing file extension
    if (cur[0] != '\0' && cur[1] != '\0') {
        strcpy(result, basename);
        // otherwise, append the appropriate extension for the locale
    } else if (*cur == '.') {
        result[pos++] = '.';
        if (lbLang_IsSettingUS()) {
            strcpy(&result[pos], "usd");
        } else {
            strcpy(&result[pos], "dat");
        }
    } else {
        result[pos++] = '.';
        if (lbLang_IsSavedLanguageUS()) {
            strcpy(&result[pos], "usd");
        } else {
            strcpy(&result[pos], "dat");
        }
    }
    return result;
}

size_t lbFile_8001634C(int fileno)
{
    DVDFileInfo info;
    size_t length;
    bool intr = OSDisableInterrupts();

    if (!DVDFastOpen(fileno, &info)) {
        OSReport("Cannot open file no=%d.", fileno);
        HSD_ASSERT(0xD8, 0);
    }

    length = info.length;
    DVDClose(&info);
    OSRestoreInterrupts(intr);
    return length;
}

size_t lbFileGetSize(const char* basename)
{
    char* filename = lbFileGetFullName(basename);
#ifdef TARGET_PC
    size_t cached_sz = 0;
    if (pc_file_cache_get_size(filename, &cached_sz)) {
        return cached_sz;
    }
#endif
    int entry_num;
    entry_num = DVDConvertPathToEntrynum(filename);
    HSD_ASSERTREPORT(0xEE, entry_num != -1, "file isn't exist %s = %d\n",
                     filename, entry_num);
    return lbFile_8001634C(entry_num);
}

#define ROUND_UP_32(x) (((x) + 31) & ~31)

void lbFile_800164A4(int file, uintptr_t dst, size_t* size, int pri,
                     HSD_DevComCallback callback, void* args)
{
    int type;
    *size = lbFile_8001634C(file);
    type = !PC_IS_ARAM_ADDR(dst) ? 0x21 : 0x23;
    HSD_DevComRequest(file, 0, dst, ROUND_UP_32(*size), type, pri, callback,
                      args);
}

void lbFile_80016580(const char* basename, void* dst, size_t* size,
                     HSD_DevComCallback callback, void* args)
{
    char* filename = lbFileGetFullName(basename);
#ifdef TARGET_PC
    if (pc_file_cache_get(filename, dst, size)) {
        if (callback != NULL) {
            callback(0, (uintptr_t) args, NULL, false);
        }
        return;
    }
#endif
    int entry_num = DVDConvertPathToEntrynum(filename);
    PAD_STACK(4);

    HSD_ASSERTREPORT(0x11A, entry_num != -1, "file isn't exist %s = %d\n",
                     filename, entry_num);

    lbFile_800164A4(entry_num, (uintptr_t) dst, size, 1, callback, args);
}

void lbFile_8001668C(const char* basename, void* dst, size_t* size)
{
#ifdef TARGET_PC
    char* filename = lbFileGetFullName(basename);
    if (pc_file_cache_get(filename, dst, size)) {
        return;
    }
#endif
    cancel = false;
    lbFile_80016580(basename, dst, size, lbFile_8001615C, NULL);
    waitForDisc();
#ifdef TARGET_PC
    pc_file_cache_put(filename, dst, *size);
#endif
}

static void lbFile_80016760_inline(int heap_id, const char* basename,
                                   void** dst, size_t* size)
{
    *size = lbFileGetSize(basename);
    *dst = lbHeap_80015BD0(heap_id, ROUND_UP_32(*size));
    lbFile_80016580(basename, *dst, size, lbFile_8001615C, NULL);
    waitForDisc();
#ifdef TARGET_PC
    pc_file_cache_put(lbFileGetFullName(basename), *dst, *size);
#endif
}

void lbFile_80016760(const char* basename, void** dst, size_t* size)
{
    cancel = false;
    lbFile_80016760_inline(0, basename, dst, size);
}

bool lbFile_800168A0(int heap_id, const char* basename, void** dst,
                     size_t* size)
{
    if ((*dst = lbDvd_8001819C(basename))) {
        *size = lbFileGetSize(basename);
        return true;
    } else {
        cancel = false;
        lbFile_80016760_inline(heap_id, basename, dst, size);
        return false;
    }
}
