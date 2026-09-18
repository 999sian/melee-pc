#include <dolphin/os.h>

#include <algorithm>
#include <cstddef>
#include <unordered_map>
#include <array>
#if defined(_WIN32)
#include <windows.h>
static inline int capture_backtrace(void** buffer, int max_frames) {
  return CaptureStackBackTrace(0, max_frames, buffer, NULL);
}
#define backtrace capture_backtrace
#elif defined(__ANDROID__)
static inline int capture_backtrace(void** buffer, int max_frames) {
  (void)buffer;
  (void)max_frames;
  return 0;
}
#define backtrace capture_backtrace
#else
#include <execinfo.h>
#endif
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "../../logging.hpp"

extern "C" volatile OSHeapHandle __OSCurrHeap = -1;

namespace {

constexpr u32 kAlignment = 32;
constexpr u32 kHeaderSize = 32;
constexpr u32 kMinObjectSize = 64;

struct HeapDesc;

struct alignas(32) Cell {
  Cell* prev;
  Cell* next;
  s32 size;
  // melee-pc: the caller's requested byte count, written only under
  // MELEE_HEAP_CHECK. It occupies the 4 bytes of padding LP64 already inserts
  // before `owner`, so sizeof(Cell) stays 32 and the flag-off layout is
  // byte-identical to before.
  s32 request;
  HeapDesc* owner;
};

static_assert(sizeof(Cell) == kHeaderSize, "Cell header must stay 32 bytes");

struct HeapDesc {
  s32 size;
  Cell* freeList;
  Cell* allocated;
};

static aurora::Module AllocLog("aurora::os::alloc");

static HeapDesc* sHeapArray = nullptr;
static int sNumHeaps = 0;
static u8* sArenaStart = nullptr;
static u8* sArenaEnd = nullptr;

// melee-pc: MELEE_HEAP_CHECK=1 appends a 32-byte canary to every allocation and
// verifies all of them on each alloc/free (and from pc_frame_boundary via
// aurora_heap_check), so heap stomps are caught near the culprit.
static const bool sCanary = getenv("MELEE_HEAP_CHECK") != nullptr;
constexpr u8 kCanaryByte = 0xC5;

// Who allocated each live cell, so a stomp report names a call site instead of
// just an address. Only populated under MELEE_HEAP_CHECK.
using OwnerTrace = std::array<void*, 4>;
static std::unordered_map<const void*, OwnerTrace> sCellOwner;

static void recordOwner(const void* cell) {
  if (!sCanary) {
    return;
  }
  std::array<void*, 6> frames{};
  const int n = backtrace(frames.data(), static_cast<int>(frames.size()));
  OwnerTrace trace{};
  for (int i = 2, o = 0; i < n && o < static_cast<int>(trace.size()); ++i, ++o) {
    trace[o] = frames[i];
  }
  sCellOwner[cell] = trace;
}

static void forgetOwner(const void* cell) {
  if (sCanary) {
    sCellOwner.erase(cell);
  }
}

static uintptr_t roundUp32(const uintptr_t value) {
  return (value + (kAlignment - 1)) & ~(static_cast<uintptr_t>(kAlignment - 1));
}

static uintptr_t roundDown32(const uintptr_t value) {
  return value & ~(static_cast<uintptr_t>(kAlignment - 1));
}

static bool inArena(const void* ptr) {
  if (sArenaStart == nullptr || sArenaEnd == nullptr) {
    return false;
  }
  const auto p = reinterpret_cast<uintptr_t>(ptr);
  return p >= reinterpret_cast<uintptr_t>(sArenaStart) && p < reinterpret_cast<uintptr_t>(sArenaEnd);
}

static bool validHeapHandle(const OSHeapHandle heap) {
  return sHeapArray != nullptr && heap >= 0 && heap < sNumHeaps && sHeapArray[heap].size >= 0;
}

static bool cellAligned(const void* ptr) {
  return (reinterpret_cast<uintptr_t>(ptr) & (kAlignment - 1)) == 0;
}

// melee-pc: shared structural validation for one cell header -- exactly the
// chain OSCheckHeap already had, plus the one guard it was missing: `next` is
// bounds- and alignment-checked BEFORE `next->prev` is read. Without that the
// heap check can be killed by the very corruption it is hunting. Returns
// nullptr when the header looks sane, else a short description of the defect.
// Never dereferences a pointer it has not already validated. Kept to those
// checks so OSCheckHeap, which the game itself calls with MELEE_HEAP_CHECK off,
// does not get slower; the extra paranoia checkCanaries wants lives in
// checkCanaries.
static inline __attribute__((always_inline)) const char*
cellDefect(const HeapDesc& hd, const Cell* cell, const bool allocated) {
  if (!inArena(cell)) {
    return "cell pointer outside arena";
  }
  if (!cellAligned(cell)) {
    return "cell pointer misaligned";
  }
  if (cell->size < static_cast<s32>(kMinObjectSize) || (cell->size & (kAlignment - 1)) != 0) {
    return "cell size implausible";
  }
  if (cell->owner != (allocated ? &hd : nullptr)) {
    return "cell owner wrong";
  }
  if (cell->next != nullptr) {
    if (!inArena(cell->next) || !cellAligned(cell->next)) {
      return "next link invalid";
    }
    if (cell->next->prev != cell) {
      return "next->prev mismatch";
    }
  }
  return nullptr;
}

// Size sanity against the arena extent. Only the canary sweep pays for this;
// OSCheckHeap already rejects an oversized cell through its running total.
static const char* cellExtentDefect(const HeapDesc& hd, const Cell* cell) {
  if (cell->size > hd.size) {
    return "cell size exceeds its heap";
  }
  if (reinterpret_cast<uintptr_t>(cell) + static_cast<uintptr_t>(cell->size)
      > reinterpret_cast<uintptr_t>(sArenaEnd)) {
    return "cell extends past arena end";
  }
  return nullptr;
}

// The guarded region of a live cell runs from the end of the caller's request
// to the end of the cell, so the up-to-63 bytes of rounding slack that used to
// be a blind spot are guarded too.
static u32 guardLength(const Cell* cell) {
  return static_cast<u32>(cell->size) - kHeaderSize - static_cast<u32>(cell->request);
}

static bool guardPlausible(const Cell* cell) {
  return cell->request > 0
      && cell->request <= cell->size - static_cast<s32>(kHeaderSize + kAlignment);
}

// Reports a broken header without dereferencing the suspect cell. The owner
// map lives on the host heap, not in the arena, so its backtraces survive an
// arena stomp; we name the failing cell's allocator if we know it, else the
// last cell that validated cleanly.
[[noreturn]] static void reportBrokenHeader(const char* where, const char* defect, const int heap,
                                            const void* cell, const void* lastGood,
                                            const u32 walked) {
  static const OwnerTrace kNoTrace{};
  const auto self = sCellOwner.find(cell);
  const auto prev = sCellOwner.find(lastGood);
  const bool haveSelf = self != sCellOwner.end();
  const OwnerTrace trace = haveSelf ? self->second
                           : prev != sCellOwner.end() ? prev->second
                                                      : kNoTrace;
  AllocLog.fatal("heap header corrupted at {}: {} in heap {} cell {}, walked {} cells "
                 "(last good cell {}) allocated by {} {} {} {}",
                 where, defect, heap, cell, walked, lastGood, trace[0], trace[1], trace[2],
                 trace[3]);
}

static void checkCanaries(const char* where) {
  if (!sCanary || sHeapArray == nullptr) {
    return;
  }
  for (int h = 0; h < sNumHeaps; ++h) {
    auto& hd = sHeapArray[h];
    if (hd.size < 0) {
      continue;
    }
    // Cells never overlap and are never smaller than kMinObjectSize, so a sane
    // allocated list cannot be longer than this. Bounds the walk, so a cycle
    // introduced by a stomped link terminates with a report instead of hanging.
    const u32 maxCells = static_cast<u32>(hd.size) / kMinObjectSize + 1;
    const Cell* lastGood = nullptr;
    const Cell* expectedPrev = nullptr;
    u32 walked = 0;
    for (Cell* c = hd.allocated; c != nullptr; c = c->next) {
      if (walked++ >= maxCells) {
        reportBrokenHeader(where, "allocated list longer than the heap can hold (link cycle)", h, c,
                           lastGood, walked);
      }
      if (const char* defect = cellDefect(hd, c, true); defect != nullptr) {
        reportBrokenHeader(where, defect, h, c, lastGood, walked);
      }
      if (c->prev != expectedPrev) {
        reportBrokenHeader(where, "prev link does not match walk order", h, c, lastGood, walked);
      }
      if (!guardPlausible(c)) {
        reportBrokenHeader(where, "recorded request size implausible", h, c, lastGood, walked);
      }
      if (const char* defect = cellExtentDefect(hd, c); defect != nullptr) {
        reportBrokenHeader(where, defect, h, c, lastGood, walked);
      }
      const u8* canary = reinterpret_cast<const u8*>(c) + kHeaderSize + static_cast<u32>(c->request);
      const u32 len = guardLength(c);
      for (u32 i = 0; i < len; ++i) {
        if (canary[i] != kCanaryByte) {
          const auto owner = sCellOwner.find(c);
          AllocLog.fatal("heap stomp detected at {}: cell {} (user {}, size {}) canary at {} "
                         "first bad byte +{} allocated by {} {} {} {}",
                         where, static_cast<const void*>(c),
                         static_cast<const void*>(reinterpret_cast<const u8*>(c) + kHeaderSize),
                         c->size, static_cast<const void*>(canary), i,
                         owner == sCellOwner.end() ? nullptr : owner->second[0],
                         owner == sCellOwner.end() ? nullptr : owner->second[1],
                         owner == sCellOwner.end() ? nullptr : owner->second[2],
                         owner == sCellOwner.end() ? nullptr : owner->second[3]);
        }
      }
      expectedPrev = c;
      lastGood = c;
    }
  }
}

static Cell* addFront(Cell* list, Cell* cell) {
  cell->prev = nullptr;
  cell->next = list;
  if (list != nullptr) {
    list->prev = cell;
  }
  return cell;
}

static Cell* extract(Cell* list, Cell* cell) {
  if (cell->next != nullptr) {
    cell->next->prev = cell->prev;
  }
  if (cell->prev != nullptr) {
    cell->prev->next = cell->next;
    return list;
  }
  return cell->next;
}

static bool containsCell(Cell* list, Cell* cell) {
  for (Cell* it = list; it != nullptr; it = it->next) {
    if (it == cell) {
      return true;
    }
  }
  return false;
}

static Cell* insertAndCoalesce(Cell* list, Cell* cell) {
  Cell* prev = nullptr;
  Cell* next = list;
  while (next != nullptr && next < cell) {
    prev = next;
    next = next->next;
  }

  cell->prev = prev;
  cell->next = next;
  if (prev != nullptr) {
    prev->next = cell;
  } else {
    list = cell;
  }
  if (next != nullptr) {
    next->prev = cell;
  }

  if (cell->next != nullptr) {
    auto* right = cell->next;
    if (reinterpret_cast<u8*>(cell) + cell->size == reinterpret_cast<u8*>(right)) {
      cell->size += right->size;
      cell->next = right->next;
      if (right->next != nullptr) {
        right->next->prev = cell;
      }
    }
  }

  if (cell->prev != nullptr) {
    auto* left = cell->prev;
    if (reinterpret_cast<u8*>(left) + left->size == reinterpret_cast<u8*>(cell)) {
      left->size += cell->size;
      left->next = cell->next;
      if (cell->next != nullptr) {
        cell->next->prev = left;
      }
      return list;
    }
  }

  return list;
}

static bool validateBlockRange(const uintptr_t start, const uintptr_t end) {
  if (start >= end) {
    return false;
  }
  if (sArenaStart == nullptr || sArenaEnd == nullptr) {
    return false;
  }
  return start >= reinterpret_cast<uintptr_t>(sArenaStart)
      && end <= reinterpret_cast<uintptr_t>(sArenaEnd)
      && (end - start) >= kMinObjectSize;
}

static void dropTinyCell(HeapDesc& hd, Cell* cell) {
  hd.freeList = extract(hd.freeList, cell);
  hd.size -= cell->size;
}

static void carveRangeFromHeap(HeapDesc& hd, uintptr_t carveStart, uintptr_t carveEnd) {
  Cell* cell = hd.freeList;
  while (cell != nullptr) {
    Cell* nextCell = cell->next;
    const auto cellStart = reinterpret_cast<uintptr_t>(cell);
    const auto cellEnd = cellStart + static_cast<uintptr_t>(cell->size);

    const auto overlapStart = carveStart > cellStart ? carveStart : cellStart;
    const auto overlapEnd = carveEnd < cellEnd ? carveEnd : cellEnd;
    if (overlapStart >= overlapEnd) {
      cell = nextCell;
      continue;
    }

    const auto removed = static_cast<s32>(overlapEnd - overlapStart);
    hd.size -= removed;

    const bool cutHead = overlapStart == cellStart;
    const bool cutTail = overlapEnd == cellEnd;

    if (cutHead && cutTail) {
      hd.freeList = extract(hd.freeList, cell);
    } else if (cutHead) {
      auto* newCell = reinterpret_cast<Cell*>(overlapEnd);
      newCell->size = static_cast<s32>(cellEnd - overlapEnd);
      newCell->owner = nullptr;
      newCell->prev = cell->prev;
      newCell->next = cell->next;
      if (newCell->prev != nullptr) {
        newCell->prev->next = newCell;
      } else {
        hd.freeList = newCell;
      }
      if (newCell->next != nullptr) {
        newCell->next->prev = newCell;
      }
      if (newCell->size < static_cast<s32>(kMinObjectSize)) {
        dropTinyCell(hd, newCell);
      }
    } else if (cutTail) {
      cell->size = static_cast<s32>(overlapStart - cellStart);
      if (cell->size < static_cast<s32>(kMinObjectSize)) {
        dropTinyCell(hd, cell);
      }
    } else {
      const auto leftSize = static_cast<s32>(overlapStart - cellStart);
      const auto rightSize = static_cast<s32>(cellEnd - overlapEnd);
      if (leftSize >= static_cast<s32>(kMinObjectSize) && rightSize >= static_cast<s32>(kMinObjectSize)) {
        auto* right = reinterpret_cast<Cell*>(overlapEnd);
        right->size = rightSize;
        right->owner = nullptr;
        right->prev = cell;
        right->next = cell->next;
        if (right->next != nullptr) {
          right->next->prev = right;
        }
        cell->next = right;
        cell->size = leftSize;
      } else if (leftSize >= static_cast<s32>(kMinObjectSize)) {
        cell->size = leftSize;
      } else if (rightSize >= static_cast<s32>(kMinObjectSize)) {
        auto* right = reinterpret_cast<Cell*>(overlapEnd);
        right->size = rightSize;
        right->owner = nullptr;
        right->prev = cell->prev;
        right->next = cell->next;
        if (right->prev != nullptr) {
          right->prev->next = right;
        } else {
          hd.freeList = right;
        }
        if (right->next != nullptr) {
          right->next->prev = right;
        }
      } else {
        hd.freeList = extract(hd.freeList, cell);
      }
    }

    cell = nextCell;
  }
}

} // namespace

