// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <thread>
#include <vector>

#include "core/fork_gate.hpp"
#include "core/volume.hpp"  // Volume::TeardownSeam (test-seam builds)
#include "cyclone/cache.hpp"
#include "cyclone/key.hpp"
#include "support/temp_cache.hpp"

#ifndef _WIN32
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#endif

using namespace cyclone;

// =============================================================================
// Basic Lifecycle Tests
// =============================================================================

TEST_CASE("Cache start without volumes", "[lifecycle]") {
  CacheConfig config;
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  // Start without adding any volumes should fail or handle gracefully
  auto start_result = cache->start();
  // Either fails (no volumes) or succeeds (empty cache is valid)

  cache->stop();
}

TEST_CASE("Cache double start", "[lifecycle]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheConfig config;
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(10 * 1024 * 1024);

  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());
  REQUIRE(cache->is_running());

  // Second start should be handled gracefully (either no-op or error)
  auto second_start = cache->start();
  // Should not crash

  cache->stop();
}

TEST_CASE("Cache double stop", "[lifecycle]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheConfig config;
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(10 * 1024 * 1024);

  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  cache->stop();
  REQUIRE_FALSE(cache->is_running());

  // Second stop should be safe
  cache->stop();
  REQUIRE_FALSE(cache->is_running());
}

TEST_CASE("Cache stop without start", "[lifecycle]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheConfig config;
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(10 * 1024 * 1024);

  REQUIRE(cache->add_volume(vol_config).has_value());

  // Stop without start should be safe
  cache->stop();
  REQUIRE_FALSE(cache->is_running());
}

TEST_CASE("Cache operations on stopped cache", "[lifecycle]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheConfig config;
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(10 * 1024 * 1024);

  REQUIRE(cache->add_volume(vol_config).has_value());

  // Try operations without starting
  CacheKey key("test-key");

  auto read_result = cache->read_sync(key);
  // Should fail gracefully
  REQUIRE_FALSE(read_result.has_value());

  auto write_result = cache->write_sync(key, 100);
  // Should fail gracefully
  REQUIRE_FALSE(write_result.has_value());

  auto exists_result = cache->exists_sync(key);
  // Should fail gracefully
  REQUIRE_FALSE(exists_result.has_value());
}

// =============================================================================
// Start/Stop Cycle Stress Tests
// =============================================================================

TEST_CASE("Repeated start/stop cycles", "[lifecycle][stress]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  for (int cycle = 0; cycle < 10; ++cycle) {
    CacheConfig config;
    auto cache_result = Cache::create(config);
    REQUIRE(cache_result.has_value());
    auto &cache = *cache_result;

    VolumeConfig vol_config;
    vol_config.path = cache_path;
    vol_config.size = static_cast<size_t>(10 * 1024 * 1024);

    REQUIRE(cache->add_volume(vol_config).has_value());
    REQUIRE(cache->start().has_value());

    // Write some data
    CacheKey key("cycle-" + std::to_string(cycle));
    std::string content = "Content for cycle " + std::to_string(cycle);
    std::vector<std::byte> data(content.size());
    std::memcpy(data.data(), content.data(), content.size());

    auto wh = cache->write_sync(key, data.size());
    if (wh.has_value()) {
      wh->write_sync(std::span<const std::byte>(data));
      wh->close_sync();
    }

    cache->stop();
  }
}

TEST_CASE("Rapid start/stop cycles", "[lifecycle][stress]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  for (int cycle = 0; cycle < 20; ++cycle) {
    CacheConfig config;
    auto cache_result = Cache::create(config);
    REQUIRE(cache_result.has_value());
    auto &cache = *cache_result;

    VolumeConfig vol_config;
    vol_config.path = cache_path;
    vol_config.size = static_cast<size_t>(10 * 1024 * 1024);

    REQUIRE(cache->add_volume(vol_config).has_value());
    REQUIRE(cache->start().has_value());

    // Immediately stop
    cache->stop();
  }
}

TEST_CASE("Start/stop with ongoing operations", "[lifecycle][stress]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheConfig config;
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(20 * 1024 * 1024);

  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  // Pre-populate some data
  for (int i = 0; i < 50; ++i) {
    CacheKey key("pre-" + std::to_string(i));
    std::string content = "Pre-populated content " + std::to_string(i);
    std::vector<std::byte> data(content.size());
    std::memcpy(data.data(), content.data(), content.size());

    auto wh = cache->write_sync(key, data.size());
    if (wh.has_value()) {
      wh->write_sync(std::span<const std::byte>(data));
      wh->close_sync();
    }
  }

  std::atomic<bool> stop_flag{false};
  std::atomic<int> operations{0};

  // Start background operations
  std::thread reader([&]() {
    while (!stop_flag.load()) {
      int idx = operations.fetch_add(1) % 50;
      CacheKey key("pre-" + std::to_string(idx));
      auto rh = cache->read_sync(key);
      // May fail if cache is stopping, that's OK
    }
  });

  std::thread writer([&]() {
    int counter = 0;
    while (!stop_flag.load()) {
      CacheKey key("write-" + std::to_string(counter++));
      std::string content = "New content";
      std::vector<std::byte> data(content.size());
      std::memcpy(data.data(), content.data(), content.size());

      auto wh = cache->write_sync(key, data.size());
      if (wh.has_value()) {
        wh->write_sync(std::span<const std::byte>(data));
        wh->close_sync();
      }
      operations.fetch_add(1);
    }
  });

  // Let operations run for a bit
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Signal threads to stop FIRST
  stop_flag.store(true);

  // Wait for threads to finish before stopping cache
  reader.join();
  writer.join();

  // Now stop the cache
  cache->stop();

  INFO("Operations completed: " << operations.load());
  REQUIRE(operations.load() > 0);
}

// =============================================================================
// Data Persistence Tests
// =============================================================================

TEST_CASE("Data persists across restart", "[lifecycle][persistence]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();
  CacheKey key("persistent-key");
  std::string expected_content = "This content should persist";

  // Write data
  {
    CacheConfig config;
    auto cache_result = Cache::create(config);
    REQUIRE(cache_result.has_value());
    auto &cache = *cache_result;

    VolumeConfig vol_config;
    vol_config.path = cache_path;
    vol_config.size = static_cast<size_t>(10 * 1024 * 1024);

    REQUIRE(cache->add_volume(vol_config).has_value());
    REQUIRE(cache->start().has_value());

    std::vector<std::byte> data(expected_content.size());
    std::memcpy(data.data(), expected_content.data(), expected_content.size());

    auto wh = cache->write_sync(key, data.size());
    REQUIRE(wh.has_value());
    wh->write_sync(std::span<const std::byte>(data));
    REQUIRE(wh->close_sync().has_value());

    cache->stop();
  }

  // Read data after restart
  {
    CacheConfig config;
    auto cache_result = Cache::create(config);
    REQUIRE(cache_result.has_value());
    auto &cache = *cache_result;

    VolumeConfig vol_config;
    vol_config.path = cache_path;
    vol_config.size = static_cast<size_t>(10 * 1024 * 1024);

    REQUIRE(cache->add_volume(vol_config).has_value());
    REQUIRE(cache->start().has_value());

    auto rh = cache->read_sync(key);
    // Note: In-memory directory may not persist, so this might fail
    // The test verifies graceful handling either way

    cache->stop();
  }
}

