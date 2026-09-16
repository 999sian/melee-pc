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
  GuardGCMemory();

  if (aurora::g_config.mem1Size > 0) {
    MEM1Start = AllocMEM1(aurora::g_config.mem1Size);
    MEM1End = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(MEM1Start) + aurora::g_config.mem1Size);
    OSBaseAddress = reinterpret_cast<uintptr_t>(MEM1Start);
  }
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
  // Reserve the normal GC/Wii memory map so accesses are 100% guaranteed to fail.
  // https://www.gc-forever.com/yagcd/chap5.html#sec5.11
  // https://wiibrew.org/wiki/Memory_map

  // We can't quite map at address 0 (for good reasons) but we *can* map at the next granularity over!
  TryGuardRegion(0x00000000 + GetAllocationGranularity(), 0x017fffff, "MEM1 Physical");
  TryGuardRegion(0x80000000, 0x817fffff, "MEM1 Logical (cached)");
  TryGuardRegion(0xC0000000, 0xC17fffff, "MEM1 Logical (uncached)");
  TryGuardRegion(0x10000000, 0x13FFFFFF, "MEM2 Physical");
  TryGuardRegion(0x90000000, 0x93FFFFFF, "MEM2 Logical (cached)");
  TryGuardRegion(0xD0000000, 0xD3FFFFFF, "MEM2 Logical (uncached)");
  TryGuardRegion(0x08000000, 0x08300000, "EFB Physical");
  TryGuardRegion(0xC8000000, 0xC8300000, "EFB Logical");
  TryGuardRegion(0x0D000000, 0x0D008000, "Hollywood HW registers Physical");
  TryGuardRegion(0xCD000000, 0xCD008000, "Hollywood HW registers Logical");
  TryGuardRegion(0x0C000000, 0x0C008020, "Broadway/GC HW registers Physical");
  TryGuardRegion(0xCC000000, 0xCC008020, "Broadway/GC HW registers Logical");
  TryGuardRegion(0xe0000000, 0xe0003fff, "GC L2 cache");
  TryGuardRegion(0xfff00000, 0xffffffff, "GC IPL");
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

  // If fixed candidate probing failed, scan 32-bit user space below 4GB
  if (!p) {
    for (uintptr_t addr = 0x10000000ULL; addr <= 0xE0000000ULL - size; addr += 0x01000000ULL) {
      p = VirtualAlloc(reinterpret_cast<void*>(addr), size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
      if (p) break;
    }
  }

  // If fixed address probing failed, try VirtualAlloc2 with 4GB limit if available
  if (!p) {
    typedef PVOID (WINAPI *VirtualAlloc2_t)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
    HMODULE kernelBase = GetModuleHandleA("kernelbase.dll");
    if (kernelBase) {
      auto pVirtualAlloc2 = reinterpret_cast<VirtualAlloc2_t>(GetProcAddress(kernelBase, "VirtualAlloc2"));
      if (pVirtualAlloc2) {
        MEM_ADDRESS_REQUIREMENTS reqs = {};
        reqs.HighestEndingAddress = reinterpret_cast<PVOID>(0xFFFFFFFFULL);
        MEM_EXTENDED_PARAMETER param = {};
        param.Type = MemExtendedParameterAddressRequirements;
        param.Pointer = &reqs;
        p = pVirtualAlloc2(GetCurrentProcess(), nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, &param, 1);
      }
    }
  }

  if (p && reinterpret_cast<uintptr_t>(p) + size > 0x100000000ULL) {
    Log.error("Allocated MEM1 at {:p}, which exceeds 4GB boundary (required for 32-bit disc slots)", p);
    VirtualFree(p, 0, MEM_RELEASE);
    p = nullptr;
  }

  if (!p) {
    DWORD err = GetLastError();
    fmt::memory_buffer msg;
    fmt::format_system_error(
      msg,
      static_cast<int>(err),
      "Failed to commit memory for MEM1 strictly under 4GB");
    Log.fatal("{}", fmt::to_string(msg));
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
#elif defined(__APPLE__)
#include <sys/mman.h>
// arm64 macOS kills native processes whose __PAGEZERO is smaller than 4GB, so MEM1 can never
// sit below 4GB here. Instead place it at an address whose low 32 bits are exactly 0x80000000:
// truncating a MEM1 pointer to 32 bits then yields its GameCube address, which is what the
// game's 32-bit disc slots hold (see src/pc/disc.h, PC_MEM1_ALIAS).
static void* AllocMEM1(u32 size) {
  for (uintptr_t hi = 0x1; hi < 0x100; hi++) {
    const uintptr_t want = (hi << 32) | 0x80000000ULL;
    void* res = mmap(reinterpret_cast<void*>(want), size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (res == MAP_FAILED) {
      continue;
    }
    if (reinterpret_cast<uintptr_t>(res) == want) {
      return res;
    }
    munmap(res, size);
  }
  Log.fatal("Failed to map MEM1 ({} bytes) at an address aliasing 0x80000000", size);
  return nullptr;
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
