#include "lbarchive.h"
#include <stdlib.h>

#include <stdarg.h>
#include <string.h>

#include "lbdvd.h"
#include "lbfile.h"
#include "lbheap.h"
#include <dolphin/os.h>
#include <sysdolphin/baselib/archive.h>
#include <sysdolphin/baselib/debug.h>
#ifdef TARGET_PC
#include "pc/region.h"
#endif

#ifdef MUST_MATCH
#pragma push
#pragma dont_inline on
#endif
void lbArchive_InitializeDAT(HSD_Archive* archive, void* data, size_t length)
{
    const char* symbol;
    int i = 0;

    if (HSD_ArchiveParse(archive, data, length) == -1) {
        OSReport("HSD_ArchiveParse error!\n");
        HSD_ASSERT(73, 0);
    }

    while (true) {
        symbol = HSD_ArchiveGetExtern(archive, i++);
        if (symbol != NULL) {
            HSD_ArchiveLocateExtern(archive, symbol, NULL);
        }
        if (symbol == NULL) {
            return;
        }
    }
}
#ifdef MUST_MATCH
#pragma pop
#endif

void lbArchive_LoadSections(HSD_Archive* archive, void** symbol, ...)
{
    const char* symbol_name;
    va_list symbols;

    va_start(symbols, symbol);
    for (; symbol != NULL && (uint32_t)(uintptr_t)symbol != 0; symbol = va_arg(symbols, void**)) {
        symbol_name = va_arg(symbols, const char*);
        *symbol = NULL;
        *symbol = HSD_ArchiveGetPublicAddress(archive, symbol_name);
        if (*symbol == NULL) {
            OSReport("Cannot find symbol %s.\n", symbol_name);
        }
    }
    va_end(symbols);
}

/* Validate a freshly read archive against its own DAT header.
 *
 * On PC the DVD read completes on an aurora worker thread, so a short or
 * failed read yields a plausible-looking buffer whose symbol lookup then
 * returns NULL. The failure only surfaces much later: gmregclear.c:1056
 * merely reports a NULL section and proceeds, panicking at :843 when the
 * joint pointer it needed is a zero disc slot. Reporting here names the file
 * and the mismatch at the point it happens. Archive loads are rare, so this
 * costs nothing measurable. */
static void pc_archive_check(const char* filename, const void* data,
                             size_t length)
{
    const u32* hdr = data;
    u32 file_size, data_size, nb_reloc, nb_public;

    if (data == NULL) {
        OSReport("ARCHIVE %s: ALLOCATION FAILED (needed %u bytes, read %u)\n",
                 filename, (unsigned) OSRoundUp32B(lbFileGetSize(filename)),
                 (unsigned) length);
        return;
    }
    if (length < 0x20) {
        OSReport("ARCHIVE %s: read %u bytes, too short for a DAT header\n",
                 filename, (unsigned) length);
        return;
    }

    file_size = __builtin_bswap32(hdr[0]);
    data_size = __builtin_bswap32(hdr[1]);
    nb_reloc = __builtin_bswap32(hdr[2]);
    nb_public = __builtin_bswap32(hdr[3]);

    if (getenv("MELEE_ARCHIVE_LOG") != NULL) {
        OSReport("ARCHIVE ok %s: length=%u header=%u public=%u\n", filename,
                 (unsigned) length, (unsigned) file_size,
                 (unsigned) nb_public);
    }
    if (file_size != (u32) length) {
        OSReport("ARCHIVE %s: header says %u bytes, read %u\n", filename,
                 (unsigned) file_size, (unsigned) length);
    }
    if (data_size + 0x20 > (u32) length || nb_reloc > (u32) length / 4 ||
        nb_public > (u32) length / 8)
    {
        OSReport("ARCHIVE %s: implausible header (data=%u reloc=%u public=%u,"
                 " length=%u)\n",
                 filename, (unsigned) data_size, (unsigned) nb_reloc,
                 (unsigned) nb_public, (unsigned) length);
    }
}

static inline HSD_Archive* lbArchive_LoadArchive_inline(const char* filename)
{
    HSD_Archive* archive;
    void* data;
    size_t length;

    data = lbHeap_80015BD0(0, OSRoundUp32B(lbFileGetSize(filename)));
    archive = lbHeap_80015BD0(0, sizeof(HSD_Archive));
    lbFile_8001668C(filename, data, &length);
    pc_archive_check(filename, data, length);
    lbArchive_InitializeDAT(archive, data, length);
    return archive;
}