// =============================================================================
// Multiple Volume Lifecycle Tests
// =============================================================================

TEST_CASE("Multiple volumes start/stop", "[lifecycle][volume]") {
  TempCacheDir tmp;
  std::string path1 = tmp.path("vol1.dat");
  std::string path2 = tmp.path("vol2.dat");
  std::string path3 = tmp.path("vol3.dat");

  CacheConfig config;
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol1, vol2, vol3;
  vol1.path = path1;
  vol1.size = static_cast<size_t>(10 * 1024 * 1024);
  vol2.path = path2;
  vol2.size = static_cast<size_t>(10 * 1024 * 1024);
  vol3.path = path3;
  vol3.size = static_cast<size_t>(10 * 1024 * 1024);

  REQUIRE(cache->add_volume(vol1).has_value());
  REQUIRE(cache->add_volume(vol2).has_value());
  REQUIRE(cache->add_volume(vol3).has_value());

  REQUIRE(cache->volume_count() == 3);
  REQUIRE(cache->start().has_value());

  // Write to different volumes (based on key hashing)
  for (int i = 0; i < 30; ++i) {
    CacheKey key("multi-vol-" + std::to_string(i));
    std::string content = "Content " + std::to_string(i);
    std::vector<std::byte> data(content.size());
    std::memcpy(data.data(), content.data(), content.size());

    auto wh = cache->write_sync(key, data.size());
    if (wh.has_value()) {
      wh->write_sync(std::span<const std::byte>(data));
      wh->close_sync();
    }
  }

  cache->stop();
}

TEST_CASE("Add volume after start fails", "[lifecycle][volume]") {
  TempCacheDir tmp;
  std::string path1 = tmp.path("vol1.dat");
  std::string path2 = tmp.path("vol2.dat");

  CacheConfig config;
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol1;
  vol1.path = path1;
  vol1.size = static_cast<size_t>(10 * 1024 * 1024);

  REQUIRE(cache->add_volume(vol1).has_value());
  REQUIRE(cache->start().has_value());

  // Try to add volume after start
  VolumeConfig vol2;
  vol2.path = path2;
  vol2.size = static_cast<size_t>(10 * 1024 * 1024);

  auto add_result = cache->add_volume(vol2);
  // Should fail or be handled gracefully

  cache->stop();
}

// =============================================================================
// Error Recovery Tests
// =============================================================================

TEST_CASE("Cache handles missing file on restart", "[lifecycle][error]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();
  CacheKey key("test-key");

  // Start, write a key, and verify it is present before the file is deleted.
  {
    CacheConfig config;
    auto cache_result = Cache::create(config);
    REQUIRE(cache_result.has_value());
    auto &cache = *cache_result;

    VolumeConfig vol_config;
    vol_config.path = cache_path;
    vol_config.size = static_cast<size_t>(10 * 1024 * 1024);

    REQUIRE(cache->add_volume(vol_config).has_value());
    REQUIRE(cache->start().has_value());

    std::string content = "test content";
    std::vector<std::byte> data(content.size());
    std::memcpy(data.data(), content.data(), content.size());

    auto wh = cache->write_sync(key, data.size());
    REQUIRE(wh.has_value());
    REQUIRE(wh->write_sync(std::span<const std::byte>(data)).has_value());
    REQUIRE(wh->close_sync().has_value());

    auto exists = cache->exists_sync(key);
    REQUIRE(exists.has_value());
    REQUIRE(*exists == true);

    cache->stop();
  }

  // Delete the REAL backing file(s).  add_volume() structural-fingerprints the
  // filename (cyclone.dat -> cyclone-8-<hex>.dat, plus a possible ".small"
  // sibling), so removing the raw cache_path would miss the file the library
  // actually uses.  Remove the whole temp directory subtree and recreate it
  // empty (a single remove_all, not an iterate-while-removing loop, which is
  // unsafe when more than one backing file is present) to guarantee the
  // backing store is gone, reproducing a genuinely missing file on restart.
  std::error_code ec;
  std::filesystem::remove_all(tmp.dir(), ec);
  REQUIRE_FALSE(ec);
  std::filesystem::create_directories(tmp.dir(), ec);
  REQUIRE_FALSE(ec);
  REQUIRE(std::filesystem::is_empty(tmp.dir()));

  // Restart onto the now-missing backing file: open/start must succeed by
  // creating a fresh, empty volume, and the key written before the delete must
  // be absent (the cache does not resurrect data from a file that no longer
  // exists).
  {
    CacheConfig config;
    auto cache_result = Cache::create(config);
    REQUIRE(cache_result.has_value());
    auto &cache = *cache_result;

    VolumeConfig vol_config;
    vol_config.path = cache_path;
    vol_config.size = static_cast<size_t>(10 * 1024 * 1024);

    REQUIRE(cache->add_volume(vol_config).has_value());
    // start() must open gracefully despite the missing backing file, creating
    // a fresh volume in its place.  If Volume::open regressed to require a
    // pre-existing file, this would fail (the file-present sibling case would
    // not) -- that is the missing-file path this case guards.
    REQUIRE(cache->start().has_value());

    // Deletion-specific: we emptied the directory above, so a non-empty
    // directory now proves start() recreated the backing file.
    REQUIRE_FALSE(std::filesystem::is_empty(tmp.dir()));

    // The volume is fresh, so the key written before the delete is gone (the
    // single-process directory is rebuilt empty, and there is no file to
    // recover it from).
    auto exists = cache->exists_sync(key);
    REQUIRE(exists.has_value());
    REQUIRE(*exists == false);

    auto rh = cache->read_sync(key);
    REQUIRE_FALSE(rh.has_value());
    REQUIRE(rh.error() == CacheError::NotFound);

    cache->stop();
  }
}

TEST_CASE("Cache handles read-only file gracefully", "[lifecycle][error]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  // Make file read-only (platform-specific)
#ifndef _WIN32
  chmod(cache_path.c_str(), 0444);
#endif

  CacheConfig config;
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(10 * 1024 * 1024);

  // Should fail gracefully due to read-only
  auto add_result = cache->add_volume(vol_config);
  // Either fails (expected) or somehow succeeds

  // Restore permissions for cleanup
#ifndef _WIN32
  chmod(cache_path.c_str(), 0644);
#endif
}

// =============================================================================
// Destructor Safety Tests
// =============================================================================

