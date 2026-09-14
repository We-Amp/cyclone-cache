// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#pragma once

#include <atomic>
#include <memory>
#include <shared_mutex>
#include <thread>
#include <vector>

#include "adaptive_pool.hpp"
#include "cyclone/config.hpp"
#include "cyclone/plugin/optimization.hpp"
#include "load_monitor.hpp"
#include "work_queue.hpp"

namespace cyclone {

class Cache;

// Statistics for the optimization engine
struct OptimizationStats {
  uint64_t queued = 0;
  uint64_t completed = 0;
  uint64_t cancelled = 0;
  uint64_t failed = 0;
  uint64_t skipped = 0;  // Skipped due to min_hits_before_optimize

  WorkQueueStats queue_stats;
  ThreadPoolStats pool_stats;
  LoadMetrics load_metrics;
};

// Main orchestrator for background optimization
class OptimizationEngine : public TaskProvider {
 public:
  OptimizationEngine(const OptimizationConfig &config, Cache *cache);
  ~OptimizationEngine() override;

  OptimizationEngine(const OptimizationEngine &) = delete;
  OptimizationEngine &operator=(const OptimizationEngine &) = delete;

  // Lifecycle
  void start();
  void stop();

  // Two-phase stop. request_stop() quiesces intake — stops the
  // WorkQueue, cancels active work, and signals the workers + monitor to exit
  // — without joining; it is non-blocking and safe to call under a lock the
  // optimization workers re-enter (they call Cache::read_sync(), which takes
  // the Cache's shared lock). join_threads() then joins the workers and the
  // monitor OUTSIDE that lock. Both idempotent; stop() is the two combined.
  void request_stop();
  void join_threads();

  bool is_running() const noexcept;

  // Plugin management
  void register_plugin(std::shared_ptr<OptimizationPlugin> plugin);
  void unregister_plugin(uint32_t plugin_id);
  std::vector<std::shared_ptr<OptimizationPlugin>> plugins() const;

  // Called when a write completes (from Cache)
  void on_write_complete(const CacheKey &key, std::span<const std::byte> header,
                         uint64_t content_length, AlternateId written_alternate,
                         uint32_t hit_count);

  // Access to sub-components
  LoadMonitor &load_monitor() { return _load_monitor; }
  const LoadMonitor &load_monitor() const { return _load_monitor; }

  WorkQueue &work_queue() { return _work_queue; }
  const WorkQueue &work_queue() const { return _work_queue; }

  // Manual control
  void pause();
  void resume();
  bool is_paused() const noexcept;

  // Get combined statistics
  OptimizationStats stats() const;

  // TaskProvider interface
  TaskFunction get_task() override;
  size_t queue_depth() const override;

 private:
  void process_work_item(WorkItem item);
  void monitoring_loop();

  OptimizationConfig _config;
  Cache *_cache;

  WorkQueue _work_queue;
  LoadMonitor _load_monitor;
  std::unique_ptr<AdaptiveThreadPool> _thread_pool;

  // Plugin list
  mutable std::shared_mutex _plugins_mutex;
  std::vector<std::shared_ptr<OptimizationPlugin>> _plugins;

  // Background monitoring thread (handles load checking and scaling)
  std::thread _monitoring_thread;

  // State
  std::atomic<bool> _running{false};
  std::atomic<bool> _paused{false};
  std::atomic<bool> _load_shedding{false};

  // Cooldown tracking for load shedding
  std::chrono::steady_clock::time_point _load_drop_time;
  bool _in_cooldown{false};

  // Statistics
  mutable std::mutex _stats_mutex;
  OptimizationStats _stats;

  // Cancellation tokens for in-progress work items
  mutable std::mutex _active_work_mutex;
  std::unordered_map<WorkItemId, std::shared_ptr<std::atomic<bool>>>
      _active_work;
};

}  // namespace cyclone