extern "C" {

void* OSInitAlloc(void* arenaStart, void* arenaEnd, int maxHeaps) {
  if (arenaStart == nullptr || arenaEnd == nullptr || maxHeaps <= 0) {
    return nullptr;
  }

  auto start = reinterpret_cast<uintptr_t>(arenaStart);
  auto end = reinterpret_cast<uintptr_t>(arenaEnd);
  if (start >= end) {
    return nullptr;
  }

  const auto arrayBytes = static_cast<uintptr_t>(maxHeaps) * sizeof(HeapDesc);
  if ((end - start) < arrayBytes + kMinObjectSize) {
    return nullptr;
  }

  sHeapArray = reinterpret_cast<HeapDesc*>(arenaStart);
  sNumHeaps = maxHeaps;
  for (int i = 0; i < sNumHeaps; ++i) {
    sHeapArray[i].size = -1;
    sHeapArray[i].freeList = nullptr;
    sHeapArray[i].allocated = nullptr;
  }

  __OSCurrHeap = -1;
  sArenaStart = reinterpret_cast<u8*>(roundUp32(start + arrayBytes));
  sArenaEnd = reinterpret_cast<u8*>(roundDown32(end));
  if (sArenaEnd <= sArenaStart || static_cast<uintptr_t>(sArenaEnd - sArenaStart) < kMinObjectSize) {
    sHeapArray = nullptr;
    sNumHeaps = 0;
    sArenaStart = nullptr;
    sArenaEnd = nullptr;
    return nullptr;
  }

  return sArenaStart;
}

OSHeapHandle OSCreateHeap(void* start, void* end) {
  if (sHeapArray == nullptr) {
    return -1;
  }

  const auto blockStart = roundUp32(reinterpret_cast<uintptr_t>(start));
  const auto blockEnd = roundDown32(reinterpret_cast<uintptr_t>(end));
  if (!validateBlockRange(blockStart, blockEnd)) {
    return -1;
  }

  for (OSHeapHandle heap = 0; heap < sNumHeaps; ++heap) {
    auto& hd = sHeapArray[heap];
    if (hd.size >= 0) {
      continue;
    }

    hd.size = static_cast<s32>(blockEnd - blockStart);
    hd.allocated = nullptr;
    hd.freeList = reinterpret_cast<Cell*>(blockStart);
    hd.freeList->prev = nullptr;
    hd.freeList->next = nullptr;
    hd.freeList->size = hd.size;
    hd.freeList->owner = nullptr;
    return heap;
  }

  return -1;
}

void OSDestroyHeap(OSHeapHandle heap) {
  if (!validHeapHandle(heap)) {
    return;
  }

  auto& hd = sHeapArray[heap];
  hd.size = -1;
  hd.freeList = nullptr;
  hd.allocated = nullptr;
  if (__OSCurrHeap == heap) {
    __OSCurrHeap = -1;
  }
}

void OSAddToHeap(OSHeapHandle heap, void* start, void* end) {
  if (!validHeapHandle(heap)) {
    return;
  }

  const auto blockStart = roundUp32(reinterpret_cast<uintptr_t>(start));
  const auto blockEnd = roundDown32(reinterpret_cast<uintptr_t>(end));
  if (!validateBlockRange(blockStart, blockEnd)) {
    return;
  }

  auto& hd = sHeapArray[heap];
  auto* cell = reinterpret_cast<Cell*>(blockStart);
  cell->prev = nullptr;
  cell->next = nullptr;
  cell->size = static_cast<s32>(blockEnd - blockStart);
  cell->owner = nullptr;
  hd.freeList = insertAndCoalesce(hd.freeList, cell);
  hd.size += cell->size;
}

void* OSAllocFromHeap(OSHeapHandle heap, u32 size) {
  if (!validHeapHandle(heap) || size == 0) {
    return nullptr;
  }

  auto& hd = sHeapArray[heap];
  checkCanaries("alloc");
  const auto requested =
      static_cast<s32>(roundUp32(static_cast<uintptr_t>(size) + kHeaderSize + (sCanary ? kAlignment : 0)));

  Cell* cell = hd.freeList;
  while (cell != nullptr && cell->size < requested) {
    cell = cell->next;
  }
  if (cell == nullptr) {
    return nullptr;
  }

  const auto leftover = cell->size - requested;
  if (leftover < static_cast<s32>(kMinObjectSize)) {
    hd.freeList = extract(hd.freeList, cell);
  } else {
    auto* split = reinterpret_cast<Cell*>(reinterpret_cast<u8*>(cell) + requested);
    split->size = leftover;
    split->owner = nullptr;
    split->prev = cell->prev;
    split->next = cell->next;
    if (split->prev != nullptr) {
      split->prev->next = split;
    } else {
      hd.freeList = split;
    }
    if (split->next != nullptr) {
      split->next->prev = split;
    }
    cell->size = requested;
  }

  cell->owner = &hd;
  hd.allocated = addFront(hd.allocated, cell);
  if (sCanary) {
    cell->request = static_cast<s32>(size);
    memset(reinterpret_cast<u8*>(cell) + kHeaderSize + size, kCanaryByte, guardLength(cell));
    recordOwner(cell);
  }
  return reinterpret_cast<u8*>(cell) + kHeaderSize;
}

void OSFreeToHeap(OSHeapHandle heap, void* ptr) {
  if (!validHeapHandle(heap) || ptr == nullptr) {
    return;
  }
  if (!inArena(ptr) || (reinterpret_cast<uintptr_t>(ptr) & (kAlignment - 1)) != 0) {
    return;
  }

  auto& hd = sHeapArray[heap];
  auto* cell = reinterpret_cast<Cell*>(reinterpret_cast<u8*>(ptr) - kHeaderSize);
  if (cell->owner != &hd || !containsCell(hd.allocated, cell)) {
    return;
  }

  checkCanaries("free");
  forgetOwner(cell);
  hd.allocated = extract(hd.allocated, cell);
  cell->owner = nullptr;
  hd.freeList = insertAndCoalesce(hd.freeList, cell);
}

extern "C" void aurora_heap_check(void) { checkCanaries("frame"); }

OSHeapHandle OSSetCurrentHeap(OSHeapHandle heap) {
  const auto prev = __OSCurrHeap;
  if (heap == -1 || validHeapHandle(heap)) {
    __OSCurrHeap = heap;
  }
  return prev;
}

void* OSAllocFixed(void* rstart, void* rend) {
  if (sHeapArray == nullptr || rstart == nullptr || rend == nullptr) {
    return nullptr;
  }

  for (int i = 0; i < sNumHeaps; ++i) {
    if (sHeapArray[i].size >= 0 && sHeapArray[i].allocated != nullptr) {
      return nullptr;
    }
  }

  const auto fixedStart = roundDown32(reinterpret_cast<uintptr_t>(rstart));
  const auto fixedEnd = roundUp32(reinterpret_cast<uintptr_t>(rend));
  if (fixedStart >= fixedEnd) {
    return nullptr;
  }
  if (fixedStart < reinterpret_cast<uintptr_t>(sArenaStart)
      || fixedEnd > reinterpret_cast<uintptr_t>(sArenaEnd)) {
    return nullptr;
  }

  for (int i = 0; i < sNumHeaps; ++i) {
    auto& hd = sHeapArray[i];
    if (hd.size >= 0) {
      carveRangeFromHeap(hd, fixedStart, fixedEnd);
    }
  }

  return reinterpret_cast<void*>(fixedStart);
}

s32 OSCheckHeap(OSHeapHandle heap) {
  if (!validHeapHandle(heap)) {
    return -1;
  }

  auto& hd = sHeapArray[heap];
  s32 total = 0;
  s32 freeBytes = 0;

  if (hd.allocated != nullptr && hd.allocated->prev != nullptr) {
    return -1;
  }

  for (Cell* cell = hd.allocated; cell != nullptr; cell = cell->next) {
    if (cellDefect(hd, cell, true) != nullptr) {
      return -1;
    }
    total += cell->size;
    if (total <= 0 || total > hd.size) {
      return -1;
    }
  }

  if (hd.freeList != nullptr && hd.freeList->prev != nullptr) {
    return -1;
  }

  for (Cell* cell = hd.freeList; cell != nullptr; cell = cell->next) {
    if (cellDefect(hd, cell, false) != nullptr) {
      return -1;
    }
    if (cell->next != nullptr) {
      if (reinterpret_cast<uintptr_t>(cell) + static_cast<uintptr_t>(cell->size)
          > reinterpret_cast<uintptr_t>(cell->next)) {
        return -1;
      }
    }

    total += cell->size;
    freeBytes += cell->size - static_cast<s32>(kHeaderSize);
    if (total <= 0 || total > hd.size) {
      return -1;
    }
  }

  if (total != hd.size) {
    return -1;
  }
  return freeBytes;
}

u32 OSReferentSize(void* ptr) {
  if (ptr == nullptr || !inArena(ptr) || (reinterpret_cast<uintptr_t>(ptr) & (kAlignment - 1)) != 0) {
    return 0;
  }
  auto* cell = reinterpret_cast<Cell*>(reinterpret_cast<u8*>(ptr) - kHeaderSize);
  if (cell->owner == nullptr) {
    return 0;
  }
  return static_cast<u32>(cell->size - static_cast<s32>(kHeaderSize));
}

void OSDumpHeap(OSHeapHandle heap) {
  AllocLog.info("OSDumpHeap({})", heap);
  if (!validHeapHandle(heap)) {
    AllocLog.info("--------Invalid");
    return;
  }

  auto& hd = sHeapArray[heap];
  if (OSCheckHeap(heap) < 0) {
    AllocLog.info("--------Broken");
    return;
  }

  AllocLog.info("addr\tsize\t\tend\t\tprev\t\tnext");
  AllocLog.info("--------Allocated");
  for (Cell* cell = hd.allocated; cell != nullptr; cell = cell->next) {
    AllocLog.info("{}\t{}\t{}\t{}\t{}",
                  reinterpret_cast<void*>(cell),
                  cell->size,
                  reinterpret_cast<void*>(reinterpret_cast<u8*>(cell) + cell->size),
                  reinterpret_cast<void*>(cell->prev),
                  reinterpret_cast<void*>(cell->next));
  }

  AllocLog.info("--------Free");
  for (Cell* cell = hd.freeList; cell != nullptr; cell = cell->next) {
    AllocLog.info("{}\t{}\t{}\t{}\t{}",
                  reinterpret_cast<void*>(cell),
                  cell->size,
                  reinterpret_cast<void*>(reinterpret_cast<u8*>(cell) + cell->size),
                  reinterpret_cast<void*>(cell->prev),
                  reinterpret_cast<void*>(cell->next));
  }
}

void OSVisitAllocated(void (*visitor)(void*, u32)) {
  if (visitor == nullptr || sHeapArray == nullptr) {
    return;
  }

  for (int heap = 0; heap < sNumHeaps; ++heap) {
    auto& hd = sHeapArray[heap];
    if (hd.size < 0) {
      continue;
    }
    for (Cell* cell = hd.allocated; cell != nullptr; cell = cell->next) {
      visitor(reinterpret_cast<u8*>(cell) + kHeaderSize,
              static_cast<u32>(cell->size - static_cast<s32>(kHeaderSize)));
    }
  }
}

// melee-pc: bytes that must be copied to snapshot/restore a heap: every
// allocated cell in full, every free cell's header (its body is dead space
// until it is split, and splitting rewrites only headers). Returns false for
// an unused handle.
bool aurora_heap_extent(OSHeapHandle heap, void** lo, void** hi) {
  if (!validHeapHandle(heap)) {
    return false;
  }
  const auto& hd = sHeapArray[heap];
  uintptr_t min = UINTPTR_MAX, max = 0;
  for (Cell* cell = hd.allocated; cell != nullptr; cell = cell->next) {
    const auto p = reinterpret_cast<uintptr_t>(cell);
    min = std::min(min, p);
    max = std::max(max, p + static_cast<uintptr_t>(cell->size));
  }
  for (Cell* cell = hd.freeList; cell != nullptr; cell = cell->next) {
    const auto p = reinterpret_cast<uintptr_t>(cell);
    min = std::min(min, p);
    max = std::max(max, p + kHeaderSize);
  }
  if (max == 0) {
    return false;
  }
  *lo = reinterpret_cast<void*>(min);
  *hi = reinterpret_cast<void*>(max);
  return true;
}

// melee-pc: the HeapDesc array (list heads) sits at the arena start, outside
// every cell, and must be part of any snapshot of the heaps.
void aurora_heap_descs(void** lo, size_t* len) {
  *lo = sHeapArray;
  *len = static_cast<size_t>(sNumHeaps) * sizeof(HeapDesc);
}

} // extern "C"
