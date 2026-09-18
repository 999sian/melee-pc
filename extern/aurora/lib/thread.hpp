#pragma once

#include <concepts>
#include <functional>
#include <version>
// Apple's libc++ ships <stop_token> long before it exposes std::stop_token
// and std::jthread (they stay experimental until the macOS 26 SDK), so key on
// the feature-test macro rather than on the header being present.
#if defined(__cpp_lib_jthread) && __cpp_lib_jthread >= 201911L
#include <stop_token>
#include <thread>
#define AURORA_HAS_STOP_TOKEN 1
#endif

#ifndef AURORA_HAS_STOP_TOKEN
#define AURORA_HAS_STOP_TOKEN 0
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>
namespace std {
struct stop_state {
  std::atomic<bool> stop_requested{false};
  std::vector<std::function<void()>> callbacks;
  std::mutex mutex;
};
struct stop_token {
  std::shared_ptr<stop_state> state;
  [[nodiscard]] bool stop_requested() const noexcept {
    return state ? state->stop_requested.load(std::memory_order_acquire) : false;
  }
};
template <typename Callback>
struct stop_callback {
  explicit stop_callback(const stop_token& token, Callback&& cb) {
    if (token.state) {
      std::lock_guard lock{token.state->mutex};
      if (token.state->stop_requested.load(std::memory_order_relaxed)) {
        cb();
      } else {
        token.state->callbacks.push_back(std::forward<Callback>(cb));
      }
    }
  }
};
}
#endif

#include <string>
#include <thread>
#include <type_traits>
#include <utility>

namespace aurora::thread {

enum class Priority { Low, Normal, High };
enum class Affinity { None, SharedCache };

struct Options {
  std::string name;
  Priority priority = Priority::Normal;
  Affinity affinity = Affinity::None;
};

// Applies thread options to the current thread
void set_current(const Options& options) noexcept;

class Thread {
public:
  Thread() noexcept = default;

#if AURORA_HAS_STOP_TOKEN
  template <typename Function>
    requires std::invocable<std::decay_t<Function>&, std::stop_token>
  explicit Thread(Options options, Function&& function)
  : mThread{[options = std::move(options), function = std::forward<Function>(function)](std::stop_token token) mutable {
    set_current(options);
    std::invoke(function, token);
  }} {}
#else
  template <typename Function>
  explicit Thread(Options options, Function&& function)
  : mStopState{std::make_shared<std::stop_state>()}
  , mThread{[options = std::move(options), function = std::forward<Function>(function), state = mStopState]() mutable {
    set_current(options);
    std::stop_token token{state};
    std::invoke(function, token);
  }} {}
#endif

  Thread(Thread&&) noexcept = default;
  Thread& operator=(Thread&&) noexcept = default;
  Thread(const Thread&) = delete;
  Thread& operator=(const Thread&) = delete;
  ~Thread() {
#if !AURORA_HAS_STOP_TOKEN
    request_stop();
    if (mThread.joinable()) mThread.join();
#endif
  }

  [[nodiscard]] bool joinable() const noexcept { return mThread.joinable(); }
  [[nodiscard]] std::thread::id get_id() const noexcept { return mThread.get_id(); }
#if AURORA_HAS_STOP_TOKEN
  [[nodiscard]] std::stop_token get_stop_token() const noexcept { return mThread.get_stop_token(); }
  bool request_stop() noexcept { return mThread.request_stop(); }
  std::jthread::native_handle_type native_handle() { return mThread.native_handle(); }
#else
  [[nodiscard]] std::stop_token get_stop_token() const noexcept { return {mStopState}; }
  bool request_stop() noexcept {
    if (!mStopState || mStopState->stop_requested.exchange(true, std::memory_order_acq_rel)) {
      return false;
    }
    std::vector<std::function<void()>> cbs;
    {
      std::lock_guard lock{mStopState->mutex};
      cbs = mStopState->callbacks;
    }
    for (auto& cb : cbs) {
      cb();
    }
    return true;
  }
  std::thread::native_handle_type native_handle() { return mThread.native_handle(); }
#endif

  void join() { mThread.join(); }

private:
#if AURORA_HAS_STOP_TOKEN
  std::jthread mThread;
#else
  std::shared_ptr<std::stop_state> mStopState;
  std::thread mThread;
#endif
};

} // namespace aurora::thread
