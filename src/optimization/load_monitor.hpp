// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>

#include "../core/thread_shard.hpp"
#include "../core/write_commit_observer.hpp"
#include "cyclone/config.hpp"

namespace cyclone {

struct LoadMetrics {
  double cpu_usage = 0.0;  // Estimated CPU usage (0.0-1.0)
  double io_usage = 0.0;   // Estimated I/O usage (0.0-1.0)
  double combined = 0.0;   // Combined load (0.0-1.0)

  uint64_t read_ops = 0;
  uint64_t write_ops = 0;
  uint64_t read_bytes = 0;
  uint64_t write_bytes = 0;

  double read_ops_per_sec = 0.0;
  double write_ops_per_sec = 0.0;
  double read_bytes_per_sec = 0.0;
  double write_bytes_per_sec = 0.0;
};

class LoadMonitor final : public WriteCommitObserver {
 public:
  explicit LoadMonitor(const OptimizationConfig &config);
  ~LoadMonitor() = default;

  LoadMonitor(const LoadMonitor &) = delete;
  LoadMonitor &operator=(const LoadMonitor &) = delete;

  // Record completed operations.  Called from the cache hot paths: relaxed
  // increments on this thread's counter shard, so concurrent readers do not
  // contend on one cache line.  update_metrics() sums the shards.
  void record_read_complete(uint64_t bytes) {
    auto &shard = _shards[thread_shard_index(kCounterShards)];
    shard.read_ops.fetch_add(1, std::memory_order_relaxed);
    shard.read_bytes.fetch_add(bytes, std::memory_order_relaxed);
  }
  void record_write_complete(uint64_t bytes) {
    auto &shard = _shards[thread_shard_index(kCounterShards)];
    shard.write_ops.fetch_add(1, std::memory_order_relaxed);
    shard.write_bytes.fetch_add(bytes, std::memory_order_relaxed);
  }

  // WriteCommitObserver: a cache write handle committed `bytes`.
  void on_write_committed(uint64_t bytes) noexcept override {
    record_write_complete(bytes);
  }

  // Update metrics (call periodically, e.g., every second)
  void update_metrics();

  // Get current load metrics
  LoadMetrics metrics() const;

  // Get combined load value (weighted average of CPU and I/O)
  double current_load() const;

  // Check against watermarks
  bool is_above_high_watermark() const;
  bool is_below_low_watermark() const;

  // Reset counters (for testing only - not thread-safe with update_metrics())
  // Must be called only when no other thread is calling update_metrics()
  void reset();

 private:
  OptimizationConfig _config;

  // Counters incremented in hot paths, sharded per thread and padded so
  // each shard sits on its own cache lines.  A power of two (see
  // thread_shard_index).
  static constexpr size_t kCounterShards = 16;
  struct alignas(kShardPad) CounterShard {
    std::atomic<uint64_t> read_ops{0};
    std::atomic<uint64_t> write_ops{0};
    std::atomic<uint64_t> read_bytes{0};
    std::atomic<uint64_t> write_bytes{0};
  };
  std::array<CounterShard, kCounterShards> _shards;

  // Previous values for rate calculation
  uint64_t _prev_read_ops{0};
  uint64_t _prev_write_ops{0};
  uint64_t _prev_read_bytes{0};
  uint64_t _prev_write_bytes{0};
  std::chrono::steady_clock::time_point _last_update;

  // Computed metrics
  mutable std::mutex _metrics_mutex;
  LoadMetrics _metrics;

  // Weights for combined load calculation
  static constexpr double kCpuWeight = 0.3;
  static constexpr double kIoWeight = 0.7;
};

}  // namespace cyclone
