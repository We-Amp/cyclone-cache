// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "adaptive_pool.hpp"

#include <algorithm>

// Thread cancellation on glibc is a forced unwind (abi::__forced_unwind),
// which must not be swallowed: the runtime aborts the process if it is.  The
// catch-all guards below rethrow it.
#if defined(__GLIBCXX__) && __has_include(<cxxabi.h>)
#include <cxxabi.h>
#define CYCLONE_OPT_FORCED_UNWIND 1
#else
#define CYCLONE_OPT_FORCED_UNWIND 0
#endif

namespace cyclone {

AdaptiveThreadPool::AdaptiveThreadPool(const OptimizationConfig &config,
                                       TaskProvider &provider)
    : _config(config), _provider(provider) {}

AdaptiveThreadPool::~AdaptiveThreadPool() { stop(); }

void AdaptiveThreadPool::start() {
  if (_running.exchange(true, std::memory_order_acq_rel)) {
    return;  // Already running
  }

  _stopping.store(false, std::memory_order_release);
  _paused.store(false, std::memory_order_release);

  size_t initial = _config.min_threads;
  _target_thread_count.store(initial, std::memory_order_release);

  std::lock_guard lock(_workers_mutex);
  _workers.reserve(_config.effective_max_threads());

  for (size_t i = 0; i < initial; ++i) {
    spawn_worker();
  }

  {
    std::lock_guard stats_lock(_stats_mutex);
    _stats.current_threads = _workers.size();
  }
}

void AdaptiveThreadPool::stop() {
  request_stop();
  join_workers();
}

void AdaptiveThreadPool::request_stop() {
  if (!_running.exchange(false, std::memory_order_acq_rel)) {
    return;  // Already stopped
  }

  // Signal every worker to exit WITHOUT joining.  Safe to call while holding a
  // lock the workers may re-enter: this only flips flags and
  // notifies the pause CV.  The caller (OptimizationEngine::request_stop())
  // stops the WorkQueue first, unblocking any worker parked in get_task().
  _stopping.store(true, std::memory_order_release);
  _paused.store(false, std::memory_order_release);
  _pause_cv.notify_all();
}

void AdaptiveThreadPool::join_workers() {
  // UNBOUNDED join — not a bounded-wait-then-leak pattern.
  // A worker executes cache-touching optimization tasks (task() reads/writes
  // volumes through the engine); a leaked-but-still- running worker abandoned
  // on a timeout would then dereference cache state freed by teardown.  Given
  // request_stop() + the WorkQueue stop, every worker observes shutdown and
  // returns, so the join completes.  Must run OUTSIDE any lock the workers
  // re-enter, else it deadlocks against a worker blocked acquiring that lock.
  // The fork-inherited case never reaches here — Cache::stop() release()s the
  // owning engine in the child.  Idempotent: a second call finds _workers
  // already empty.
  std::vector<WorkerSlot> workers_to_join;
  {
    std::lock_guard lock(_workers_mutex);
    workers_to_join = std::move(_workers);
    _workers.clear();
  }

  for (auto &worker : workers_to_join) {
    if (worker.thread.joinable()) {
      worker.thread.join();
    }
  }

  // Workers register their exit while the loop above joins them, so drop the
  // registrations only now that every one of them has returned.  Clearing
  // before the join would leave entries naming already-joined workers for the
  // next reap to find.
  {
    std::lock_guard lock(_workers_mutex);
    _finished_workers.clear();
  }

  _live_count.store(0, std::memory_order_release);
  _active_count.store(0, std::memory_order_release);
  _target_thread_count.store(0, std::memory_order_release);
}

void AdaptiveThreadPool::pause() {
  _paused.store(true, std::memory_order_release);
}

void AdaptiveThreadPool::resume() {
  _paused.store(false, std::memory_order_release);
  _pause_cv.notify_all();
}

bool AdaptiveThreadPool::is_paused() const noexcept {
  return _paused.load(std::memory_order_acquire);
}

bool AdaptiveThreadPool::is_running() const noexcept {
  return _running.load(std::memory_order_acquire);
}

size_t AdaptiveThreadPool::thread_count() const noexcept {
  return _live_count.load(std::memory_order_acquire);
}

size_t AdaptiveThreadPool::active_count() const noexcept {
  return _active_count.load(std::memory_order_acquire);
}

void AdaptiveThreadPool::check_scaling() {
  // Also bail once stopping: this runs from the OptimizationEngine monitor,
  // which may call check_scaling() during teardown before it observes the
  // engine's stop.  Spawning a worker after join_workers() would break the
  // "phase-2 join is terminal" invariant (teardown review).
  if (!_running.load(std::memory_order_acquire) ||
      _stopping.load(std::memory_order_acquire) ||
      _paused.load(std::memory_order_acquire)) {
    return;
  }

  if (should_scale_up()) {
    size_t current = _target_thread_count.load(std::memory_order_acquire);
    size_t max = _config.effective_max_threads();
    if (current < max) {
      size_t new_target = std::min(current + 1, max);
      _target_thread_count.store(new_target, std::memory_order_release);

      std::lock_guard lock(_workers_mutex);
      reap_finished_workers();
      // Gate on the LIVE count, not _workers.size(): self-terminated workers
      // would otherwise keep their entries until shutdown and pin the size at
      // the historical maximum, blocking every later scale-up below it.
      if (_live_count.load(std::memory_order_acquire) < new_target) {
        spawn_worker();
        std::lock_guard stats_lock(_stats_mutex);
        ++_stats.scale_up_count;
      }
    }
  } else if (should_scale_down()) {
    size_t current = _target_thread_count.load(std::memory_order_acquire);
    size_t min = _config.min_threads;
    if (current > min) {
      size_t new_target = std::max(current - 1, min);
      _target_thread_count.store(new_target, std::memory_order_release);

      std::lock_guard stats_lock(_stats_mutex);
      ++_stats.scale_down_count;
    }
  }
}

void AdaptiveThreadPool::scale_to(size_t target) {
  size_t min = _config.min_threads;
  size_t max = _config.effective_max_threads();
  target = std::clamp(target, min, max);

  _target_thread_count.store(target, std::memory_order_release);

  std::lock_guard lock(_workers_mutex);
  reap_finished_workers();
  // Gate on the LIVE count (see check_scaling); spawn_worker() counts each
  // new worker as live at once.
  while (_live_count.load(std::memory_order_acquire) < target) {
    spawn_worker();
  }

  {
    std::lock_guard stats_lock(_stats_mutex);
    _stats.current_threads = _workers.size();
  }
}

ThreadPoolStats AdaptiveThreadPool::stats() const {
  std::lock_guard lock(_stats_mutex);
  ThreadPoolStats result = _stats;
  result.current_threads = _live_count.load(std::memory_order_relaxed);
  result.active_threads = _active_count.load(std::memory_order_relaxed);
  return result;
}

void AdaptiveThreadPool::worker_loop(uint64_t generation) {
  // spawn_worker() already counted this worker as live.
  while (!_stopping.load(std::memory_order_acquire)) {
    // Check if we should self-terminate (scaling down)
    size_t live = _live_count.load(std::memory_order_acquire);
    size_t target = _target_thread_count.load(std::memory_order_acquire);
    if (live > target) {
      // Try to be the one to exit
      if (_live_count.compare_exchange_strong(live, live - 1,
                                              std::memory_order_acq_rel)) {
        // Register the exit so the spawn paths can reap this entry from
        // _workers; the thread object is joined there, not here.
        {
          std::lock_guard lock(_workers_mutex);
          _finished_workers.push_back(generation);
        }
        return;  // Exit this worker
      }
      // Another thread won the race, continue
    }

    // Wait if paused
    if (_paused.load(std::memory_order_acquire)) {
      std::unique_lock lock(_pause_mutex);
      _pause_cv.wait(lock, [this] {
        return !_paused.load(std::memory_order_acquire) ||
               _stopping.load(std::memory_order_acquire);
      });
      if (_stopping.load(std::memory_order_acquire)) {
        break;
      }
      continue;
    }

    // Get and execute task
    auto task = _provider.get_task();
    if (!task) {
      break;  // Provider signaled shutdown
    }

    _active_count.fetch_add(1, std::memory_order_relaxed);
    {
      std::lock_guard stats_lock(_stats_mutex);
      ++_stats.tasks_started;
    }

    try {
      task();
      std::lock_guard stats_lock(_stats_mutex);
      ++_stats.tasks_completed;
#if CYCLONE_OPT_FORCED_UNWIND
    } catch (abi::__forced_unwind &) {
      throw;
#endif
    } catch (...) {
      // Tasks run embedder-supplied code (plugin transforms); an exception
      // escaping worker_loop would terminate the host process.  Record the
      // failure and keep serving later tasks.
      std::lock_guard stats_lock(_stats_mutex);
      ++_stats.tasks_failed;
    }

    _active_count.fetch_sub(1, std::memory_order_relaxed);
  }

  _live_count.fetch_sub(1, std::memory_order_relaxed);
  {
    std::lock_guard lock(_workers_mutex);
    _finished_workers.push_back(generation);
  }
}

void AdaptiveThreadPool::reap_finished_workers() {
  for (const auto generation : _finished_workers) {
    auto it = std::find_if(_workers.begin(), _workers.end(),
                           [generation](const WorkerSlot &w) {
                             return w.generation == generation;
                           });
    if (it != _workers.end()) {
      // The worker pushed its generation on the way out of worker_loop and
      // never touches _workers_mutex afterwards, so this join waits at most
      // for the last few instructions of the thread; it cannot deadlock here.
      // Generations are never reused, so the entry cannot name a live worker.
      it->thread.join();
      _workers.erase(it);
    }
  }
  _finished_workers.clear();
}

void AdaptiveThreadPool::spawn_worker() {
  const uint64_t generation = _next_generation++;
  // Count the worker as live before it is first scheduled, so the spawn
  // paths (which gate on the live count) never spawn twice for it.
  _live_count.fetch_add(1, std::memory_order_acq_rel);
  try {
    _workers.push_back({generation, std::thread([this, generation] {
                          worker_loop(generation);
                        })});
  } catch (...) {
    // No worker was started: take the count back.
    _live_count.fetch_sub(1, std::memory_order_acq_rel);
    throw;
  }
}

bool AdaptiveThreadPool::should_scale_up() const {
  size_t depth = _provider.queue_depth();
  size_t threads = _live_count.load(std::memory_order_relaxed);
  size_t threshold = _config.scale_up_threshold;

  if (threads == 0) {
    return depth > 0;
  }

  return depth > threads * threshold;
}

bool AdaptiveThreadPool::should_scale_down() const {
  size_t depth = _provider.queue_depth();
  size_t threads = _live_count.load(std::memory_order_relaxed);
  size_t threshold = _config.scale_down_threshold;

  if (threads <= _config.min_threads) {
    return false;
  }

  return depth < threads * threshold;
}

}  // namespace cyclone
