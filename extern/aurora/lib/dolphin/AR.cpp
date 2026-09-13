#include <dolphin/ar.h>
#include "../internal.hpp"
#include "dolphin/os.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

static aurora::Module Log("aurora::ar");

static u32 AR_StackPointer;
static u32* AR_BlockLength;
static u32 AR_FreeBlocks;
static BOOL AR_init_flag;

#define ARAM_STACK_START 0x4000

// ARAM emulation: allocate a large buffer to simulate the GameCube's Auxiliary RAM.
// ARAM "addresses" are offsets into this buffer. On GameCube, ARAM is 16 MB starting
// at a base address returned by ARInit. We emulate this by malloc'ing a buffer
// and using a simple bump allocator (matching ARAlloc behavior on real hardware).
static u8* sAramBuffer = nullptr;

// Convert an ARAM "address" (offset) to a real host pointer. `length` is the
// size of the access: a transfer that starts inside ARAM but runs past its end
// must be rejected, not clipped to the start check.
static u8* aramToHost(u32 aramAddr, u32 length) {
  const u32 size = aurora::g_config.mem2Size;
  if (!sAramBuffer || aramAddr >= size || length > size - aramAddr) {
    return nullptr;
  }
  return sAramBuffer + aramAddr;
}

// melee-pc: the audio mixer decodes samples straight out of ARAM.
extern "C" u8* aurora_aram_base(void) { return sAramBuffer; }

u32 ARAlloc(u32 length) {
  u32 tmp;

  ASSERTMSGLINE(430, !(length & 0x1F), "ARAlloc(): length is not multiple of 32bytes!");
  ASSERTMSGLINE(434, length <= (__AR_Size - __AR_StackPointer), "ARAlloc(): Out of ARAM!");
  ASSERTMSGLINE(435, __AR_FreeBlocks, "ARAlloc(): No more free blocks!");

  tmp = AR_StackPointer;
  AR_StackPointer += length;
  *AR_BlockLength = length;
  AR_BlockLength += 1;
  AR_FreeBlocks -= 1;
  return tmp;
}

u32 ARFree(u32* length) {
  AR_BlockLength -= 1;
  if (length) {
    *length = *AR_BlockLength;
  }
  AR_StackPointer -= *AR_BlockLength;
  AR_FreeBlocks += 1;
  return AR_StackPointer;
}

BOOL ARCheckInit(void) { return AR_init_flag; }

u32 ARInit(u32* stack_index_addr, u32 num_entries) {
  if (aurora::g_config.mem2Size == 0) {
    Log.warn("ARInit called but no mem2Size specified in AuroraConfig. ARAM will not be available!");
    return 0;
  }

  if (AR_init_flag == TRUE) {
    return ARAM_STACK_START;
  }

  sAramBuffer = (u8*)calloc(1, aurora::g_config.mem2Size);
  if (sAramBuffer) {
    Log.debug("Initialized 0x{:X} bytes of ARAM!", aurora::g_config.mem2Size);
  } else {
    Log.fatal("Failed to allocate ARAM!");
  }

  AR_StackPointer = ARAM_STACK_START;
  AR_FreeBlocks = num_entries;
  AR_BlockLength = stack_index_addr;

  AR_init_flag = TRUE;
  return AR_StackPointer;
}

u32 ARGetSize(void) { return aurora::g_config.mem2Size; }

#if !defined(_MSC_VER)
#pragma mark ARQ
#endif
// ARQ requests complete asynchronously, like real DMA: the transfer and its
// callback run on a worker thread. Games commonly post a request while
// interrupts are disabled and finish updating their bookkeeping before the
// callback may run; a synchronous callback would re-enter that code.
namespace {
struct ArqJob {
  ARQRequest* request;
  u32 type;
  uintptr_t source;
  uintptr_t dest;
  u32 length;
  ARQCallback callback;
};
std::mutex sArqMutex;
std::condition_variable sArqCv;
std::deque<ArqJob> sArqQueue;
std::thread sArqThread;
bool sArqStop = false;

void arq_transfer(const ArqJob& job) {
  // type 0 = MRAM -> ARAM, type 1 = ARAM -> MRAM
  if (job.type == ARAM_DIR_MRAM_TO_ARAM) {
    u8* hostSrc = reinterpret_cast<u8*>(job.source);
    u8* aramDst = aramToHost(static_cast<u32>(job.dest), job.length);
    if (aramDst && hostSrc) {
      memcpy(aramDst, hostSrc, job.length);
    }
  } else {
    u8* aramSrc = aramToHost(static_cast<u32>(job.source), job.length);
    u8* hostDst = reinterpret_cast<u8*>(job.dest);
    if (aramSrc && hostDst) {
      memcpy(hostDst, aramSrc, job.length);
    }
  }
}

void arq_worker() {
  std::unique_lock lock{sArqMutex};
  while (!sArqStop) {
    if (sArqQueue.empty()) {
      sArqCv.wait(lock);
      continue;
    }
    ArqJob job = sArqQueue.front();
    sArqQueue.pop_front();
    lock.unlock();
    arq_transfer(job);
    if (job.callback) {
      job.callback(job.request);
    }
    lock.lock();
  }
}
} // namespace

void ARQPostRequest(ARQRequest* request, u32 owner, u32 type, u32 priority, uintptr_t source, uintptr_t dest,
                    u32 length, ARQCallback callback) {
  // The SDK records the request parameters in the request itself; callbacks
  // read them back (e.g. `owner` carries the caller's context).
  request->next = nullptr;
  request->owner = owner;
  request->type = type;
  request->priority = priority;
  request->source = static_cast<u32>(source);
  request->dest = static_cast<u32>(dest);
  request->length = length;
  request->callback = callback;
  {
    std::lock_guard lock{sArqMutex};
    sArqQueue.push_back(ArqJob{request, type, source, dest, length, callback});
  }
  sArqCv.notify_one();
}

void ARQInit() {
  std::lock_guard lock{sArqMutex};
  if (!sArqThread.joinable()) {
    sArqStop = false;
    sArqThread = std::thread{arq_worker};
  }
}

void ARQReset() {
  {
    std::lock_guard lock{sArqMutex};
    sArqStop = true;
    sArqQueue.clear();
  }
  sArqCv.notify_all();
  if (sArqThread.joinable()) {
    sArqThread.join();
  }
}

void* ARGetStorageAddress() {
  return sAramBuffer;
}