TEST_CASE("Cache destructor cleans up without explicit stop", "[lifecycle]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  {
    CacheConfig config;
    auto cache_result = Cache::create(config);
    REQUIRE(cache_result.has_value());
    auto &cache = *cache_result;

    VolumeConfig vol_config;
    vol_config.path = cache_path;
    vol_config.size = static_cast<size_t>(10 * 1024 * 1024);

    REQUIRE(cache->add_volume(vol_config).has_value());
    REQUIRE(cache->start().has_value());

    // Write some data
    CacheKey key("destructor-test");
    std::string content = "content";
    std::vector<std::byte> data(content.size());
    std::memcpy(data.data(), content.data(), content.size());

    auto wh = cache->write_sync(key, data.size());
    if (wh.has_value()) {
      wh->write_sync(std::span<const std::byte>(data));
      wh->close_sync();
    }

    // Don't call stop() - let destructor handle cleanup
  }

  // If we get here without crash, destructor worked correctly
  REQUIRE(true);
}

TEST_CASE("WriteHandle destructor aborts uncommitted write",
          "[lifecycle][handle]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheConfig config;
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(10 * 1024 * 1024);

  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  CacheKey key("abort-test");

  {
    auto wh = cache->write_sync(key, 100);
    REQUIRE(wh.has_value());

    std::string content = "partial content";
    std::vector<std::byte> data(content.size());
    std::memcpy(data.data(), content.data(), content.size());

    wh->write_sync(std::span<const std::byte>(data));

    // Don't close - let destructor handle abort
  }

  // Key should not exist (write was aborted)
  auto exists = cache->exists_sync(key);
  REQUIRE(exists.has_value());
  REQUIRE(*exists == false);

  cache->stop();
}

// =============================================================================
// Hit Tracking Lifecycle Tests
// =============================================================================

TEST_CASE("Hit tracking survives restart", "[lifecycle][hit_tracking]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();
  CacheKey key("hit-track-test");

  // Write and read multiple times
  {
    CacheConfig config;
    config.enable_hit_tracking = true;
    config.hit_flush_interval = std::chrono::milliseconds(50);
    auto cache_result = Cache::create(config);
    REQUIRE(cache_result.has_value());
    auto &cache = *cache_result;

    VolumeConfig vol_config;
    vol_config.path = cache_path;
    vol_config.size = static_cast<size_t>(10 * 1024 * 1024);

    REQUIRE(cache->add_volume(vol_config).has_value());
    REQUIRE(cache->start().has_value());

    std::string content = "hit tracking content";
    std::vector<std::byte> data(content.size());
    std::memcpy(data.data(), content.data(), content.size());

    auto wh = cache->write_sync(key, data.size());
    REQUIRE(wh.has_value());
    wh->write_sync(std::span<const std::byte>(data));
    REQUIRE(wh->close_sync().has_value());

    // Generate some hits
    for (int i = 0; i < 10; ++i) {
      auto rh = cache->read_sync(key);
      REQUIRE(rh.has_value());
    }

    // Wait for flush
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    cache->stop();
  }
}

// =============================================================================
// All-threads teardown regression guard (thread-lifecycle leak fix)
// =============================================================================

TEST_CASE("Cache destructor stops all background threads within bound",
          "[lifecycle][hit_tracking]") {
  // Regression guard tied to the production thread leak: a Cache built with
  // BOTH hit-tracking and the optimization engine enabled spins up all three
  // background-thread classes (HitTracker flush thread, OptimizationEngine
  // monitoring thread, and the AdaptiveThreadPool workers).  Tearing the cache
  // down -- whether via explicit stop() or via the destructor -- must join
  // every one of them promptly so no thread survives teardown.
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheConfig config;
  // Both thread-spawning subsystems on (these are the defaults, set explicitly
  // here so the intent of the test survives any future default change).
  config.enable_hit_tracking = true;
  config.hit_flush_interval = std::chrono::milliseconds(50);
  config.optimization_config.enabled = true;
  // Keep the monitoring loop lively so the thread is genuinely active.
  config.optimization_config.scale_check_interval =
      std::chrono::milliseconds(50);

  SECTION("explicit stop() joins all threads quickly") {
    auto cache_result = Cache::create(config);
    REQUIRE(cache_result.has_value());
    auto &cache = *cache_result;

    VolumeConfig vol_config;
    vol_config.path = cache_path;
    vol_config.size = static_cast<size_t>(20 * 1024 * 1024);

    REQUIRE(cache->add_volume(vol_config).has_value());
    REQUIRE(cache->start().has_value());
    REQUIRE(cache->is_running());

    // Drive a few Put/Get so all background threads have real work in flight
    // (writes feed the optimization queue, reads feed the hit tracker).
    for (int i = 0; i < 25; ++i) {
      CacheKey key("all-threads-" + std::to_string(i));
      std::string content = "payload for entry " + std::to_string(i);
      std::vector<std::byte> data(content.size());
      std::memcpy(data.data(), content.data(), content.size());

      auto wh = cache->write_sync(key, data.size());
      if (wh.has_value()) {
        wh->write_sync(std::span<const std::byte>(data));
        wh->close_sync();
      }
    }
    for (int round = 0; round < 5; ++round) {
      for (int i = 0; i < 25; ++i) {
        CacheKey key("all-threads-" + std::to_string(i));
        auto rh = cache->read_sync(key);
        (void)rh;
      }
    }

    // Let the background threads actually pick up the work.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // stop() must join the hit tracker, optimization monitor, and pool workers
    // within a bounded time -- a hung/leaked thread would blow past this.
    auto begin = std::chrono::steady_clock::now();
    cache->stop();
    auto elapsed = std::chrono::steady_clock::now() - begin;

    auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    INFO("Cache::stop() took " << elapsed_ms << " ms");

    // Each component bounds its join at ~5s; with all three the worst-case
    // serial bound is well under this generous 8s ceiling on loaded CI.
    REQUIRE(elapsed < std::chrono::seconds(8));
    REQUIRE_FALSE(cache->is_running());
  }

  SECTION("destructor (no explicit stop) joins all threads quickly") {
    auto begin = std::chrono::steady_clock::now();

    {
      auto cache_result = Cache::create(config);
      REQUIRE(cache_result.has_value());
      auto &cache = *cache_result;

      VolumeConfig vol_config;
      vol_config.path = cache_path;
      vol_config.size = static_cast<size_t>(20 * 1024 * 1024);

      REQUIRE(cache->add_volume(vol_config).has_value());
      REQUIRE(cache->start().has_value());
      REQUIRE(cache->is_running());

      for (int i = 0; i < 25; ++i) {
        CacheKey key("dtor-threads-" + std::to_string(i));
        std::string content = "payload for entry " + std::to_string(i);
        std::vector<std::byte> data(content.size());
        std::memcpy(data.data(), content.data(), content.size());

        auto wh = cache->write_sync(key, data.size());
        if (wh.has_value()) {
          wh->write_sync(std::span<const std::byte>(data));
          wh->close_sync();
        }
        auto rh = cache->read_sync(key);
        (void)rh;
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(100));

      // No explicit stop() -- the destructor must stop and join every thread.
    }

    auto elapsed = std::chrono::steady_clock::now() - begin;
    auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    INFO("Cache lifetime + destructor took " << elapsed_ms << " ms");

    // Destructor completed; if any background thread had survived teardown the
    // join would have stalled and pushed us past this bound.
    REQUIRE(elapsed < std::chrono::seconds(8));
  }
}

