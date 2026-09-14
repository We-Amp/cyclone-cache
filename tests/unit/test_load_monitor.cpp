// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <thread>

#include "optimization/load_monitor.hpp"

using namespace cyclone;

namespace {

OptimizationConfig default_config() {
  OptimizationConfig config;
  config.load_high_watermark = 0.8;
  config.load_low_watermark = 0.5;
  return config;
}

}  // namespace

TEST_CASE("LoadMonitor initialization", "[load][optimization]") {
  OptimizationConfig config = default_config();
  LoadMonitor monitor(config);

  auto metrics = monitor.metrics();
  REQUIRE(metrics.cpu_usage == 0.0);
  REQUIRE(metrics.io_usage == 0.0);
  REQUIRE(metrics.combined == 0.0);
  REQUIRE(metrics.read_ops == 0);
  REQUIRE(metrics.write_ops == 0);
}

TEST_CASE("LoadMonitor records operations", "[load][optimization]") {
  OptimizationConfig config = default_config();
  LoadMonitor monitor(config);

  monitor.record_read_complete(1024);
  monitor.record_read_complete(2048);
  monitor.record_write_complete(512);

  // Wait a bit and update
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  monitor.update_metrics();

  auto metrics = monitor.metrics();
  REQUIRE(metrics.read_ops == 2);
  REQUIRE(metrics.write_ops == 1);
  REQUIRE(metrics.read_bytes == 3072);
  REQUIRE(metrics.write_bytes == 512);
}

TEST_CASE("LoadMonitor calculates rates", "[load][optimization]") {
  OptimizationConfig config = default_config();
  LoadMonitor monitor(config);

  // Record some operations
  for (int i = 0; i < 100; ++i) {
    monitor.record_read_complete(1_KB);
  }

  // Wait and update
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  monitor.update_metrics();

  auto metrics = monitor.metrics();

  // Should have calculated positive rates
  REQUIRE(metrics.read_ops_per_sec > 0.0);
  REQUIRE(metrics.read_bytes_per_sec > 0.0);
  REQUIRE(metrics.write_ops_per_sec == 0.0);
}

TEST_CASE("LoadMonitor watermark checks", "[load][optimization]") {
  OptimizationConfig config;
  config.load_high_watermark = 0.3;
  config.load_low_watermark = 0.1;
  LoadMonitor monitor(config);

  // Initially under all watermarks
  REQUIRE_FALSE(monitor.is_above_high_watermark());
  REQUIRE(monitor.is_below_low_watermark());

  // Generate enough load to exceed high watermark
  // Need to generate significant ops/bytes to raise the load
  for (int i = 0; i < 50000; ++i) {
    monitor.record_read_complete(10_KB);
    monitor.record_write_complete(10_KB);
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  monitor.update_metrics();

  auto metrics = monitor.metrics();
  // If load is high enough, should be above high watermark
  if (metrics.combined > config.load_high_watermark) {
    REQUIRE(monitor.is_above_high_watermark());
    REQUIRE_FALSE(monitor.is_below_low_watermark());
  }
}

TEST_CASE("LoadMonitor reset", "[load][optimization]") {
  OptimizationConfig config = default_config();
  LoadMonitor monitor(config);

  // Record some data
  monitor.record_read_complete(1_MB);
  monitor.record_write_complete(1_MB);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  monitor.update_metrics();

  auto metrics = monitor.metrics();
  REQUIRE(metrics.read_ops > 0);
  REQUIRE(metrics.write_ops > 0);

  // Reset
  monitor.reset();

  metrics = monitor.metrics();
  REQUIRE(metrics.read_ops == 0);
  REQUIRE(metrics.write_ops == 0);
  REQUIRE(metrics.cpu_usage == 0.0);
  REQUIRE(metrics.io_usage == 0.0);
}

TEST_CASE("LoadMonitor combined load calculation", "[load][optimization]") {
  OptimizationConfig config = default_config();
  LoadMonitor monitor(config);

  // Generate I/O heavy load (should weight I/O more)
  for (int i = 0; i < 1000; ++i) {
    monitor.record_read_complete(500_KB);
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  monitor.update_metrics();

  auto metrics = monitor.metrics();

  // Combined should be between 0 and 1
  REQUIRE(metrics.combined >= 0.0);
  REQUIRE(metrics.combined <= 1.0);

  // Combined should be influenced more by I/O (70% weight) than CPU (30%
  // weight) The exact calculation is: combined = 0.3 * cpu_usage + 0.7 *
  // io_usage
  double expected = 0.3 * metrics.cpu_usage + 0.7 * metrics.io_usage;
  REQUIRE(std::abs(metrics.combined - expected) < 0.001);
}

TEST_CASE("LoadMonitor concurrent recording",
          "[load][optimization][concurrent]") {
  OptimizationConfig config = default_config();
  LoadMonitor monitor(config);

  constexpr int num_threads = 4;
  constexpr int ops_per_thread = 10000;

  std::vector<std::thread> threads;

  threads.reserve(num_threads);
  for (int t = 0; t < num_threads; ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < ops_per_thread; ++i) {
        if (t % 2 == 0) {
          monitor.record_read_complete(100);
        } else {
          monitor.record_write_complete(100);
        }
      }
    });
  }

  for (auto &th : threads) {
    th.join();
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  monitor.update_metrics();

  auto metrics = monitor.metrics();

  // Should have recorded all operations
  uint64_t expected_reads = static_cast<uint64_t>(num_threads / 2) *
                            static_cast<uint64_t>(ops_per_thread);
  uint64_t expected_writes = static_cast<uint64_t>(num_threads / 2) *
                             static_cast<uint64_t>(ops_per_thread);

  REQUIRE(metrics.read_ops == expected_reads);
  REQUIRE(metrics.write_ops == expected_writes);
}

