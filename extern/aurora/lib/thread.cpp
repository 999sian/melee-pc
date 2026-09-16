#include "thread.hpp"
#include "logging.hpp"

#include <SDL3/SDL_thread.h>
#include <tracy/Tracy.hpp>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <pthread.h>
#endif

namespace aurora::thread {
namespace {
constexpr Module Log{"aurora::thread"};

bool is_pinning_disabled() noexcept {
  if (const char* env = std::getenv("AURORA_NO_PIN"); env && *env && *env != '0') {
    return true;
  }
  if (const char* env = std::getenv("AURORA_PIN_THREADS"); env && *env) {
    if (*env == '0' || *env == 'n' || *env == 'N' || *env == 'f' || *env == 'F') {
      return true;
    }
  }
  return false;
}

bool is_pinning_forced() noexcept {
  if (const char* env = std::getenv("AURORA_PIN_THREADS"); env && *env) {
    if (*env == '1' || *env == 'y' || *env == 'Y' || *env == 't' || *env == 'T') {
      return true;
    }
  }
  return false;
}

struct Processor {
  uint16_t group = 0;
  uint32_t number = 0;
};

struct CacheDomain {
  std::vector<Processor> processors;
  uint8_t cacheLevel = 0;
};

std::mutex sDomainMutex;
std::optional<CacheDomain> sDomain;
bool sDomainConfigured = false;

#if defined(__linux__)
bool get_current_thread_affinity(cpu_set_t& set) noexcept {
#if defined(__ANDROID__)
  return sched_getaffinity(pid_t{0}, sizeof(set), &set) == 0;
#else
  return pthread_getaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#endif
}

bool set_current_thread_affinity(const cpu_set_t& set) noexcept {
#if defined(__ANDROID__)
  return sched_setaffinity(pid_t{0}, sizeof(set), &set) == 0;
#else
  return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#endif
}

std::optional<std::string> read_line(const std::string& path) {
  std::ifstream input{path};
  std::string value;
  if (!std::getline(input, value) || value.empty()) {
    return std::nullopt;
  }
  return value;
}

std::vector<uint32_t> parse_cpu_list(const std::string& list) {
  std::vector<uint32_t> cpus;
  size_t tokenStart = 0;
  while (tokenStart < list.size()) {
    const size_t tokenEnd = list.find(',', tokenStart);
    const size_t end = tokenEnd == std::string::npos ? list.size() : tokenEnd;
    const size_t dash = list.find('-', tokenStart);

    uint32_t first = 0;
    const char* firstBegin = list.data() + tokenStart;
    const char* firstEnd = list.data() + (dash < end ? dash : end);
    if (std::from_chars(firstBegin, firstEnd, first).ec != std::errc{}) {
      return {};
    }

    uint32_t last = first;
    if (dash < end) {
      const char* lastBegin = list.data() + dash + 1;
      const char* lastEnd = list.data() + end;
      if (std::from_chars(lastBegin, lastEnd, last).ec != std::errc{} || last < first) {
        return {};
      }
    }

    for (uint32_t cpu = first; cpu <= last; ++cpu) {
      cpus.push_back(cpu);
      if (cpu == UINT32_MAX) {
        break;
      }
    }
    tokenStart = end + 1;
  }
  return cpus;
}

std::optional<CacheDomain> find_cache_domain() {
  if (is_pinning_disabled()) {
    Log.info("Thread pinning disabled by environment");
    return std::nullopt;
  }
  const bool forcePin = is_pinning_forced();

#if defined(__ANDROID__) || defined(__arm__) || defined(__aarch64__)
  if (!forcePin) {
    // On ARM platforms (Android, Nintendo Switch L4T Linux, Raspberry Pi, etc.),
    // cores are dynamically managed by kernel Energy Aware Scheduling (EAS) and cpufreq.
    // Pinning threads restricts execution to a subset of cores, starves governor frequency scaling,
    // causes thermal throttling, and on quad-core SoCs (like Tegra X1) starves cores 0 and 1
    // while overloading cores 2 and 3 with all 3 engine threads.
    return std::nullopt;
  }
#endif

  const long numProcessors = sysconf(_SC_NPROCESSORS_CONF);
  if (!forcePin && numProcessors <= 4) {
    // On systems with 4 or fewer cores, Aurora's 3 primary threads (Main, FIFO processor,
    // and Render worker) need all available cores. Pinning to a cache domain or subset of
    // cores artificially starves threads.
    return std::nullopt;
  }

  int targetCpu = sched_getcpu();
  if (targetCpu < 0) {
    targetCpu = 0;
  }

  // On heterogeneous architectures (e.g. ARM big.LITTLE),
  // select the highest-capacity or highest-frequency CPU core cluster rather than
  // arbitrarily using whichever low-power core the calling thread happened to start on.
  if (numProcessors > 1) {
    uint64_t bestMetric = 0;
    int bestCpu = targetCpu;
    for (long i = 0; i < numProcessors; ++i) {
      uint64_t metric = 0;
      auto capStr = read_line("/sys/devices/system/cpu/cpu" + std::to_string(i) + "/cpu_capacity");
      if (capStr) {
        try { metric = std::stoull(*capStr); } catch (...) {}
      }
      if (metric == 0) {
        auto freqStr = read_line("/sys/devices/system/cpu/cpu" + std::to_string(i) + "/cpufreq/cpuinfo_max_freq");
        if (!freqStr) freqStr = read_line("/sys/devices/system/cpu/cpu" + std::to_string(i) + "/cpufreq/scaling_max_freq");
        if (freqStr) {
          try { metric = std::stoull(*freqStr); } catch (...) {}
        }
      }
      if (metric > bestMetric) {
        bestMetric = metric;
        bestCpu = static_cast<int>(i);
      }
    }
    targetCpu = bestCpu;
  }

  CacheDomain best;
  const std::string cacheRoot = "/sys/devices/system/cpu/cpu" + std::to_string(targetCpu) + "/cache";
  for (uint32_t index = 0; index < 32; ++index) {
    const std::string indexRoot = cacheRoot + "/index" + std::to_string(index);
    const auto levelText = read_line(indexRoot + "/level");
    const auto type = read_line(indexRoot + "/type");
    const auto cpuList = read_line(indexRoot + "/shared_cpu_list");
    if (!levelText || !type || !cpuList || (*type != "Unified" && *type != "Data")) {
      continue;
    }

    uint32_t level = 0;
    if (std::from_chars(levelText->data(), levelText->data() + levelText->size(), level).ec != std::errc{} ||
        level <= best.cacheLevel) {
      continue;
    }

    const auto cpus = parse_cpu_list(*cpuList);
    if (std::find(cpus.begin(), cpus.end(), static_cast<uint32_t>(targetCpu)) == cpus.end()) {
      continue;
    }

    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (!get_current_thread_affinity(allowed)) {
      continue;
    }

    CacheDomain candidate;
    candidate.cacheLevel = static_cast<uint8_t>(level);
    for (const uint32_t cpu : cpus) {
      if (cpu < CPU_SETSIZE && CPU_ISSET(static_cast<int>(cpu), &allowed)) {
        candidate.processors.push_back({.number = cpu});
      }
    }
    if (!candidate.processors.empty()) {
      best = std::move(candidate);
    }
  }

  // Aurora requires at least 3 heavy threads (Main, FIFO processor, Render worker).
  // If the cache domain contains fewer than 3 processors, pinning all 3 threads to it
  // will cause severe contention.
  // Furthermore, if the domain covers all available processors, explicit affinity is redundant.
  if (best.processors.size() < 3 || (!forcePin && best.processors.size() >= static_cast<size_t>(numProcessors))) {
    return std::nullopt;
  }

  return best;
}

bool apply_cache_domain(const CacheDomain& domain) noexcept {
  cpu_set_t set;
  CPU_ZERO(&set);
  for (const auto& processor : domain.processors) {
    if (processor.number < CPU_SETSIZE) {
      CPU_SET(static_cast<int>(processor.number), &set);
    }
  }
  return set_current_thread_affinity(set);
}
#elif defined(_WIN32)
std::optional<CacheDomain> find_cache_domain() {
  if (is_pinning_disabled()) {
    return std::nullopt;
  }
  const bool forcePin = is_pinning_forced();

  SYSTEM_INFO sysInfo{};
  GetSystemInfo(&sysInfo);
  if (!forcePin && sysInfo.dwNumberOfProcessors <= 4) {
    return std::nullopt;
  }

  PROCESSOR_NUMBER currentProcessor{};
  GetCurrentProcessorNumberEx(&currentProcessor);

  DWORD bufferSize = 0;
  GetLogicalProcessorInformationEx(RelationCache, nullptr, &bufferSize);
  if (bufferSize == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
    return std::nullopt;
  }

  std::vector<uint8_t> buffer(bufferSize);
  auto* first = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data());
  if (!GetLogicalProcessorInformationEx(RelationCache, first, &bufferSize)) {
    return std::nullopt;
  }

