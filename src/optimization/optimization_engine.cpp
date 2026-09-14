// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "optimization_engine.hpp"

#include <algorithm>
#include <chrono>

#include "cyclone/cache.hpp"

namespace cyclone {

// OptimizationContext implementation
OptimizationContext::OptimizationContext(
    const CacheKey &key, std::span<const std::byte> source_header,
    std::span<const std::byte> source_content, uint32_t hit_count, Cache *cache,
    std::atomic<bool> &cancelled)
    : _key(key),
      _source_header(source_header),
      _source_content(source_content),
      _hit_count(hit_count),
      _cache(cache),
      _cancelled(cancelled) {}

// OptimizationEngine implementation
OptimizationEngine::OptimizationEngine(const OptimizationConfig &config,
                                       Cache *cache)
    : _config(config),
      _cache(cache),
      _work_queue(config),
      _load_monitor(config) {
  _thread_pool = std::make_unique<AdaptiveThreadPool>(config, *this);
}

OptimizationEngine::~OptimizationEngine() { stop(); }

void OptimizationEngine::start() {
  if (!_config.enabled) {
    return;
  }

  if (_running.exchange(true, std::memory_order_acq_rel)) {
    return;  // Already running
  }

  // Start thread pool
  _thread_pool->start();

  // Start background monitoring thread
  _monitoring_thread = std::thread([this] { monitoring_loop(); });
}

void OptimizationEngine::stop() {
  request_stop();
  join_threads();
}

void OptimizationEngine::request_stop() {
  if (!_running.exchange(false, std::memory_order_acq_rel)) {
    return;  // Already stopped
  }

  // Quiesce intake WITHOUT joining.  Setting _running=false makes
  // on_write_complete() a no-op, and stopping the WorkQueue both unblocks any
  // worker parked in get_task() and refuses new enqueues — so once this
  // returns, no in-flight or future Cache operation feeds the engine.  This is
  // non-blocking and safe under the Cache's exclusive lock; the blocking joins
  // live in join_threads().
  _work_queue.stop();

  // Cancel all active work
  {
    std::lock_guard lock(_active_work_mutex);
    for (auto &[id, cancelled] : _active_work) {
      cancelled->store(true, std::memory_order_release);
    }
  }

  // Signal the pool workers to exit (no join here).
  _thread_pool->request_stop();
}

void OptimizationEngine::join_threads() {
  // Must run OUTSIDE the Cache's exclusive lock: a worker may be blocked
  // acquiring that lock inside Cache::read_sync(), and joining it under the
  // lock would deadlock.  request_stop() has already signalled
  // shutdown, so both joins complete.
  //
  // Join the MONITOR first, then the pool workers.  The monitor can call
  // _thread_pool->check_scaling() -> spawn_worker() in its final iteration;
  // once the monitor has exited it spawns no more, so the subsequent
  // join_workers() is genuinely terminal and reaps any worker the monitor just
  // spawned (teardown review).
  //
  // UNBOUNDED joins — not the bounded-join + leak pattern (see
  // thread_util.hpp). A leaked worker would dereference cache state freed by
  // teardown; a leaked monitor would dereference this engine's own members
  // after ~OptimizationEngine frees them.  The monitor loop re-checks
  // _running frequently and does no blocking I/O, so its join is prompt.  The
  // fork-inherited case never reaches here — Cache::stop() release()s the whole
  // engine in the child.  Idempotent.
  if (_monitoring_thread.joinable()) {
    _monitoring_thread.join();
  }

  _thread_pool->join_workers();
}

bool OptimizationEngine::is_running() const noexcept {
  return _running.load(std::memory_order_acquire);
}

void OptimizationEngine::register_plugin(
    std::shared_ptr<OptimizationPlugin> plugin) {
  std::unique_lock lock(_plugins_mutex);
  _plugins.push_back(std::move(plugin));
}

void OptimizationEngine::unregister_plugin(uint32_t plugin_id) {
  std::unique_lock lock(_plugins_mutex);
  _plugins.erase(std::remove_if(_plugins.begin(), _plugins.end(),
                                [plugin_id](const auto &p) {
                                  return p->info().plugin_id == plugin_id;
                                }),
                 _plugins.end());
}

std::vector<std::shared_ptr<OptimizationPlugin>> OptimizationEngine::plugins()
    const {
  std::shared_lock lock(_plugins_mutex);
  return _plugins;
}

void OptimizationEngine::on_write_complete(const CacheKey &key,
                                           std::span<const std::byte> header,
                                           uint64_t content_length,
                                           AlternateId written_alternate,
                                           uint32_t hit_count) {
  if (!_running.load(std::memory_order_acquire) || !_config.enabled) {
    return;
  }

  // Check minimum hits threshold
  if (hit_count < _config.min_hits_before_optimize) {
    std::lock_guard stats_lock(_stats_mutex);
    ++_stats.skipped;
    return;
  }

  // Get current plugins
  std::vector<std::shared_ptr<OptimizationPlugin>> current_plugins;
  {
    std::shared_lock lock(_plugins_mutex);
    current_plugins = _plugins;
  }

  // Ask each plugin what optimizations to queue
  for (const auto &plugin : current_plugins) {
    auto plan = plugin->plan_optimization(key, header, content_length,
                                          written_alternate, hit_count);

    for (const auto &target : plan.targets) {
      WorkItem item;
      item.id.key = key;
      item.id.target_alternate = target.target_alternate;
      item.plugin_id = plugin->info().plugin_id;
      item.priority = target.priority;
      item.hit_count = hit_count;
      item.content_length = content_length;
      // Get memory estimate and clamp to reasonable bounds (1KB - 1GB)
      constexpr size_t kMinMemoryEstimate = 1024;
      constexpr size_t kMaxMemoryEstimate = 1024ULL * 1024 * 1024;
      size_t mem_estimate = target.estimated_memory > 0
                                ? target.estimated_memory
                                : plugin->estimate_memory(
                                      content_length, target.target_alternate);
      item.estimated_memory =
          std::clamp(mem_estimate, kMinMemoryEstimate, kMaxMemoryEstimate);
      item.deferrable = target.deferrable;

      auto result = _work_queue.enqueue(item);
      if (result.has_value()) {
        std::lock_guard stats_lock(_stats_mutex);
        ++_stats.queued;
      }
    }
  }
}

void OptimizationEngine::pause() {
  _paused.store(true, std::memory_order_release);
  _thread_pool->pause();
}

void OptimizationEngine::resume() {
  _paused.store(false, std::memory_order_release);
  _load_shedding.store(false, std::memory_order_release);
  _thread_pool->resume();
}

bool OptimizationEngine::is_paused() const noexcept {
  return _paused.load(std::memory_order_acquire);
}

OptimizationStats OptimizationEngine::stats() const {
  std::lock_guard lock(_stats_mutex);
  OptimizationStats result = _stats;
  result.queue_stats = _work_queue.stats();
  result.pool_stats = _thread_pool->stats();
  result.load_metrics = _load_monitor.metrics();
  return result;
}

TaskFunction OptimizationEngine::get_task() {
  auto item = _work_queue.dequeue();
  if (!item) {
    return nullptr;  // Queue stopped
  }

  return [this, work_item = *item]() mutable { process_work_item(work_item); };
}

size_t OptimizationEngine::queue_depth() const { return _work_queue.size(); }

void OptimizationEngine::process_work_item(WorkItem item) {
  // Create cancellation token
  auto cancelled = std::make_shared<std::atomic<bool>>(false);
  {
    std::lock_guard lock(_active_work_mutex);
    _active_work[item.id] = cancelled;
  }

  // Find the plugin that queued this work item
  std::shared_ptr<OptimizationPlugin> plugin;
  {
    std::shared_lock lock(_plugins_mutex);
    for (const auto &p : _plugins) {
      if (p->info().plugin_id == item.plugin_id) {
        plugin = p;
        break;
      }
    }
  }

  if (!plugin) {
    std::lock_guard lock(_active_work_mutex);
    _active_work.erase(item.id);
    return;
  }

  // Read source content from cache.
  // Note: If the original content was evicted while this work item was queued,
  // the read will fail. This is expected behavior - we simply increment the
  // 'failed' stat and move on. The optimization can be re-queued on next
  // access.
  auto read_result = _cache->read_sync(item.id.key);
  if (!read_result) {
    std::lock_guard stats_lock(_stats_mutex);
    ++_stats.failed;
    std::lock_guard lock(_active_work_mutex);
    _active_work.erase(item.id);
    return;
  }

  auto &handle = *read_result;

  // Get header and content
  auto header_data = handle.header();
  auto content_data = handle.content();

  // Create optimization context
  OptimizationContext ctx(item.id.key, header_data, content_data,
                          item.hit_count, _cache, *cancelled);

  // Perform transformation
  auto transform_result = plugin->transform(item.id.target_alternate, ctx);

  // Remove from active work
  {
    std::lock_guard lock(_active_work_mutex);
    _active_work.erase(item.id);
  }

  if (!transform_result) {
    if (transform_result.error() == CacheError::OptimizationCancelled) {
      plugin->on_cancelled(item.id.key, item.id.target_alternate);
      std::lock_guard stats_lock(_stats_mutex);
      ++_stats.cancelled;
    } else {
      std::lock_guard stats_lock(_stats_mutex);
      ++_stats.failed;
    }
    return;
  }

  // Write the new alternate to cache
  auto write_result =
      _cache->write_alternate_sync(item.id.key, transform_result->alternate_id,
                                   transform_result->content.size());

  if (!write_result) {
    std::lock_guard stats_lock(_stats_mutex);
    ++_stats.failed;
    return;
  }

  auto &write_handle = *write_result;

  // Write header
  write_handle.set_header(transform_result->header);

  // Write content
  auto write_content_result =
      write_handle.write_sync(transform_result->content);
  if (!write_content_result) {
    write_handle.abort();
    std::lock_guard stats_lock(_stats_mutex);
    ++_stats.failed;
    return;
  }

  // Close/commit
  auto close_result = write_handle.close_sync();
  if (!close_result) {
    std::lock_guard stats_lock(_stats_mutex);
    ++_stats.failed;
    return;
  }

  std::lock_guard stats_lock(_stats_mutex);
  ++_stats.completed;
}

void OptimizationEngine::monitoring_loop() {
  while (_running.load(std::memory_order_acquire)) {
    // Sleep in small increments so stop()'s unconditional join stays
    // responsive even when scale_check_interval is large.
    auto sleep_end =
        std::chrono::steady_clock::now() + _config.scale_check_interval;
    while (_running.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < sleep_end) {
      std::this_thread::sleep_for(std::chrono::milliseconds(
          std::min<int64_t>(100, _config.scale_check_interval.count())));
    }

    if (!_running.load(std::memory_order_acquire)) {
      break;
    }

    // Update load metrics
    _load_monitor.update_metrics();

    // Check for load shedding
    if (_load_monitor.is_above_high_watermark()) {
      _in_cooldown = false;  // Reset cooldown if load goes back up
      if (!_load_shedding.exchange(true, std::memory_order_acq_rel)) {
        // Just crossed above high watermark
        _thread_pool->pause();

        // Cancel deferrable work
        size_t cancelled = _work_queue.cancel_deferrable();
        if (cancelled > 0) {
          std::lock_guard lock(_stats_mutex);
          _stats.cancelled += cancelled;
        }
      }
    } else if (_load_shedding.load(std::memory_order_acquire) &&
               _load_monitor.is_below_low_watermark()) {
      // Crossed below low watermark, start cooldown if not already started
      auto now = std::chrono::steady_clock::now();
      if (!_in_cooldown) {
        _in_cooldown = true;
        _load_drop_time = now;
      } else if (now - _load_drop_time >= _config.load_shedding_cooldown) {
        // Cooldown period elapsed, can resume
        _in_cooldown = false;
        _load_shedding.store(false, std::memory_order_release);
        _thread_pool->resume();
      }
    } else if (_load_shedding.load(std::memory_order_acquire)) {
      // Load is between watermarks, reset cooldown
      _in_cooldown = false;
    }

    // Trigger autoscaling check (only if not paused or load shedding)
    if (!_paused.load(std::memory_order_acquire) &&
        !_load_shedding.load(std::memory_order_acquire)) {
      _thread_pool->check_scaling();
    }
  }
}

}  // namespace cyclone
