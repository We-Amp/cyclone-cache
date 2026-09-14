// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "load_monitor.hpp"

#include <algorithm>

namespace cyclone {

LoadMonitor::LoadMonitor(const OptimizationConfig &config)
    : _config(config), _last_update(std::chrono::steady_clock::now()) {}

void LoadMonitor::record_read_complete(uint64_t bytes) {
  _read_ops.fetch_add(1, std::memory_order_relaxed);
  _read_bytes.fetch_add(bytes, std::memory_order_relaxed);
}

void LoadMonitor::record_write_complete(uint64_t bytes) {
  _write_ops.fetch_add(1, std::memory_order_relaxed);
  _write_bytes.fetch_add(bytes, std::memory_order_relaxed);
}

void LoadMonitor::update_metrics() {
  auto now = std::chrono::steady_clock::now();

  // Get current counter values
  uint64_t read_ops = _read_ops.load(std::memory_order_relaxed);
  uint64_t write_ops = _write_ops.load(std::memory_order_relaxed);
  uint64_t read_bytes = _read_bytes.load(std::memory_order_relaxed);
  uint64_t write_bytes = _write_bytes.load(std::memory_order_relaxed);

  // Calculate elapsed time
  auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(now - _last_update);
  double elapsed_sec = elapsed.count() / 1000.0;

  // Avoid division by zero
  if (elapsed_sec < 0.001) {
    return;
  }

  // Calculate deltas
  uint64_t delta_read_ops = read_ops - _prev_read_ops;
  uint64_t delta_write_ops = write_ops - _prev_write_ops;
  uint64_t delta_read_bytes = read_bytes - _prev_read_bytes;
  uint64_t delta_write_bytes = write_bytes - _prev_write_bytes;

  // Calculate rates
  double read_ops_per_sec = delta_read_ops / elapsed_sec;
  double write_ops_per_sec = delta_write_ops / elapsed_sec;
  double read_bytes_per_sec = delta_read_bytes / elapsed_sec;
  double write_bytes_per_sec = delta_write_bytes / elapsed_sec;

  // Estimate load
  double total_ops_per_sec = read_ops_per_sec + write_ops_per_sec;
  double total_bytes_per_sec = read_bytes_per_sec + write_bytes_per_sec;

  double cpu_usage =
      std::min(1.0, total_ops_per_sec / _config.baseline_ops_per_sec);
  double io_usage =
      std::min(1.0, total_bytes_per_sec / _config.baseline_bytes_per_sec);
  double combined = kCpuWeight * cpu_usage + kIoWeight * io_usage;

  // Update metrics
  {
    std::lock_guard lock(_metrics_mutex);
    _metrics.read_ops = read_ops;
    _metrics.write_ops = write_ops;
    _metrics.read_bytes = read_bytes;
    _metrics.write_bytes = write_bytes;

    _metrics.read_ops_per_sec = read_ops_per_sec;
    _metrics.write_ops_per_sec = write_ops_per_sec;
    _metrics.read_bytes_per_sec = read_bytes_per_sec;
    _metrics.write_bytes_per_sec = write_bytes_per_sec;

    _metrics.cpu_usage = cpu_usage;
    _metrics.io_usage = io_usage;
    _metrics.combined = combined;
  }

  // Save current values for next calculation
  _prev_read_ops = read_ops;
  _prev_write_ops = write_ops;
  _prev_read_bytes = read_bytes;
  _prev_write_bytes = write_bytes;
  _last_update = now;
}

LoadMetrics LoadMonitor::metrics() const {
  std::lock_guard lock(_metrics_mutex);
  return _metrics;
}

double LoadMonitor::current_load() const {
  std::lock_guard lock(_metrics_mutex);
  return _metrics.combined;
}

bool LoadMonitor::is_above_high_watermark() const {
  return current_load() > _config.load_high_watermark;
}

bool LoadMonitor::is_below_low_watermark() const {
  return current_load() < _config.load_low_watermark;
}

void LoadMonitor::reset() {
  _read_ops.store(0, std::memory_order_relaxed);
  _write_ops.store(0, std::memory_order_relaxed);
  _read_bytes.store(0, std::memory_order_relaxed);
  _write_bytes.store(0, std::memory_order_relaxed);

  _prev_read_ops = 0;
  _prev_write_ops = 0;
  _prev_read_bytes = 0;
  _prev_write_bytes = 0;
  _last_update = std::chrono::steady_clock::now();

  {
    std::lock_guard lock(_metrics_mutex);
    _metrics = LoadMetrics{};
  }
}

}  // namespace cyclone