  CacheDomain best;
  size_t offset = 0;
  while (offset < bufferSize) {
    auto* info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data() + offset);
    const auto& cache = info->Cache;
    const KAFFINITY currentMask = KAFFINITY{1} << currentProcessor.Number;
    if ((cache.Type == CacheUnified || cache.Type == CacheData) && cache.Level > best.cacheLevel &&
        cache.GroupMask.Group == currentProcessor.Group && (cache.GroupMask.Mask & currentMask) != 0) {
      GROUP_AFFINITY allowed{};
      KAFFINITY mask = cache.GroupMask.Mask;
      if (GetThreadGroupAffinity(GetCurrentThread(), &allowed) && allowed.Group == cache.GroupMask.Group) {
        mask &= allowed.Mask;
      }

      CacheDomain candidate;
      candidate.cacheLevel = cache.Level;
      constexpr uint32_t bits = sizeof(KAFFINITY) * 8;
      for (uint32_t number = 0; number < bits; ++number) {
        if ((mask & (KAFFINITY{1} << number)) != 0) {
          candidate.processors.push_back({.group = cache.GroupMask.Group, .number = number});
        }
      }
      if (!candidate.processors.empty()) {
        best = std::move(candidate);
      }
    }
    if (info->Size == 0) {
      break;
    }
    offset += info->Size;
  }