TEST_CASE("LoadMonitor rate calculation accuracy", "[load][optimization]") {
  OptimizationConfig config = default_config();
  LoadMonitor monitor(config);

  // First update to establish baseline
  monitor.update_metrics();

  // Wait known duration and record known operations
  auto start = std::chrono::steady_clock::now();

  constexpr int ops = 1000;
  constexpr int bytes_each = 1000;

  for (int i = 0; i < ops; ++i) {
    monitor.record_read_complete(bytes_each);
  }

  auto end = std::chrono::steady_clock::now();
  auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

  // Wait a bit more to ensure elapsed time > 0
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  monitor.update_metrics();
  auto metrics = monitor.metrics();

  // Ops per second should be reasonable
  // Note: This is approximate due to timing uncertainties
  REQUIRE(metrics.read_ops_per_sec > 0.0);
  REQUIRE(metrics.read_bytes_per_sec > 0.0);

  // Verify the ratio of bytes to ops is correct
  if (metrics.read_ops_per_sec > 0) {
    double ratio = metrics.read_bytes_per_sec / metrics.read_ops_per_sec;
    REQUIRE(std::abs(ratio - bytes_each) < bytes_each * 0.1);  // Within 10%
  }
}

TEST_CASE("LoadMonitor multiple update cycles", "[load][optimization]") {
  OptimizationConfig config = default_config();
  LoadMonitor monitor(config);

  // First cycle
  for (int i = 0; i < 100; ++i) {
    monitor.record_read_complete(1_KB);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  monitor.update_metrics();
  auto metrics1 = monitor.metrics();

  // Second cycle - more load
  for (int i = 0; i < 200; ++i) {
    monitor.record_read_complete(1_KB);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  monitor.update_metrics();
  auto metrics2 = monitor.metrics();

  // Third cycle - no new load
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  monitor.update_metrics();
  auto metrics3 = monitor.metrics();

  // Total ops should be cumulative
  REQUIRE(metrics2.read_ops > metrics1.read_ops);
  REQUIRE(metrics3.read_ops == metrics2.read_ops);

  // Third cycle should show lower rates since no new ops
  REQUIRE(metrics3.read_ops_per_sec < metrics2.read_ops_per_sec);
}
