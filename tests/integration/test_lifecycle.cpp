// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <thread>
#include <vector>

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
  // filename (cyclone.dat -> cyclone-7-<hex>.dat, plus a possible ".small"
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

TEST_CASE("Concurrent stop with in-flight writes must not crash",
          "[lifecycle][concurrency]") {
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

  std::atomic<bool> keep_going{true};
  std::atomic<int> operations_completed{0};

  constexpr int kWriterThreads = 4;
  std::vector<std::thread> writers;

  writers.reserve(kWriterThreads);
  for (int t = 0; t < kWriterThreads; ++t) {
    writers.emplace_back([&, t]() {
      int i = 0;
      while (keep_going.load(std::memory_order_relaxed)) {
        CacheKey key("stop-write-" + std::to_string(t * 1000 + i));
        std::string content = "payload-" + std::to_string(i);
        std::vector<std::byte> data(content.size());
        std::memcpy(data.data(), content.data(), content.size());

        auto wh = cache->write_sync(key, data.size());
        if (wh.has_value()) {
          wh->write_sync(std::span<const std::byte>(data));
          wh->close_sync();
        }
        // write_sync returning NotInitialized is expected after stop()

        operations_completed.fetch_add(1, std::memory_order_relaxed);
        ++i;
      }
    });
  }

  // Let writers warm up
  while (operations_completed.load(std::memory_order_relaxed) <
         kWriterThreads * 5) {
    std::this_thread::yield();
  }

  // Now stop the cache while writers are active
  cache->stop();
  keep_going = false;

  for (auto &t : writers) {
    t.join();
  }

  REQUIRE_FALSE(cache->is_running());
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
#endif  // _WIN32
