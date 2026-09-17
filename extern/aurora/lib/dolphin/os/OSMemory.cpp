#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include "fmt/base.h"

#include <cassert>

#include "internal.hpp"
#include <dolphin/os.h>
#include "dolphin/types.h"

#if !NDEBUG && (INTPTR_MAX > INT32_MAX)
#define GUARD_MEMORY 1
#endif

uintptr_t OSBaseAddress = 0;

void* MEM1Start;
void* MEM1End;

static void GuardGCMemory();
static void* AllocMEM1(u32 size);

void AuroraOSInitMemory() {
  if (MEM1Start != nullptr) {
    return;
  }

  u32 size = aurora::g_config.mem1Size;
  if (size == 0) {
    size = 96u * 1024 * 1024;
    aurora::g_config.mem1Size = size;
  }

  MEM1Start = AllocMEM1(size);
  MEM1End = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(MEM1Start) + size);
  OSBaseAddress = reinterpret_cast<uintptr_t>(MEM1Start);
  GuardGCMemory();
}

#if GUARD_MEMORY
static uintptr_t GetAllocationGranularity() {
#if _WIN32
  SYSTEM_INFO sysInfo;
  GetSystemInfo(&sysInfo);

  return sysInfo.dwAllocationGranularity;
#else
  // TODO: posix impl
  return 0;
#endif
}

static void TryGuardRegion(const uintptr_t start, const uintptr_t end, char const* const name) {
#if _WIN32
  assert(start != 0);
  const auto addr = VirtualAlloc(
      reinterpret_cast<LPVOID>(start),
      end - start,
      MEM_RESERVE,
      PAGE_NOACCESS);

  if (addr == nullptr) {
    Log.debug("Unable to guard memory region: {}", name);
  } else {
    assert(addr == reinterpret_cast<LPVOID>(start));
    Log.debug("Successfully guarded memory range: {:08X}-{:08X} ({})", start, end, name);
  }
#else
  // TODO: posix impl
#endif
}

static void GuardGCMemory() {
  // Reserve the normal GC/Wii memory map so accesses are 100% guaranteed to fail,
  // skipping any range that overlaps with the allocated MEM1 arena.
  const uintptr_t mem1 = reinterpret_cast<uintptr_t>(MEM1Start);
  const uintptr_t mem1End = reinterpret_cast<uintptr_t>(MEM1End);

  auto tryGuard = [&](uintptr_t start, uintptr_t end, const char* name) {
    if (start < mem1End && end > mem1) {
      return;
    }
    TryGuardRegion(start, end, name);
  };

  tryGuard(0x00000000 + GetAllocationGranularity(), 0x017fffff, "MEM1 Physical");
  tryGuard(0x80000000, 0x817fffff, "MEM1 Logical (cached)");
  tryGuard(0xC0000000, 0xC17fffff, "MEM1 Logical (uncached)");
  tryGuard(0x10000000, 0x13FFFFFF, "MEM2 Physical");
  tryGuard(0x90000000, 0x93FFFFFF, "MEM2 Logical (cached)");
  tryGuard(0xD0000000, 0xD3FFFFFF, "MEM2 Logical (uncached)");
  tryGuard(0x08000000, 0x08300000, "EFB Physical");
  tryGuard(0xC8000000, 0xC8300000, "EFB Logical");
  tryGuard(0x0D000000, 0x0D008000, "Hollywood HW registers Physical");
  tryGuard(0xCD000000, 0xCD008000, "Hollywood HW registers Logical");
  tryGuard(0x0C000000, 0x0C008020, "Broadway/GC HW registers Physical");
  tryGuard(0xCC000000, 0xCC008020, "Broadway/GC HW registers Logical");
  tryGuard(0xe0000000, 0xe0003fff, "GC L2 cache");
  tryGuard(0xfff00000, 0xffffffff, "GC IPL");
}
#else
static void GuardGCMemory() { }
#endif