HSD_Archive* lbArchive_LoadArchive(const char* filename)
{
    return lbArchive_LoadArchive_inline(filename);
}

static inline void lbArchive_vLoadSectionsFatal(HSD_Archive* archive,
                                                void** symbol, va_list symbols)
{
    const char* symbol_name;

    for (; symbol != NULL && (uint32_t)(uintptr_t)symbol != 0; symbol = va_arg(symbols, void**)) {
        symbol_name = va_arg(symbols, const char*);
        *symbol = NULL;
        *symbol = HSD_ArchiveGetPublicAddress(archive, symbol_name);
#ifdef TARGET_PC
        if (*symbol == NULL) {
            *symbol = (void*) pc_region_missing_symbol(symbol_name);
        }
#endif
        if (*symbol == NULL) {
            OSReport("Cannot find symbol %s.\n", symbol_name);
            HSD_ASSERT(112, 0);
        }
    }
}

static inline void lbArchive_vLoadSections(HSD_Archive* archive, void** symbol,
                                           va_list symbols)
{
    const char* symbol_name;

    for (; symbol != NULL && (uint32_t)(uintptr_t)symbol != 0; symbol = va_arg(symbols, void**)) {
        symbol_name = va_arg(symbols, const char*);
        *symbol = NULL;
        *symbol = HSD_ArchiveGetPublicAddress(archive, symbol_name);
        if (*symbol == NULL) {
            OSReport("Cannot find symbol %s.\n", symbol_name);
        }
    }
}

HSD_Archive* lbArchive_LoadSymbols(const char* filename, void* symbols, ...)
{
    va_list sections;
    HSD_Archive* archive;
    void* data;
    size_t length;
    u8 _[8];

    va_start(sections, symbols);

    data = lbHeap_80015BD0(0, OSRoundUp32B(lbFileGetSize(filename)));
    archive = lbHeap_80015BD0(0, sizeof(HSD_Archive));
    lbFile_8001668C(filename, data, &length);
    pc_archive_check(filename, data, length);
    lbArchive_InitializeDAT(archive, data, length);
    lbArchive_vLoadSectionsFatal(archive, symbols, sections);

    va_end(sections);
    return archive;
}

HSD_Archive* lbArchive_80016DBC(const char* filename, void* symbols, ...)
{
    va_list sections;
    HSD_Archive* archive;
    void* data;
    size_t length;
    u8 _[8];

    va_start(sections, symbols);

    data = lbHeap_80015BD0(0, OSRoundUp32B(lbFileGetSize(filename)));
    archive = lbHeap_80015BD0(0, sizeof(HSD_Archive));
    lbFile_8001668C(filename, data, &length);
    pc_archive_check(filename, data, length);
    lbArchive_InitializeDAT(archive, data, length);
    lbArchive_vLoadSections(archive, symbols, sections);

    va_end(sections);
    return archive;
}

void lbArchive_80016EFC(HSD_Archive* archive)
{
    HSD_ASSERT(0xFC, archive);
    HSD_ASSERT(0xFD, archive->flags & HSD_ARCHIVE_DONT_FREE);
    lbHeap_80015CA8(0, archive->data - sizeof(archive->header));
    lbHeap_80015CA8(0, archive);
}

bool lbArchive_80016F80(HSD_Archive** archive, const char* filename)
{
    void* data;
    size_t length;
    HSD_Archive* var_r3;
    bool result;
    u8 _[8];

    var_r3 = lbDvd_8001819C(filename);
    if (var_r3 != NULL) {
        result = true;
    } else {
        HSD_Archive* tmp;
        data = lbHeap_80015BD0(0, OSRoundUp32B(lbFileGetSize(filename)));
        tmp = lbHeap_80015BD0(0, sizeof(HSD_Archive));
        lbFile_8001668C(filename, data, &length);
        lbArchive_InitializeDAT(tmp, data, length);
        var_r3 = tmp;
        result = false;
    }
    if (archive != NULL) {
        *archive = var_r3;
    }
    return result;
}