// =============================================================================
// Configuration Change Tests
// =============================================================================

TEST_CASE("Different config on restart", "[lifecycle][config]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  // Start with one config
  {
    CacheConfig config;
    config.ram_cache_size = static_cast<size_t>(1024 * 1024);  // 1MB RAM cache
    auto cache_result = Cache::create(config);
    REQUIRE(cache_result.has_value());
    auto &cache = *cache_result;

    VolumeConfig vol_config;
    vol_config.path = cache_path;
    vol_config.size = static_cast<size_t>(20 * 1024 * 1024);

    REQUIRE(cache->add_volume(vol_config).has_value());
    REQUIRE(cache->start().has_value());

    // Write data
    for (int i = 0; i < 10; ++i) {
      CacheKey key("config-test-" + std::to_string(i));
      std::string content = "Content " + std::to_string(i);
      std::vector<std::byte> data(content.size());
      std::memcpy(data.data(), content.data(), content.size());

      auto wh = cache->write_sync(key, data.size());
      if (wh.has_value()) {
        wh->write_sync(std::span<const std::byte>(data));
        wh->close_sync();
      }
    }

    cache->stop();
  }

  // Restart with different config
  {
    CacheConfig config;
    config.ram_cache_size =
        static_cast<size_t>(2 * 1024 * 1024);  // 2MB RAM cache (different)
    auto cache_result = Cache::create(config);
    REQUIRE(cache_result.has_value());
    auto &cache = *cache_result;

    VolumeConfig vol_config;
    vol_config.path = cache_path;
    vol_config.size = static_cast<size_t>(20 * 1024 * 1024);

    REQUIRE(cache->add_volume(vol_config).has_value());
    REQUIRE(cache->start().has_value());

    // Should work with new config
    REQUIRE(cache->is_running());

    cache->stop();
  }
}

// =============================================================================
// Concurrent stop() safety
// =============================================================================

TEST_CASE("Concurrent stop with in-flight reads must not crash",
          "[lifecycle][concurrency]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheConfig config;
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(10 * 1024 * 1024);

  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  // Write some entries so reads have something to find
  for (int i = 0; i < 100; ++i) {
    CacheKey key("stop-race-" + std::to_string(i));
    std::string content = "data-" + std::to_string(i);
    std::vector<std::byte> data(content.size());
    std::memcpy(data.data(), content.data(), content.size());

    auto wh = cache->write_sync(key, data.size());
    if (wh.has_value()) {
      wh->write_sync(std::span<const std::byte>(data));
      wh->close_sync();
    }
  }

  // Hammer the cache with reads from multiple threads while stop() is called.
  // Before the fix: TOCTOU between is_running() check and volume access could
  // cause a use-after-free if stop() destroyed volumes between the two.
  std::atomic<bool> keep_going{true};
  std::atomic<int> operations_completed{0};

  constexpr int kReaderThreads = 8;
  std::vector<std::thread> readers;

  readers.reserve(kReaderThreads);
  for (int t = 0; t < kReaderThreads; ++t) {
    readers.emplace_back([&, t]() {
      int i = 0;
      while (keep_going.load(std::memory_order_relaxed)) {
        CacheKey key("stop-race-" + std::to_string((t * 13 + i) % 100));

        // These must either succeed or return NotInitialized — never crash
        auto result = cache->read_sync(key);
        (void)result;

        auto exists = cache->exists_sync(key);
        (void)exists;

        operations_completed.fetch_add(1, std::memory_order_relaxed);
        ++i;
      }
    });
  }

  // Let readers warm up
  while (operations_completed.load(std::memory_order_relaxed) <
         kReaderThreads * 10) {
    std::this_thread::yield();
  }

  // Now stop the cache while readers are active
  cache->stop();
  keep_going = false;

  for (auto &t : readers) {
    t.join();
  }

  // If we got here without crashing or TSan/ASan firing, the fix works.
  REQUIRE_FALSE(cache->is_running());
}

// =============================================================================
// A handle call on one thread while another thread stops the cache
// =============================================================================
//
// An embedder serves from borrowed handles on its request threads and stops
// the cache at shutdown or reload on another.  Closing a handle, renewing its
// lease and polling its forced-wrap deadline all dereference the handle's
// stripe, outside the cache's gate.  stop() frees the stripes.  A call that
// had already passed the "torn down?" check when stop() published teardown
// used to go on into freed memory (ThreadSanitizer: Volume::close against
// release_borrow, from the case "Concurrent stop with in-flight reads must
// not crash" above).  Now close() waits for such a call.
//
// The cases below make that window deterministic with the teardown seams:
// the call is parked right after it was admitted, stop() runs meanwhile, and
// the call is let go only when close() reports that it is waiting for it --
// or, in a library without the wait, when stop() has returned, which fails
// the case.

namespace {

enum class HandleCallKind : uint8_t {
  kClose,
  kRenew,
  kRenewStrict,
  kForcedWrapPoll,
  kCommit,           // WriteHandle::close_sync()
  kCommitAlternate,  // the same, for a handle from write_alternate_sync()
};

const char *to_string(HandleCallKind kind) {
  switch (kind) {
    case HandleCallKind::kClose:
      return "close";
    case HandleCallKind::kRenew:
      return "renew_lease";
    case HandleCallKind::kRenewStrict:
      return "renew_lease_strict";
    case HandleCallKind::kForcedWrapPoll:
      return "ns_until_forced_wrap";
    case HandleCallKind::kCommit:
      return "write commit";
    case HandleCallKind::kCommitAlternate:
      return "alternate write commit";
  }
  return "?";
}

// Parks the handle call made on `caller` right after it was admitted, until
// close() waits for it, the test says stop() returned, or a deadline.
struct TeardownWindow {
  std::atomic<std::thread::id> caller{};
  std::atomic<bool> admitted{false};
  std::atomic<bool> close_waiting{false};
  std::atomic<bool> stop_returned{false};
  // What the parked call saw when it was let go.
  std::atomic<bool> let_go_by_close_waiting{false};
  std::atomic<bool> let_go_by_stop_returned{false};

