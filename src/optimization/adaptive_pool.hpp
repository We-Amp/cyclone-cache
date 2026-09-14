// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "cyclone/config.hpp"

namespace cyclone {

// Statistics about the thread pool
struct ThreadPoolStats {
  size_t current_threads = 0;
  size_t active_threads = 0;
  uint64_t tasks_started = 0;
  uint64_t tasks_completed = 0;
  uint64_t scale_up_count = 0;
  uint64_t scale_down_count = 0;
};

// Task function type
using TaskFunction = std::function<void()>;

// Provides tasks to the thread pool
class TaskProvider {
 public:
  virtual ~TaskProvider() = default;

  // Get the next task to execute. Should block until a task is available or
  // stop is signaled. Return nullptr to indicate the worker should exit.
  virtual TaskFunction get_task() = 0;

  // Get current queue depth (for autoscaling decisions)
  [[nodiscard]] virtual size_t queue_depth() const = 0;
};

class AdaptiveThreadPool {
 public:
  AdaptiveThreadPool(const OptimizationConfig &config, TaskProvider &provider);
  ~AdaptiveThreadPool();

  AdaptiveThreadPool(const AdaptiveThreadPool &) = delete;
  AdaptiveThreadPool &operator=(const AdaptiveThreadPool &) = delete;

  // Start the thread pool with initial threads
  void start();

  // Stop all threads gracefully (signal + join). Equivalent to
  // request_stop() followed by join_workers().
  void stop();

  // Two-phase stop for callers that must join OUTSIDE a lock the workers may
  // re-enter (see Cache::stop()). request_stop() only SIGNALS the
  // workers to exit (non-blocking, safe under such a lock); join_workers()
  // blocks until they have. Both are idempotent.
  void request_stop();
  void join_workers();

  // Pause all workers (they finish current task then wait)
  void pause();

  // Resume paused workers
  void resume();

  // Check if workers are paused
  bool is_paused() const noexcept;

  // Check if pool is running
  bool is_running() const noexcept;

  // Get current thread count
  size_t thread_count() const noexcept;

  // Get count of threads actively executing tasks
  size_t active_count() const noexcept;

  // Trigger scaling check (call periodically from external timer)
  void check_scaling();

  // Force scale to specific thread count
  void scale_to(size_t target);

  // Get statistics
  ThreadPoolStats stats() const;

 private:
  void worker_loop(size_t worker_id);
  // Spawn a new worker thread. Requires: _workers_mutex must be held by caller.
  void spawn_worker();
  bool should_scale_up() const;
  bool should_scale_down() const;

  OptimizationConfig _config;
  TaskProvider &_provider;

  std::vector<std::thread> _workers;
  mutable std::mutex _workers_mutex;

  std::atomic<bool> _running{false};
  std::atomic<bool> _paused{false};
  std::atomic<bool> _stopping{false};
  std::atomic<size_t> _target_thread_count{0};
  std::atomic<size_t> _active_count{0};
  std::atomic<size_t> _live_count{0};

  std::mutex _pause_mutex;
  std::condition_variable _pause_cv;

  mutable std::mutex _stats_mutex;
  ThreadPoolStats _stats;
};

}  // namespace cyclone
