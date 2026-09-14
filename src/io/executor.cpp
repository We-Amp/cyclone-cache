// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Async I/O executor for cache operations.
// Provides a simple thread pool for executing I/O operations asynchronously.

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "../core/thread_util.hpp"
#include "cyclone/task.hpp"

namespace cyclone {

class Executor {
 public:
  explicit Executor(size_t num_threads = 0);
  ~Executor();

  Executor(const Executor &) = delete;
  Executor &operator=(const Executor &) = delete;

  void submit(std::function<void()> task);
  void stop();
  [[nodiscard]] bool is_running() const { return _running; }

  [[nodiscard]] size_t pending_count() const;
  [[nodiscard]] size_t thread_count() const { return _threads.size(); }

  static Executor &global();

 private:
  std::vector<std::thread> _threads;
  std::queue<std::function<void()>> _tasks;
  std::mutex _mutex;
  std::condition_variable _cv;
  std::atomic<bool> _running{true};
  std::atomic<size_t> _live_count{0};

  void worker_loop();
};

Executor::Executor(size_t num_threads) {
  if (num_threads == 0) {
    num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) {
      num_threads = 4;
    }
  }

  for (size_t i = 0; i < num_threads; ++i) {
    _threads.emplace_back([this] { worker_loop(); });
  }
}

Executor::~Executor() { stop(); }

void Executor::stop() {
  {
    std::lock_guard<std::mutex> lock(_mutex);
    _running = false;
  }
  _cv.notify_all();

  // NOTE (teardown audit): unlike the Cache-owned runtime components
  // (HitTracker / OptimizationEngine / AdaptiveThreadPool), which were switched
  // to unconditional joins, this Executor's only intended lifecycle is the
  // process-lifetime global() singleton torn down at true atexit — the one case
  // the leak_thread_on_shutdown contract is written for (the C runtime may have
  // already killed the workers, making join()/detach() UB).  So the bounded
  // wait + leak below is deliberately retained here.  (It is also currently
  // unwired: global() has no callers, so this path does not run in production.)
  //
  // Wait for all workers to exit their loops before joining.
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (_live_count.load(std::memory_order_acquire) > 0) {
    if (std::chrono::steady_clock::now() > deadline) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  bool all_exited = (_live_count.load(std::memory_order_acquire) == 0);

  for (auto &t : _threads) {
    if (all_exited && t.joinable()) {
      t.join();
    } else if (t.joinable()) {
      leak_thread_on_shutdown(std::move(t));
    }
  }
  _threads.clear();
}

void Executor::submit(std::function<void()> task) {
  {
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_running) {
      return;
    }
    _tasks.push(std::move(task));
  }
  _cv.notify_one();
}

size_t Executor::pending_count() const {
  std::lock_guard<std::mutex> lock(const_cast<std::mutex &>(_mutex));
  return _tasks.size();
}

void Executor::worker_loop() {
  _live_count.fetch_add(1, std::memory_order_relaxed);
  while (true) {
    std::function<void()> task;
    {
      std::unique_lock<std::mutex> lock(_mutex);
      _cv.wait(lock, [this] { return !_running || !_tasks.empty(); });

      if (!_running && _tasks.empty()) {
        _live_count.fetch_sub(1, std::memory_order_relaxed);
        return;
      }

      if (!_tasks.empty()) {
        task = std::move(_tasks.front());
        _tasks.pop();
      }
    }

    if (task) {
      task();
    }
  }
}

Executor &Executor::global() {
  static Executor instance;
  return instance;
}

}  // namespace cyclone