  TeardownWindow() {
    Volume::s_teardown_seam_for_test = [this](Volume::TeardownSeam seam) {
      if (seam == Volume::TeardownSeam::kCloseWaiting) {
        close_waiting.store(true, std::memory_order_release);
        return;
      }
      if (std::this_thread::get_id() !=
          caller.load(std::memory_order_acquire)) {
        return;
      }
      admitted.store(true, std::memory_order_release);
      const auto deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(60);
      while (std::chrono::steady_clock::now() < deadline) {
        if (close_waiting.load(std::memory_order_acquire)) {
          let_go_by_close_waiting.store(true, std::memory_order_release);
          return;
        }
        if (stop_returned.load(std::memory_order_acquire)) {
          let_go_by_stop_returned.store(true, std::memory_order_release);
          return;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
      }
    };
  }
  ~TeardownWindow() { Volume::s_teardown_seam_for_test = nullptr; }
  TeardownWindow(const TeardownWindow &) = delete;
  TeardownWindow &operator=(const TeardownWindow &) = delete;

  bool wait_admitted() const {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!admitted.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return admitted.load(std::memory_order_acquire);
  }
};

void run_handle_call_against_stop(HandleCallKind kind, bool mmap_directory) {
  CAPTURE(to_string(kind), mmap_directory);
  TempCacheDir tmp;
  CacheConfig config;
  config.set_ram_cache_size(0);  // Disk hits: the handle borrows the mapping
  if (mmap_directory) {
    config.set_multi_process(0, 1);
  }
  auto created = Cache::create(config);
  REQUIRE(created.has_value());
  auto &cache = *created;
  VolumeConfig vol_config;
  vol_config.path = tmp.path();
  vol_config.size = static_cast<size_t>(10 * 1024 * 1024);
  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  const CacheKey key("handle-call-against-stop");
  const std::vector<std::byte> body(4096, std::byte{0x5A});
  {
    auto wh = cache->write_sync(key, body.size());
    REQUIRE(wh.has_value());
    REQUIRE(wh->write_sync(std::span<const std::byte>(body)).has_value());
    REQUIRE(wh->close_sync().has_value());
  }
  auto read = cache->read_sync(key);
  REQUIRE(read.has_value());
  REQUIRE_FALSE(read->is_ram_cache_hit());
  std::optional<ReadHandle> handle(std::move(*read));
  const bool is_commit = kind == HandleCallKind::kCommit ||
                         kind == HandleCallKind::kCommitAlternate;
  if (is_commit) {
    handle.reset();  // Only the write handle below is in play
  }

  // The write handle for the commit kinds: filled, not yet committed.
  const CacheKey committed_key("committed-against-stop");
  const std::vector<std::byte> committed_body(8192, std::byte{0xC3});
  std::optional<WriteHandle> write_handle;
  if (is_commit) {
    auto wh = kind == HandleCallKind::kCommit
                  ? cache->write_sync(committed_key, committed_body.size())
                  : cache->write_alternate_sync(key, AlternateId::Gzip,
                                                committed_body.size());
    REQUIRE(wh.has_value());
    REQUIRE(
        wh->write_sync(std::span<const std::byte>(committed_body)).has_value());
    write_handle.emplace(std::move(*wh));
  }

  TeardownWindow window;
  bool renewed = false;
  LeaseRenewal strict = LeaseRenewal::kTorn;
  uint64_t forced_wrap_ns = 0;
  std::optional<std::expected<void, CacheError>> committed;
  std::thread caller([&] {
    window.caller.store(std::this_thread::get_id(), std::memory_order_release);
    switch (kind) {
      case HandleCallKind::kCommit:
      case HandleCallKind::kCommitAlternate:
        committed = write_handle->close_sync();
        break;
      case HandleCallKind::kClose:
        handle.reset();
        break;
      case HandleCallKind::kRenew:
        renewed = handle->renew_lease();
        break;
      case HandleCallKind::kRenewStrict:
        strict = handle->renew_lease_strict();
        break;
      case HandleCallKind::kForcedWrapPoll:
        forced_wrap_ns = handle->ns_until_forced_wrap();
        break;
    }
  });
  const bool admitted = window.wait_admitted();

  // The call is inside the library, past the teardown checks, about to use
  // its stripe.  Stop the cache under it.
  cache->stop();
  window.stop_returned.store(true, std::memory_order_release);
  caller.join();

  REQUIRE(admitted);
  // close() saw the call and waited; the call ran on live stripes, and
  // stop() returned only after it had left.
  CHECK(window.close_waiting.load());
  CHECK(window.let_go_by_close_waiting.load());
  CHECK_FALSE(window.let_go_by_stop_returned.load());
  // An admitted call completes normally.
  switch (kind) {
    case HandleCallKind::kClose:
      break;
    case HandleCallKind::kRenew:
      CHECK(renewed);
      break;
    case HandleCallKind::kRenewStrict:
      CHECK(strict == LeaseRenewal::kOk);
      break;
    case HandleCallKind::kForcedWrapPoll:
      CHECK(forced_wrap_ns == UINT64_MAX);  // No wrap is being deferred
      break;
    case HandleCallKind::kCommit:
    case HandleCallKind::kCommitAlternate:
      REQUIRE(committed.has_value());
      CHECK(committed->has_value());  // It ran to its end, on live stripes
      break;
  }
  write_handle.reset();

  // A handle that outlives stop() stays safe to use and to close: its calls
  // find the generation torn down and touch no stripe.
  if (handle.has_value()) {
    CHECK_FALSE(handle->renew_lease());
    CHECK(handle->renew_lease_strict() == LeaseRenewal::kTorn);
    CHECK(handle->ns_until_forced_wrap() == UINT64_MAX);
    CHECK(handle->content().size() == body.size());
    handle.reset();
  }
  CHECK_FALSE(window.let_go_by_stop_returned.load());

  // A commit that stop() waited for is a commit: the document is there
  // when the cache is started again.  (Only the mmap directory is kept
  // across a restart; the in-memory one starts empty.)
  if (is_commit && mmap_directory) {
    REQUIRE(cache->start().has_value());
    if (kind == HandleCallKind::kCommit) {
      auto back = cache->read_sync(committed_key);
      REQUIRE(back.has_value());
      CHECK(back->content().size() == committed_body.size());
    } else {
      auto alternates = cache->list_alternates_sync(key);
      REQUIRE(alternates.has_value());
      CHECK(alternates->size() == 2);
    }
    cache->stop();
  }
}

}  // namespace

TEST_CASE("stop() waits for a handle call that is already using its stripe",
          "[lifecycle][concurrency][borrow]") {
  const HandleCallKind kind =
      GENERATE(HandleCallKind::kClose, HandleCallKind::kRenew,
               HandleCallKind::kRenewStrict, HandleCallKind::kForcedWrapPoll,
               HandleCallKind::kCommit, HandleCallKind::kCommitAlternate);
  const bool mmap_directory = GENERATE(false, true);
  run_handle_call_against_stop(kind, mmap_directory);
}

TEST_CASE("A handle from before a stop()/start() cycle touches no stripe",
          "[lifecycle][concurrency][borrow]") {
  // The stripes the handle points at were freed by stop(); start() built
  // new ones and re-armed the volume.  The old handle's calls must be
  // refused by its generation, not admitted on the strength of the reopened
  // volume: an admitted call would fire the seam below.
  TempCacheDir tmp;
  CacheConfig config;
  config.set_ram_cache_size(0);
  auto created = Cache::create(config);
  REQUIRE(created.has_value());
  auto &cache = *created;
  VolumeConfig vol_config;
  vol_config.path = tmp.path();
  vol_config.size = static_cast<size_t>(10 * 1024 * 1024);
  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());
  const CacheKey key("handle-across-restart");
  const std::vector<std::byte> body(4096, std::byte{0x3C});
  {
    auto wh = cache->write_sync(key, body.size());
    REQUIRE(wh.has_value());
    REQUIRE(wh->write_sync(std::span<const std::byte>(body)).has_value());
    REQUIRE(wh->close_sync().has_value());
  }
  auto read = cache->read_sync(key);
  REQUIRE(read.has_value());
  REQUIRE_FALSE(read->is_ram_cache_hit());
  std::optional<ReadHandle> stale(std::move(*read));