#if defined(_WIN32)
static void* AllocMEM1(u32 size) {
  // Try preferred address first, then try candidates strictly < 4GB.
  // Pointers in disc files and relocations require addresses to fit in 32 bits.
  static const uintptr_t candidates[] = {
    0x80000000ULL,
    0x70000000ULL,
    0x60000000ULL,
    0x50000000ULL,
    0x40000000ULL,
    0x30000000ULL,
    0x20000000ULL,
    0x90000000ULL,
    0xA0000000ULL,
    0xB0000000ULL,
  };

  void* p = nullptr;
  for (uintptr_t addr : candidates) {
    if (addr + size <= 0x100000000ULL) {
      p = VirtualAlloc(reinterpret_cast<void*>(addr), size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
      if (p) break;
    }
  }

  // Try VirtualAlloc2 with 4GB limit if available (Windows 10 1803+)
  // HighestEndingAddress is inclusive and must be aligned to system allocation granularity (64KB) minus 1.
  if (!p) {
    typedef PVOID (WINAPI *VirtualAlloc2_t)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
    HMODULE kernelBase = GetModuleHandleA("kernelbase.dll");
    if (!kernelBase) kernelBase = GetModuleHandleA("kernel32.dll");
    if (kernelBase) {
      auto pVirtualAlloc2 = reinterpret_cast<VirtualAlloc2_t>(GetProcAddress(kernelBase, "VirtualAlloc2"));
      if (pVirtualAlloc2) {
        MEM_ADDRESS_REQUIREMENTS reqs = {};
        reqs.LowestStartingAddress = reinterpret_cast<PVOID>(0x01000000ULL);
        reqs.HighestEndingAddress = reinterpret_cast<PVOID>(0xFFFFFFFFULL);
        MEM_EXTENDED_PARAMETER param = {};
        param.Type = MemExtendedParameterAddressRequirements;
        param.Pointer = &reqs;
        p = pVirtualAlloc2(GetCurrentProcess(), nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, &param, 1);
      }
    }
  }

  // If VirtualAlloc2 failed or unavailable, scan 32-bit user space below 4GB using VirtualQuery
  if (!p) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000ULL;
    uintptr_t current = 0x01000000ULL;
    if (current < gran) current = gran;
    while (current + size <= 0x100000000ULL) {
      MEMORY_BASIC_INFORMATION mbi{};
      if (VirtualQuery(reinterpret_cast<void*>(current), &mbi, sizeof(mbi)) == 0) {
        break;
      }
      if (mbi.State == MEM_FREE) {
        uintptr_t freeStart = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        if (freeStart < 0x01000000ULL) freeStart = 0x01000000ULL;
        if (freeStart < gran) freeStart = gran;
        freeStart = (freeStart + gran - 1) & ~(gran - 1);
        uintptr_t freeEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (freeEnd > 0x100000000ULL) freeEnd = 0x100000000ULL;
        if (freeStart + size <= freeEnd) {
          p = VirtualAlloc(reinterpret_cast<void*>(freeStart), size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
          if (p) break;
        }
      }
      uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
      if (next <= current) next = current + gran;
      current = (next + gran - 1) & ~(gran - 1);
    }
  }

  // Legacy fallback: fixed-step probe
  if (!p) {
    for (uintptr_t addr = 0x10000000ULL; addr <= 0xE0000000ULL - size; addr += 0x01000000ULL) {
      p = VirtualAlloc(reinterpret_cast<void*>(addr), size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
      if (p) break;
    }
  }

  if (p && reinterpret_cast<uintptr_t>(p) + size > 0x100000000ULL) {
    Log.error("Allocated MEM1 at {:p}, which exceeds 4GB boundary (required for 32-bit disc slots)", p);
    VirtualFree(p, 0, MEM_RELEASE);
    p = nullptr;
  }

  if (!p) {
    DWORD err = GetLastError();
    char errBuf[256] = {};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                   errBuf, sizeof(errBuf), NULL);
    size_t len = strlen(errBuf);
    while (len > 0 && (errBuf[len - 1] == '\r' || errBuf[len - 1] == '\n')) {
      errBuf[--len] = '\0';
    }
    Log.fatal("Failed to commit memory for MEM1 ({} bytes) strictly under 4GB: {} (Win32 error {})",
              size, errBuf[0] ? errBuf : "Unknown error", err);
  }
  return p;
}
#elif defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
#include <sys/mman.h>
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
// Map MEM1 strictly below 4GB (preferably at 0x80000000) so that 32-bit pointer slots
// inside big-endian disc structures can hold real host addresses (see src/pc/disc.h).
// On Android 11+, 0x80000000 is frequently mapped by ART/dalvik heap, so probe candidate
// addresses and scan 32-bit address space if 0x80000000 is occupied.
static void* AllocMEM1(u32 size) {
  static const uintptr_t candidates[] = {
    0x80000000ULL,
    0x70000000ULL,
    0x60000000ULL,
    0x50000000ULL,
    0x40000000ULL,
    0x30000000ULL,
    0x20000000ULL,
    0x90000000ULL,
    0xA0000000ULL,
    0xB0000000ULL,
  };

  void* p = nullptr;
  for (uintptr_t addr : candidates) {
    if (addr + size <= 0x100000000ULL) {
      void* want = reinterpret_cast<void*>(addr);
      void* res = mmap(want, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
      if (res != MAP_FAILED) {
        if ((uintptr_t)res >= 0x01000000ULL && (uintptr_t)res + size <= 0x100000000ULL) {
          p = res;
          break;
        }
        munmap(res, size);
      }
    }
  }

  // If fixed candidate probing failed, scan 32-bit user space below 4GB in 16MB steps
  if (!p) {
    for (uintptr_t addr = 0x10000000ULL; addr <= 0xE0000000ULL - size; addr += 0x01000000ULL) {
      void* want = reinterpret_cast<void*>(addr);
      void* res = mmap(want, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
      if (res != MAP_FAILED) {
        if ((uintptr_t)res >= 0x01000000ULL && (uintptr_t)res + size <= 0x100000000ULL) {
          p = res;
          break;
        }
        munmap(res, size);
      }
    }
  }

#if defined(MAP_32BIT)
  if (!p) {
    void* res = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (res != MAP_FAILED) {
      if ((uintptr_t)res >= 0x01000000ULL && (uintptr_t)res + size <= 0x100000000ULL) {
        p = res;
      } else {
        munmap(res, size);
      }
    }
  }
#endif

  if (p && reinterpret_cast<uintptr_t>(p) + size > 0x100000000ULL) {
    Log.error("Allocated MEM1 at {:p}, which exceeds 4GB boundary (required for 32-bit disc slots)", p);
    munmap(p, size);
    p = nullptr;
  }

  if (!p) {
    Log.fatal("Failed to map MEM1 ({} bytes) strictly under 4GB", size);
  }
  return p;
}
#else
static void* AllocMEM1(u32 size) {
  return calloc(1, size);
}
#endif

u32 OSGetPhysicalMemSize() {
  const auto info = static_cast<OSBootInfo*>(OSPhysicalToCached(0));
  return info->memorySize;
}
