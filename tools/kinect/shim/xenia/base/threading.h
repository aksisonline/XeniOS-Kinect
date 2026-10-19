// Harness shim for xenia/base/threading.h
#pragma once
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>
namespace xe::threading {
struct Thread {
  std::thread t;
  void set_name(const char*) {}
  static std::unique_ptr<Thread> Create(int, std::function<void()> fn) {
    auto th = std::make_unique<Thread>();
    th->t = std::thread(std::move(fn));
    return th;
  }
};
template <typename D> inline void Sleep(D d) { std::this_thread::sleep_for(d); }
inline void Wait(Thread* th, bool) { if (th->t.joinable()) th->t.join(); }
}  // namespace xe::threading