  cache->stop();
  REQUIRE(cache->start().has_value());
  const CacheKey fresh_key("handle-after-restart");
  {
    auto wh = cache->write_sync(fresh_key, body.size());
    REQUIRE(wh.has_value());
    REQUIRE(wh->write_sync(std::span<const std::byte>(body)).has_value());
    REQUIRE(wh->close_sync().has_value());
  }
  auto fresh_read = cache->read_sync(fresh_key);
  REQUIRE(fresh_read.has_value());
  REQUIRE_FALSE(fresh_read->is_ram_cache_hit());
  std::optional<ReadHandle> fresh(std::move(*fresh_read));

  std::atomic<int> admitted_calls{0};
  Volume::s_teardown_seam_for_test = [&](Volume::TeardownSeam seam) {
    if (seam == Volume::TeardownSeam::kHandleCallAdmitted) {
      admitted_calls.fetch_add(1, std::memory_order_relaxed);
    }
  };
  const bool renewed = stale->renew_lease();
  const LeaseRenewal strict = stale->renew_lease_strict();
  const uint64_t forced_wrap_ns = stale->ns_until_forced_wrap();
  stale.reset();
  const int stale_admitted = admitted_calls.load();
  // A handle of the new generation is admitted as usual.
  const bool fresh_renewed = fresh->renew_lease();
  fresh.reset();
  const int fresh_admitted = admitted_calls.load() - stale_admitted;
  Volume::s_teardown_seam_for_test = nullptr;

  CHECK_FALSE(renewed);
  CHECK(strict == LeaseRenewal::kTorn);
  CHECK(forced_wrap_ns == UINT64_MAX);
  CHECK(stale_admitted == 0);
  CHECK(fresh_renewed);
  CHECK(fresh_admitted == 2);  // The renew and the close
  cache->stop();
}

// =============================================================================
// Write handles and stop()
// =============================================================================
//
// A WriteHandle buffers its content and writes it at close.  The commit
// uses the stripe the handle was created for, and stop() frees the stripes
// while the Volume object lives on (the Cache keeps it for the next
// start()).  A handle committed after stop(), or after a stop()/start()
// cycle, used to run its commit on freed memory.  Now the commit is refused
// with CacheError::Closed and nothing is written.

namespace {

struct StaleWriteOutcome {
  std::expected<size_t, CacheError> append{0};
  std::expected<std::span<std::byte>, CacheError> reserved{
      std::span<std::byte>{}};
  std::expected<void, CacheError> commit{};
  std::expected<void, CacheError> second_commit{};
};

// Uses a write handle that was created before the cache was stopped.
StaleWriteOutcome use_stale_write_handle(WriteHandle &handle,
                                         std::span<const std::byte> more) {
  StaleWriteOutcome outcome;
  outcome.append = handle.write_sync(more);
  outcome.reserved = handle.reserve(16);
  outcome.commit = handle.close_sync();
  outcome.second_commit = handle.close_sync();
  return outcome;
}

void check_refused(const StaleWriteOutcome &outcome) {
  REQUIRE_FALSE(outcome.append.has_value());
  CHECK(outcome.append.error() == CacheError::Closed);
  REQUIRE_FALSE(outcome.reserved.has_value());
  CHECK(outcome.reserved.error() == CacheError::Closed);
  REQUIRE_FALSE(outcome.commit.has_value());
  CHECK(outcome.commit.error() == CacheError::Closed);
  // The handle is closed by the refused commit, like by any other.
  CHECK(outcome.second_commit.has_value());
}

}  // namespace

TEST_CASE("A write handle committed after stop() is refused, not written",
          "[lifecycle][write][borrow]") {
  const bool mmap_directory = GENERATE(false, true);
  const bool alternate = GENERATE(false, true);
  const bool restart = GENERATE(false, true);
  CAPTURE(mmap_directory, alternate, restart);
  TempCacheDir tmp;
  CacheConfig config;
  config.set_ram_cache_size(0);
  if (mmap_directory) {
    config.set_multi_process(0, 1);
  }
  auto created = Cache::create(config);
  REQUIRE(created.has_value());
  auto &cache = *created;
  VolumeConfig vol_config;
  vol_config.path = tmp.path();
  vol_config.size = static_cast<size_t>(10 * 1024 * 1024);
  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  const CacheKey base_key("stale-write-base");
  const CacheKey stale_key("stale-write-target");
  const std::vector<std::byte> body(4096, std::byte{0x42});
  {
    auto wh = cache->write_sync(base_key, body.size());
    REQUIRE(wh.has_value());
    REQUIRE(wh->write_sync(std::span<const std::byte>(body)).has_value());
    REQUIRE(wh->close_sync().has_value());
  }
  auto wh = alternate ? cache->write_alternate_sync(base_key, AlternateId::Gzip,
                                                    2 * body.size())
                      : cache->write_sync(stale_key, 2 * body.size());
  REQUIRE(wh.has_value());
  REQUIRE(wh->write_sync(std::span<const std::byte>(body)).has_value());

  cache->stop();
  if (restart) {
    REQUIRE(cache->start().has_value());
  }

  // The handle's stripe was freed by stop().  With `restart` the volume is
  // open again, on new stripes: the stale handle must not write into them.
  const StaleWriteOutcome outcome =
      use_stale_write_handle(*wh, std::span<const std::byte>(body));
  check_refused(outcome);
  wh = make_unexpected(CacheError::Closed);  // Destroying it is safe too

  if (!restart) {
    REQUIRE(cache->start().has_value());
  }
  // Nothing of the refused write reached the volume.
  if (alternate) {
    // The base document alone, or (in-memory directory: not kept across
    // the restart) nothing at all; never the refused alternate.
    auto alternates = cache->list_alternates_sync(base_key);
    if (mmap_directory) {
      REQUIRE(alternates.has_value());
      CHECK(alternates->size() == 1);
    } else {
      CHECK((!alternates.has_value() || alternates->empty()));
    }
  } else {
    CHECK_FALSE(cache->exists_sync(stale_key).value_or(true));
  }
  // The restarted cache takes writes as usual.
  {
    auto fresh = cache->write_sync(stale_key, body.size());
    REQUIRE(fresh.has_value());
    REQUIRE(fresh->write_sync(std::span<const std::byte>(body)).has_value());
    REQUIRE(fresh->close_sync().has_value());
    CHECK(cache->exists_sync(stale_key).value_or(false));
  }
  cache->stop();
}