  if (best.processors.size() < 3 || (!forcePin && best.processors.size() >= sysInfo.dwNumberOfProcessors)) {
    return std::nullopt;
  }
  return best;
}

bool apply_cache_domain(const CacheDomain& domain) noexcept {
  GROUP_AFFINITY affinity{};
  affinity.Group = domain.processors.front().group;
  for (const auto& processor : domain.processors) {
    if (processor.group == affinity.Group && processor.number < sizeof(KAFFINITY) * 8) {
      affinity.Mask |= KAFFINITY{1} << processor.number;
    }
  }
  return SetThreadGroupAffinity(GetCurrentThread(), &affinity, nullptr) != 0;
}
#else
std::optional<CacheDomain> find_cache_domain() { return std::nullopt; }
bool apply_cache_domain(const CacheDomain&) noexcept { return false; }
#endif

void pin_shared_cache() noexcept {
  std::lock_guard lock{sDomainMutex};
  if (!sDomainConfigured) {
    auto domain = find_cache_domain();
    if (domain && apply_cache_domain(*domain)) {
      Log.info("Pinned threads to shared cache domain with {} cores", domain->processors.size());
      sDomain = std::move(domain);
    }
    sDomainConfigured = true;
    return;
  }
  if (sDomain) {
    apply_cache_domain(*sDomain);
  }
}

SDL_ThreadPriority to_sdl_priority(Priority priority) noexcept {
  switch (priority) {
  case Priority::Low:
    return SDL_THREAD_PRIORITY_LOW;
  case Priority::Normal:
    return SDL_THREAD_PRIORITY_NORMAL;
  case Priority::High:
    return SDL_THREAD_PRIORITY_HIGH;
  }
  return SDL_THREAD_PRIORITY_NORMAL;
}

void set_thread_name(const std::string& name) noexcept {
#if defined(_WIN32)
  /* SetThreadDescription is Windows 10 1607+, and mingw-w64 before v12 does
   * not declare it at all, so a cross build cannot call it directly.
   * Resolve it at runtime and skip naming where it is unavailable. */
  using SetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
  static const auto setThreadDescription = [] {
    const HMODULE kernelBase = GetModuleHandleW(L"KernelBase.dll");
    return kernelBase == nullptr ? nullptr
                                 : reinterpret_cast<SetThreadDescriptionFn>(reinterpret_cast<void*>(
                                       GetProcAddress(kernelBase, "SetThreadDescription")));
  }();
  if (setThreadDescription == nullptr) {
    return;
  }
  const int length = MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), nullptr, 0);
  if (length <= 0) {
    return;
  }
  std::wstring wideName;
  wideName.resize(static_cast<size_t>(length));
  MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), wideName.data(), length);
  setThreadDescription(GetCurrentThread(), wideName.c_str());
#elif defined(__APPLE__)
  const std::string truncated = name.substr(0, 63);
  pthread_setname_np(truncated.c_str());
#elif defined(__linux__)
  const std::string truncated = name.substr(0, 15);
  pthread_setname_np(pthread_self(), truncated.c_str());
#endif
}
} // namespace

void set_current(const Options& options) noexcept {
  if (!options.name.empty()) {
    set_thread_name(options.name);
#ifdef TRACY_ENABLE
    tracy::SetThreadName(options.name.c_str());
#endif
  }

  SDL_SetCurrentThreadPriority(to_sdl_priority(options.priority));
  if (options.affinity == Affinity::SharedCache) {
    pin_shared_cache();
  }
}

} // namespace aurora::thread
