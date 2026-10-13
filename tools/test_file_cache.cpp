// Exercise the real preloader with a host allocation in the ARAM address range.
#include <cassert>
#include <sys/mman.h>
#include "../src/pc/file_cache.cpp"

static void* low_buffer;
static unsigned char aram[0x1000000];
extern "C" void* __real_malloc(size_t);
extern "C" void __real_free(void*);
extern "C" void* __wrap_malloc(size_t size) {
    return size == 128 ? low_buffer : __real_malloc(size);
}
extern "C" void __wrap_free(void* ptr) {
    if (ptr != low_buffer) __real_free(ptr);
}
extern "C" u8* aurora_aram_base(void) { return aram; }
extern "C" void OSReport(const char*, ...) {}
extern "C" BOOL DVDOpen(const char*, DVDFileInfo* info) {
    info->length = 86;
    return TRUE;
}
extern "C" BOOL DVDFastOpen(s32, DVDFileInfo* info) { return DVDOpen(nullptr, info); }
extern "C" BOOL DVDClose(DVDFileInfo*) { return TRUE; }
extern "C" s32 DVDReadPrio(DVDFileInfo*, void* dst, s32 len, s32, s32) {
    memset(dst, 0x5a, len);
    return len;
}
int main() {
    low_buffer = mmap(reinterpret_cast<void*>(0x800000), 4096,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(low_buffer == reinterpret_cast<void*>(0x800000));
    setenv("MELEE_FILES_DIR", "/nonexistent-melee-test", 1);
    assert(preload_single_file("test-low-address.dat", -1));
    unsigned char out[86]{};
    size_t size = 0;
    assert(pc_file_cache_get("test-low-address.dat", out, &size));
    assert(size == sizeof(out));
    for (auto byte : out) assert(byte == 0x5a);
    // Game-side ARAM offsets must still be translated.
    memset(aram + 0x1000, 0x3c, sizeof(out));
    pc_file_cache_put("test-aram.dat", reinterpret_cast<void*>(0x1000), sizeof(out));
    assert(pc_file_cache_get("test-aram.dat", out, &size));
    for (auto byte : out) assert(byte == 0x3c);
    munmap(low_buffer, 4096);
}