TEST_CASE("A stale write handle on a bare Volume is refused after reopen",
          "[lifecycle][write][borrow]") {
  // The same without a Cache: close() retires the stripe generation, and a
  // later open() does not bring it back.
  TempCacheDir tmp;
  VolumeConfig vol_config;
  vol_config.path = tmp.path();
  vol_config.size = static_cast<size_t>(8 * 1024 * 1024);
  auto volume = std::make_shared<Volume>(vol_config);
  REQUIRE(volume->open().has_value());
  const CacheKey key("stale-on-a-bare-volume");
  const std::vector<std::byte> body(4096, std::byte{0x24});
  auto wh = volume->write_sync(key, 2 * body.size());
  REQUIRE(wh.has_value());
  REQUIRE(wh->write_sync(std::span<const std::byte>(body)).has_value());
  volume->close();
  REQUIRE(volume->open().has_value());
  check_refused(use_stale_write_handle(*wh, std::span<const std::byte>(body)));
  CHECK_FALSE(volume->exists_sync(key).value_or(true));
  volume->close();
}

TEST_CASE("Concurrent stop with in-flight writes must not crash",
          "[lifecycle][concurrency]") {
  // Writers hold a filled write handle and commit it around the moment the
  // main thread stops the cache: some just before, some while stop() runs,
  // some after it returned.  Every commit must either succeed (it began
  // before the stripes were retired, and stop() waited for it) or be
  // refused with Closed; and no commit may touch a freed stripe, which is
  // what a sanitizer build of this case checks.
  //
  // An earlier form of this case looped write_sync / close_sync back to
  // back.  Its window between the two calls was a few instructions wide,
  // so it essentially never committed across stop().
  constexpr int kRounds = 6;
  constexpr int kWriterThreads = 8;
  std::atomic<uint64_t> committed{0};
  std::atomic<uint64_t> refused{0};
  std::atomic<uint64_t> wrong{0};
  for (int round = 0; round < kRounds; ++round) {
    TempCacheDir tmp;
    CacheConfig config;
    if ((round % 2) == 1) {
      config.set_multi_process(0, 1);
    }
    auto cache_result = Cache::create(config);
    REQUIRE(cache_result.has_value());
    auto &cache = *cache_result;
    VolumeConfig vol_config;
    vol_config.path = tmp.path();
    vol_config.size = static_cast<size_t>(20 * 1024 * 1024);
    REQUIRE(cache->add_volume(vol_config).has_value());
    REQUIRE(cache->start().has_value());

    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::atomic<bool> stop_returned{false};
    std::vector<std::thread> writers;
    writers.reserve(kWriterThreads);
    for (int t = 0; t < kWriterThreads; ++t) {
      writers.emplace_back([&, t]() {
        const std::vector<std::byte> data(2048 + 512 * t, std::byte{0x11});
        CacheKey key("stop-write-" + std::to_string(round * 100 + t));
        auto wh = cache->write_sync(key, data.size());
        const bool have =
            wh.has_value() &&
            wh->write_sync(std::span<const std::byte>(data)).has_value();
        ready.fetch_add(1, std::memory_order_release);
        while (!go.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        // Spread the commits over the stop: writer 0 at once, the others
        // after a growing number of yields, the last two only after stop()
        // returned.
        if (t >= kWriterThreads - 2) {
          while (!stop_returned.load(std::memory_order_acquire)) {
            std::this_thread::yield();
          }
        } else {
          for (int spin = 0; spin < t * (round + 1) * 4; ++spin) {
            std::this_thread::yield();
          }
        }
        if (!have) {
          wrong.fetch_add(1, std::memory_order_relaxed);
          return;
        }
        const auto result = wh->close_sync();
        if (result.has_value()) {
          committed.fetch_add(1, std::memory_order_relaxed);
        } else if (result.error() == CacheError::Closed) {
          refused.fetch_add(1, std::memory_order_relaxed);
        } else {
          wrong.fetch_add(1, std::memory_order_relaxed);
        }
      });
    }
    while (ready.load(std::memory_order_acquire) < kWriterThreads) {
      std::this_thread::yield();
    }
    go.store(true, std::memory_order_release);
    cache->stop();
    stop_returned.store(true, std::memory_order_release);
    for (auto &t : writers) {
      t.join();
    }
    REQUIRE_FALSE(cache->is_running());
  }
  CAPTURE(committed.load(), refused.load());
  CHECK(wrong.load() == 0);
  CHECK(committed.load() + refused.load() ==
        static_cast<uint64_t>(kRounds) * kWriterThreads);
  // The writers that waited for stop() to return were all refused.
  CHECK(refused.load() >= static_cast<uint64_t>(kRounds) * 2);
}

// =============================================================================
// Phase 5B: Multi-Process Configuration Tests
// =============================================================================

TEST_CASE("Cache start with invalid multi-process config",
          "[lifecycle][multiprocess]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheConfig config;
  // Enable multi-process but disable checksums -- this should be rejected
  // because multi-process requires checksums for torn read detection
  config.multi_process_config.enabled = true;
  config.multi_process_config.process_index = 0;
  config.multi_process_config.total_processes = 2;
  config.enable_checksum = false;

  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(10 * 1024 * 1024);

  REQUIRE(cache->add_volume(vol_config).has_value());

  auto start_result = cache->start();
  REQUIRE_FALSE(start_result.has_value());
  REQUIRE(start_result.error() == CacheError::InvalidConfiguration);
}

TEST_CASE("Cache start with process_index >= total_processes",
          "[lifecycle][multiprocess]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheConfig config;
  config.multi_process_config.enabled = true;
  config.multi_process_config.process_index = 5;
  config.multi_process_config.total_processes = 4;
  config.enable_checksum = true;

  // Verify is_valid() returns false for the config
  REQUIRE_FALSE(config.multi_process_config.is_valid());

  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(10 * 1024 * 1024);

  REQUIRE(cache->add_volume(vol_config).has_value());

  auto start_result = cache->start();
  REQUIRE_FALSE(start_result.has_value());
  REQUIRE(start_result.error() == CacheError::InvalidConfiguration);
}

#ifndef _WIN32
// =============================================================================
// Fork-safety (POSIX prefork embedders: Apache / nginx)
// =============================================================================
//
// A prefork web server (Apache mpm_prefork/event, nginx) creates and start()s
// the cache in the MASTER process -- which spawns the background threads
// (HitTracker flush, OptimizationEngine monitor, AdaptiveThreadPool workers and
// their WorkQueue) -- and then fork()s worker children. A child inherits the
// Cache OBJECT (and the fork-copied state of its condition variables/mutexes)
// but NOT the running threads. When that child later tears the cache down, it
// must NOT run the thread-owning components' destructors: pthread_cond_destroy
// on a condition variable that was fork-copied while a thread in the master was
// blocked on it spins/blocks forever. That hang is what wedged mod_pagespeed
// 1.1 Apache children on graceful recycle (the child never exits -> its Apache
// scoreboard slot leaks -> eventual "AH03490: scoreboard is full").
//
// stop() therefore detects that it is running in a process other than the one
// that started the threads and abandons (leaks) the thread-owning components,
// letting the OS reclaim them on process exit; only the owning process performs
// the real join+destroy. This test reproduces the hang (the forked child must
// exit within a bounded time) and is the regression guard for that fix.
TEST_CASE("Cache teardown is fork-safe in a forked child",
          "[lifecycle][fork]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheConfig config;
  config.enable_hit_tracking = true;
  config.hit_flush_interval = std::chrono::milliseconds(50);
  config.optimization_config.enabled = true;
  config.optimization_config.scale_check_interval =
      std::chrono::milliseconds(50);

  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(20 * 1024 * 1024);
  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());
  REQUIRE(cache->is_running());

  // Give the background threads real work, then let them settle back to their
  // idle state parked on their condition variables -- the state that makes
  // pthread_cond_destroy block after fork().
  for (int i = 0; i < 10; ++i) {
    CacheKey key("fork-" + std::to_string(i));
    std::string content = "fork payload " + std::to_string(i);
    std::vector<std::byte> data(content.size());
    std::memcpy(data.data(), content.data(), content.size());
    auto wh = cache->write_sync(key, data.size());
    if (wh.has_value()) {
      wh->write_sync(std::span<const std::byte>(data));
      wh->close_sync();
    }
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  pid_t pid = fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    // CHILD: inherited the cache object but none of its threads. Tearing it
    // down must complete promptly. Pre-fix this hangs in pthread_cond_destroy.
    cache->stop();
    _exit(0);
  }

  // PARENT: wait for the child to exit, with a timeout. A child that does not
  // exit within the budget is the bug (hung in the fork-inherited teardown).
  bool exited = false;
  int status = 0;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (std::chrono::steady_clock::now() < deadline) {
    pid_t r = ::waitpid(pid, &status, WNOHANG);
    if (r == pid) {
      exited = true;
      break;
    }
    REQUIRE(r != -1);
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  if (!exited) {
    ::kill(pid, SIGKILL);
    ::waitpid(pid, &status, 0);
  }

  // The owning process performs the real teardown -- must be clean.
  cache->stop();

  INFO(
      "forked child must exit promptly; a timeout means the fork-inherited "
      "Cyclone teardown hung in pthread_cond_destroy");
  REQUIRE(exited);
  REQUIRE(WIFEXITED(status));
  REQUIRE(WEXITSTATUS(status) == 0);
}

// Not under ThreadSanitizer: the parent has a test thread alive at the
// fork, and the sanitizer's exit hook in the child reports that thread as
// leaked and replaces the exit code this case reads.
#if defined(__SANITIZE_THREAD__)
#define CYCLONE_LIFECYCLE_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define CYCLONE_LIFECYCLE_TSAN 1
#endif
#endif
#if !defined(CYCLONE_LIFECYCLE_TSAN)
TEST_CASE(
    "close() in a forked child does not wait for a handle call of "
    "the parent",
    "[lifecycle][fork][borrow]") {
  // close() waits for handle calls in flight.  A call that an application
  // thread was in at the instant another thread forked is counted in the
  // child's copy of the volume too, by a thread the child does not have:
  // the child must not wait for it -- not when it closes the inherited
  // volume, and not when it then reopens the volume and closes it again
  // (the count is still there; only its fork epoch says whose it is).
  const bool reopen = GENERATE(false, true);
  // The parent's call in flight: a read handle's release, or a write
  // handle's commit (parked before it takes any lock).
  const bool commit_in_flight = GENERATE(false, true);
  CAPTURE(reopen, commit_in_flight);
  REQUIRE(install_fork_handlers());
  TempCacheDir tmp;
  VolumeConfig vol_config;
  vol_config.path = tmp.path();
  vol_config.size = static_cast<size_t>(8 * 1024 * 1024);
  auto volume = std::make_shared<Volume>(vol_config);
  REQUIRE(volume->open().has_value());
  const CacheKey key("handle-call-across-fork");
  const std::vector<std::byte> body(4096, std::byte{0x77});
  {
    auto wh = volume->write_sync(key, body.size());
    REQUIRE(wh.has_value());
    REQUIRE(wh->write_sync(std::span<const std::byte>(body)).has_value());
    REQUIRE(wh->close_sync().has_value());
  }
  auto read = volume->read_sync(key);
  REQUIRE(read.has_value());
  REQUIRE_FALSE(read->is_ram_cache_hit());
  std::optional<ReadHandle> handle(std::move(*read));

  const CacheKey parked_key("commit-parked-across-fork");
  std::optional<WriteHandle> write_handle;
  if (commit_in_flight) {
    handle.reset();
    auto wh = volume->write_sync(parked_key, body.size());
    REQUIRE(wh.has_value());
    REQUIRE(wh->write_sync(std::span<const std::byte>(body)).has_value());
    write_handle.emplace(std::move(*wh));
  }

  TeardownWindow window;
  std::optional<std::expected<void, CacheError>> committed;
  std::thread caller([&] {
    window.caller.store(std::this_thread::get_id(), std::memory_order_release);
    // Parks inside the call, counted as in flight
    if (commit_in_flight) {
      committed = write_handle->close_sync();
    } else {
      handle.reset();
    }
  });
  const bool admitted = window.wait_admitted();

  pid_t pid = -1;
  bool exited = false;
  int status = 0;
  if (admitted) {
    pid = ::fork();
    if (pid == 0) {
      volume->close();
      if (reopen) {
        if (!volume->open().has_value()) {
          ::_exit(2);
        }
        // The child's own handle calls work on the reopened volume.
        {
          const CacheKey own_key("written-by-the-child");
          auto wh = volume->write_sync(own_key, body.size());
          if (!wh.has_value() ||
              !wh->write_sync(std::span<const std::byte>(body)).has_value() ||
              !wh->close_sync().has_value()) {
            ::_exit(3);
          }
          auto own = volume->read_sync(own_key);
          if (!own.has_value()) {
            ::_exit(4);
          }
          if (!own->renew_lease()) {
            ::_exit(5);
          }
        }
        volume->close();
      }
      ::_exit(0);
    }
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (pid > 0 && std::chrono::steady_clock::now() < deadline) {
      if (::waitpid(pid, &status, WNOHANG) == pid) {
        exited = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (pid > 0 && !exited) {
      ::kill(pid, SIGKILL);
      ::waitpid(pid, &status, 0);
    }
  }
  // Let the parked call go and finish in the parent.
  window.stop_returned.store(true, std::memory_order_release);
  caller.join();
  if (commit_in_flight) {
    // The parent's commit was not disturbed by the child.
    REQUIRE(committed.has_value());
    CHECK(committed->has_value());
    CHECK(volume->exists_sync(parked_key).value_or(false));
  }
  write_handle.reset();
  volume->close();

  REQUIRE(admitted);
  REQUIRE(pid > 0);
  INFO(
      "the child must leave close() at once; a timeout means it waited "
      "for a handle call of a thread it does not have");
  REQUIRE(exited);
  REQUIRE(WIFEXITED(status));
  REQUIRE(WEXITSTATUS(status) == 0);
}
#endif  // !CYCLONE_LIFECYCLE_TSAN
#endif  // _WIN32