bool lbArchive_80017040(HSD_Archive** dst, const char* filename, void* symbols,
                        ...)
{
    void* tmp;
    HSD_Archive* archive2;
    HSD_Archive* archive;
    bool preloaded;
    va_list args;

    va_start(args, symbols);

    archive = lbDvd_8001819C(filename);
    if (archive != NULL) {
        preloaded = true;
    } else {
        // Inlined lbArchive_LoadArchive
        {
            void* data;
            size_t length;
            u32 pad;
            u32 pad2;
            data = lbHeap_80015BD0(0, OSRoundUp32B(lbFileGetSize(filename)));
            tmp = data;
            archive2 = lbHeap_80015BD0(0, sizeof(HSD_Archive));
            lbFile_8001668C(filename, tmp, &length);
            lbArchive_InitializeDAT(archive2, tmp, length);
            archive = archive2;
        }
        preloaded = false;
    }

    lbArchive_vLoadSectionsFatal(archive, symbols, args);

    va_end(args);

    if (dst != NULL) {
        *dst = archive;
    }
    return preloaded;
}

bool lbArchive_800171CC(HSD_Archive** dst, const char* filename, void* symbols,
                        ...)
{
    void* tmp;
    HSD_Archive* archive2;
    HSD_Archive* archive;
    bool preloaded;
    va_list args;

    va_start(args, symbols);

    archive = lbDvd_8001819C(filename);
    if (archive != NULL) {
        preloaded = true;
    } else {
        // Inlined lbArchive_LoadArchive
        {
            void* data;
            size_t length;
            u32 pad;
            u32 pad2;
            data = lbHeap_80015BD0(0, OSRoundUp32B(lbFileGetSize(filename)));
            tmp = data;
            archive2 = lbHeap_80015BD0(0, sizeof(HSD_Archive));
            lbFile_8001668C(filename, tmp, &length);
            lbArchive_InitializeDAT(archive2, tmp, length);
            archive = archive2;
        }
        preloaded = false;
    }

    lbArchive_vLoadSections(archive, symbols, args);

    va_end(args);

    if (dst != NULL) {
        *dst = archive;
    }
    return preloaded;
}

static inline void Locate(HSD_Archive* archive, intptr_t base_addr)
{
    u32 i;
    DiscU32* ptr;

    /* Slots are big-endian 32-bit host addresses (see pc/disc.h); base_addr
     * is a delta between two copies of the same image, so u32 wrap is fine. */
    for (i = 0; i < archive->header.nb_reloc; i++) {
        ptr = (DiscU32*) (archive->data + archive->reloc_info[i].offset);
        ptr->v += (u32) base_addr;
    }
}

int lbArchiveRelocate(HSD_Archive* archive, u8* src, size_t file_size,
                      intptr_t base_addr)
{
    size_t file_offset;

    if (archive == NULL) {
        return -1;
    }
    memset(archive, 0, sizeof(HSD_Archive));
    archive->flags |= 1;
    memcpy(archive, src, sizeof(HSD_ArchiveHeader));

    if (archive->header.file_size != file_size) {
        OSReport("lbArchiveRelocate: byte-order mismatch! "
                 "Please check data format %x %x\n",
                 archive->header.file_size, file_size);
        return -1;
    }

    file_offset = sizeof(HSD_ArchiveHeader);
    if (archive->header.data_size != 0) {
        archive->data = src + file_offset;
        file_offset = archive->header.data_size + sizeof(HSD_ArchiveHeader);
    }
    if (archive->header.nb_reloc != 0) {
        archive->reloc_info = (HSD_ArchiveRelocationInfo*) (src + file_offset);
        file_offset +=
            archive->header.nb_reloc * sizeof(HSD_ArchiveRelocationInfo);
    }
    if (archive->header.nb_public != 0) {
        archive->public_info = (HSD_ArchivePublicInfo*) (src + file_offset);
        file_offset +=
            archive->header.nb_public * sizeof(HSD_ArchivePublicInfo);
    }
    if (archive->header.nb_extern != 0) {
        archive->extern_info = (HSD_ArchiveExternInfo*) (src + file_offset);
        file_offset +=
            archive->header.nb_extern * sizeof(HSD_ArchiveExternInfo);
    }
    if (file_offset < archive->header.file_size) {
        archive->symbols = (char*) (src + file_offset);
    }

    Locate(archive, base_addr);

    return 0;
}
