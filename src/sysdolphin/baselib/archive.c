#include "archive.h"

#include <string.h>

#include <dolphin/os.h>

static inline void Locate(HSD_Archive* archive)
{
    u32 i;
    DiscU32* ptr;

    /* Pointer slots stay big-endian in the archive image and hold absolute
     * host addresses. Every slot is 32 bits wide, so an image loaded above
     * 4GB would silently truncate every pointer in it; this is the same
     * check DP_SET makes, applied to the one site that bypasses DP_SET. */
#ifdef TARGET_PC
    if (archive->header.nb_reloc != 0 && ((uint64_t) (uintptr_t) archive->data >> 32) != ((uint64_t) OSBaseAddress >> 32)) {
        pc_disc_ptr_overflow(archive->data, __FILE__, __LINE__);
    }
#endif
    for (i = 0; i < archive->header.nb_reloc; i++) {
        ptr = (DiscU32*) (archive->data + archive->reloc_info[i].offset);
        ptr->v += (u32) (uintptr_t) archive->data;
    }
}

s32 HSD_ArchiveParse(HSD_Archive* archive, u8* src, size_t file_size)
{
    u32 offset;

    if (archive == NULL) {
        return -1;
    }

    memset(archive, 0, sizeof(HSD_Archive));
    archive->flags |= 1;
    memcpy(archive, src, sizeof(HSD_ArchiveHeader));

    if (archive->header.file_size != file_size) {
        OSReport("HSD_ArchiveParse: byte-order mismatch! Please check data "
                 "format %x %x\n",
                 archive->header.file_size, file_size);
        return -1;
    }

    offset = sizeof(HSD_ArchiveHeader);
    if (archive->header.data_size != 0) { // Body Size
        archive->data = src + sizeof(HSD_ArchiveHeader);
        offset = archive->header.data_size + sizeof(HSD_ArchiveHeader);
    }
    if (archive->header.nb_reloc != 0) { // Relocation Size
        archive->reloc_info =
            (HSD_ArchiveRelocationInfo*) ((uintptr_t) src + offset);
        offset = offset +
                 archive->header.nb_reloc * sizeof(HSD_ArchiveRelocationInfo);
    }
    if (archive->header.nb_public != 0) { // Root Size
        archive->public_info =
            (HSD_ArchivePublicInfo*) ((uintptr_t) src + offset);
        offset =
            offset + archive->header.nb_public * sizeof(HSD_ArchivePublicInfo);
    }
    if (archive->header.nb_extern != 0) { // XRef Size
        archive->extern_info =
            (HSD_ArchiveExternInfo*) ((uintptr_t) src + offset);
        offset =
            offset + archive->header.nb_extern * sizeof(HSD_ArchiveExternInfo);
    }
    if (offset < archive->header.file_size) { // File Size
        archive->symbols = (char*) ((uintptr_t) src + offset);
    }

    archive->top_ptr = (void*) src;
    Locate(archive);

    return 0;
}

void* HSD_ArchiveGetPublicAddress(HSD_Archive* archive, const char* symbols)
{
    u32 i;

    for (i = 0; i < archive->header.nb_public; i++) {
        int comparison =
            strcmp(archive->symbols + archive->public_info[i].symbol, symbols);

        if (comparison == 0) {
            // If both strings are equal, we've found the node
            return archive->data + archive->public_info[i].offset;
        }
    }

    return NULL;
}

char* HSD_ArchiveGetExtern(HSD_Archive* archive, int offset)
{
    if (offset < 0 || archive->header.nb_extern <= (unsigned) offset) {
        return NULL;
    }

    return archive->symbols + archive->extern_info[offset].symbol;
}

void HSD_ArchiveLocateExtern(HSD_Archive* archive, const char* symbols,
                             void* addr)
{
    /* Archive offsets and the 0xFFFFFFFF chain terminator are 32-bit; a
     * uintptr_t -1 is 64-bit all-ones here and never equals -1U. */
    u32 next;
    u32 offset = -1;
    u32 i;

    for (i = 0; i < archive->header.nb_extern; i++) {
        int comparison =
            strcmp(symbols, archive->symbols + archive->extern_info[i].symbol);

        if (comparison == 0) {
            offset = archive->extern_info[i].offset;
            break;
        }
    }

    if (offset == -1U) {
        return;
    }

    while (offset != -1U && offset < archive->header.data_size) {
        DiscU32* slot = (DiscU32*) ((uintptr_t) archive->data + offset);
        next = slot->v;
        DP_SET(slot->v, addr);
        offset = next;
    }
}
