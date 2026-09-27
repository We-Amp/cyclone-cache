// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "cyclone/cache.hpp"
#include "cyclone/config.hpp"
#include "cyclone/error.hpp"
#include "cyclone/key.hpp"
// fingerprint_cache_path -- declared unconditionally in volume.hpp, so this
// include must be unconditional too (MSVC skips a _WIN32-guarded include and
// then cannot resolve the helper the size-change / checksum tests call).
#include "core/volume.hpp"

#ifndef _WIN32
// For the concurrent-open regression test.
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../../src/core/mmap_directory.hpp"
#include "support/liveness_file.hpp"
#endif

#ifdef _WIN32
#include <process.h>
#define MP_GETPID _getpid
#else
#define MP_GETPID getpid
#endif

using namespace cyclone;

namespace {

std::string get_temp_path(const std::string &suffix) {
  // Unique per process (getpid) AND per call (counter): a run on a persistent
  // /tmp never reopens another run's fingerprinted cache file.
  static std::atomic<int> counter{0};
  return (std::filesystem::temp_directory_path() /
          ("cyclone_mp_test_" + suffix + "_" + std::to_string(MP_GETPID()) +
           "_" + std::to_string(counter.fetch_add(1)) + ".cache"))
      .string();
}

// Remove the raw path AND the structural-fingerprint sibling(s)
// ("<stem>-<fmt>-<hash><ext>") Cache::add_volume() actually opens, so the files
// the Volume uses do not survive across runs on a persistent /tmp.  Test-only
// directory iteration.
void cleanup_temp_file(const std::string &path) {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::remove(path, ec);
  const fs::path p(path);
  const std::string prefix = p.stem().string() + "-";
  for (fs::directory_iterator it(p.parent_path(), ec), end; it != end && !ec;
       it.increment(ec)) {
    const std::string n = it->path().filename().string();
    if (n.size() > prefix.size() && n.compare(0, prefix.size(), prefix) == 0) {
      std::error_code e2;
      fs::remove(it->path(), e2);
    }
  }
}

}  // namespace

TEST_CASE("MultiProcessConfig validation", "[multiprocess][config]") {
  SECTION("Default config is valid") {
    MultiProcessConfig config;
    REQUIRE(config.is_valid());
    REQUIRE_FALSE(config.enabled);
    REQUIRE(config.process_index == 0);
    REQUIRE(config.total_processes == 1);
  }

  SECTION("Valid enabled config") {
    MultiProcessConfig config;
    config.enabled = true;
    config.process_index = 0;
    config.total_processes = 4;
    REQUIRE(config.is_valid());
  }

  SECTION("Invalid: process_index >= total_processes") {
    MultiProcessConfig config;
    config.enabled = true;
    config.process_index = 4;
    config.total_processes = 4;
    REQUIRE_FALSE(config.is_valid());
  }

  SECTION("Invalid: total_processes = 0") {
    MultiProcessConfig config;
    config.enabled = true;
    config.process_index = 0;
    config.total_processes = 0;
    REQUIRE_FALSE(config.is_valid());
  }

  SECTION("Builder methods work correctly") {
    MultiProcessConfig config;
    config.set_enabled(true)
        .set_process_index(2)
        .set_total_processes(8)
        .set_max_read_retries(3);

    REQUIRE(config.enabled);
    REQUIRE(config.process_index == 2);
    REQUIRE(config.total_processes == 8);
    REQUIRE(config.max_read_retries == 3);
    REQUIRE(config.is_valid());
  }
}

TEST_CASE("CacheConfig multi-process builder", "[multiprocess][config]") {
  SECTION("set_multi_process enables and configures") {
    CacheConfig config;
    config.set_multi_process(1, 4);

    REQUIRE(config.multi_process_config.enabled);
    REQUIRE(config.multi_process_config.process_index == 1);
    REQUIRE(config.multi_process_config.total_processes == 4);
  }

  SECTION("set_multi_process_config sets full config") {
    MultiProcessConfig mp_config;
    mp_config.enabled = true;
    mp_config.process_index = 3;
    mp_config.total_processes = 6;
    mp_config.max_read_retries = 5;

    CacheConfig config;
    config.set_multi_process_config(mp_config);

    REQUIRE(config.multi_process_config.enabled);
    REQUIRE(config.multi_process_config.process_index == 3);
    REQUIRE(config.multi_process_config.total_processes == 6);
    REQUIRE(config.multi_process_config.max_read_retries == 5);
  }
}

TEST_CASE("Cache start validates multi-process config",
          "[multiprocess][lifecycle]") {
  std::string cache_path = get_temp_path("validation");

  SECTION("Invalid config rejected at start") {
    CacheConfig config;
    config.set_multi_process(5, 4);  // Invalid: index >= total

    auto cache_result = Cache::create(config);
    REQUIRE(cache_result.has_value());
    auto &cache = *cache_result;

    auto add_result =
        cache->add_volume(cache_path, static_cast<size_t>(10 * 1024 * 1024));
    REQUIRE(add_result.has_value());

    auto start_result = cache->start();
    REQUIRE_FALSE(start_result.has_value());
    REQUIRE(start_result.error() == CacheError::InvalidConfiguration);
  }

  SECTION("Multi-process requires checksum enabled") {
    CacheConfig config;
    config.set_multi_process(0, 2);
    config.set_enable_checksum(false);  // Disable checksum

    auto cache_result = Cache::create(config);
    REQUIRE(cache_result.has_value());
    auto &cache = *cache_result;

    auto add_result =
        cache->add_volume(cache_path, static_cast<size_t>(10 * 1024 * 1024));
    REQUIRE(add_result.has_value());

    auto start_result = cache->start();
    REQUIRE_FALSE(start_result.has_value());
    REQUIRE(start_result.error() == CacheError::InvalidConfiguration);
  }

  SECTION("Valid config starts successfully") {
    CacheConfig config;
    config.set_multi_process(0, 2);
    config.set_enable_checksum(true);

    auto cache_result = Cache::create(config);
    REQUIRE(cache_result.has_value());
    auto &cache = *cache_result;

    auto add_result =
        cache->add_volume(cache_path, static_cast<size_t>(10 * 1024 * 1024));
    REQUIRE(add_result.has_value());

    auto start_result = cache->start();
    REQUIRE(start_result.has_value());

    cache->stop();
  }

  cleanup_temp_file(cache_path);
}

TEST_CASE("Explicit non-page-aligned stripe_size stays offset-aligned",
          "[multiprocess][stripe]") {
  // A caller-supplied explicit stripe_size that isn't a page multiple must not
  // drift stripe offsets out of alignment: the mmap directory places seq_cst
  // uint64_t atomics at a fixed offset within each stripe, and a misaligned
  // atomic faults (SIGBUS on arm64).  init_stripes page-floors the explicit
  // size for exactly this reason.  Opening in multi-process mode maps every
  // stripe's directory, so a misaligned offset would fault here at open --
  // reaching stats() proves every stripe mapped at an aligned offset.
  std::string cache_path = get_temp_path("explicit_align");
  cleanup_temp_file(cache_path);

  CacheConfig config;
  config.set_multi_process(0, 2);
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(512) * 1024 * 1024;
  // Deliberately misaligned: 128MB + 1 byte.  Page-floored back to 128MB.
  vol_config.stripe_size = static_cast<size_t>(128) * 1024 * 1024 + 1;

  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  CacheStats stats = cache->stats();
  REQUIRE(stats.stripe_count >= 2);  // 512MB / 128MB -> 3 stripes

  cache->stop();
  cleanup_temp_file(cache_path);
}

TEST_CASE("Stripe ownership assignment", "[multiprocess][stripe]") {
  std::string cache_path = get_temp_path("ownership");

  SECTION("Process 0 of 2 owns even stripes") {
    CacheConfig config;
    config.set_multi_process(0, 2);

    auto cache_result = Cache::create(config);
    REQUIRE(cache_result.has_value());
    auto &cache = *cache_result;

    // Use a smaller volume with multiple stripes
    VolumeConfig vol_config;
    vol_config.path = cache_path;
    vol_config.size = static_cast<size_t>(512 * 1024 * 1024);  // 512MB
    vol_config.stripe_size =
        static_cast<size_t>(128 * 1024 * 1024);  // 128MB = 4 stripes

    auto add_result = cache->add_volume(vol_config);
    REQUIRE(add_result.has_value());

    auto start_result = cache->start();
    REQUIRE(start_result.has_value());

    // Write some data - should succeed for keys that hash to owned stripes
    CacheKey key1("test_key_1");
    auto write_result = cache->write_sync(key1, 100);

    // Result depends on which stripe the key hashes to
    // This test verifies the mechanism works, not specific key assignments
    if (write_result.has_value()) {
      auto &handle = *write_result;
      std::vector<std::byte> data(100, std::byte{0x42});
      handle.write_sync(data);
      handle.close_sync();

      // Should be readable
      auto read_result = cache->read_sync(key1);
      REQUIRE(read_result.has_value());
    } else {
      // Key hashed to non-owned stripe
      REQUIRE(write_result.error() == CacheError::NotOwned);
    }

    cache->stop();
  }

  cleanup_temp_file(cache_path);
}

TEST_CASE("Write rejection on non-owned stripe", "[multiprocess][write]") {
  std::string cache_path = get_temp_path("write_reject");

  // Create cache as process 0 of 2 with small stripe size
  CacheConfig config;
  config.set_multi_process(0, 2);

  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(512 * 1024 * 1024);
  vol_config.stripe_size = static_cast<size_t>(128 * 1024 * 1024);

  auto add_result = cache->add_volume(vol_config);
  REQUIRE(add_result.has_value());

  auto start_result = cache->start();
  REQUIRE(start_result.has_value());

  // Try many keys until we find one that gets rejected
  bool found_rejected = false;
  for (int i = 0; i < 100 && !found_rejected; ++i) {
    CacheKey key("reject_test_key_" + std::to_string(i));
    auto write_result = cache->write_sync(key, 100);
    if (!write_result.has_value() &&
        write_result.error() == CacheError::NotOwned) {
      found_rejected = true;
    }
  }

  // With 2 processes and 4 stripes, process 0 owns stripes 0 and 2
  // So roughly half of keys should be rejected
  REQUIRE(found_rejected);

  cache->stop();
  cleanup_temp_file(cache_path);
}

TEST_CASE("Read from non-owned stripe succeeds", "[multiprocess][read]") {
  // This test verifies that reads work regardless of stripe ownership.
  // Note: True cross-process reading requires mmap-backed directories,
  // which is not implemented. This test uses a single cache instance
  // to verify the read-from-any-stripe behavior.

  std::string cache_path = get_temp_path("read_nonowned");

  CacheConfig config;
  config.set_multi_process(0, 2);

  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(512 * 1024 * 1024);
  vol_config.stripe_size = static_cast<size_t>(128 * 1024 * 1024);

  auto add_result = cache->add_volume(vol_config);
  REQUIRE(add_result.has_value());

  auto start_result = cache->start();
  REQUIRE(start_result.has_value());

  // Find and write to a key that this process owns
  CacheKey owned_key;
  bool found_owned = false;
  for (int i = 0; i < 100 && !found_owned; ++i) {
    owned_key = CacheKey("read_test_key_" + std::to_string(i));
    auto write_result = cache->write_sync(owned_key, 100);
    if (write_result.has_value()) {
      auto &handle = *write_result;
      std::vector<std::byte> data(100, std::byte{0xCD});
      handle.write_sync(data);
      handle.close_sync();
      found_owned = true;
    }
  }
  REQUIRE(found_owned);

  // Read should succeed (same process)
  {
    auto read_result = cache->read_sync(owned_key);
    REQUIRE(read_result.has_value());
    auto &handle = *read_result;
    auto content = handle.content();
    REQUIRE(content.size() == 100);
    REQUIRE(content[0] == std::byte{0xCD});
  }

  cache->stop();
  cleanup_temp_file(cache_path);
}

TEST_CASE("Single-process backward compatibility", "[multiprocess][compat]") {
  std::string cache_path = get_temp_path("compat");

  SECTION("Default config has multi-process disabled") {
    CacheConfig config;
    REQUIRE_FALSE(config.multi_process_config.enabled);
  }

  SECTION("Cache works normally without multi-process config") {
    CacheConfig config;  // Default, multi-process disabled

    auto cache_result = Cache::create(config);
    REQUIRE(cache_result.has_value());
    auto &cache = *cache_result;

    auto add_result =
        cache->add_volume(cache_path, static_cast<size_t>(10 * 1024 * 1024));
    REQUIRE(add_result.has_value());

    auto start_result = cache->start();
    REQUIRE(start_result.has_value());

    // All writes should succeed (all stripes owned)
    CacheKey key("compat_test_key");
    {
      auto write_result = cache->write_sync(key, 100);
      REQUIRE(write_result.has_value());

      auto &handle = *write_result;
      std::vector<std::byte> data(100, std::byte{0x55});
      handle.write_sync(data);
      handle.close_sync();
    }

    {
      auto read_result = cache->read_sync(key);
      REQUIRE(read_result.has_value());
    }  // ReadHandle destroyed before cache->stop()

    cache->stop();
  }

  cleanup_temp_file(cache_path);
}

TEST_CASE("Remove operations respect ownership", "[multiprocess][remove]") {
  std::string cache_path = get_temp_path("remove");

  CacheConfig config;
  config.set_multi_process(0, 2);

  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(512 * 1024 * 1024);
  vol_config.stripe_size = static_cast<size_t>(128 * 1024 * 1024);

  auto add_result = cache->add_volume(vol_config);
  REQUIRE(add_result.has_value());

  auto start_result = cache->start();
  REQUIRE(start_result.has_value());

  // Find a key that process 0 owns (can write)
  CacheKey owned_key;
  bool found_owned = false;
  for (int i = 0; i < 100 && !found_owned; ++i) {
    owned_key = CacheKey("remove_test_owned_" + std::to_string(i));
    auto write_result = cache->write_sync(owned_key, 50);
    if (write_result.has_value()) {
      auto &handle = *write_result;
      std::vector<std::byte> data(50, std::byte{0x11});
      handle.write_sync(data);
      handle.close_sync();
      found_owned = true;
    }
  }
  REQUIRE(found_owned);

  // Remove should succeed for owned stripe
  auto remove_result = cache->remove_sync(owned_key);
  REQUIRE(remove_result.has_value());

  // Find a key that goes to non-owned stripe
  CacheKey non_owned_key;
  bool found_non_owned = false;
  for (int i = 0; i < 100 && !found_non_owned; ++i) {
    non_owned_key = CacheKey("remove_test_nonowned_" + std::to_string(i));
    auto write_result = cache->write_sync(non_owned_key, 50);
    if (!write_result.has_value() &&
        write_result.error() == CacheError::NotOwned) {
      found_non_owned = true;
    }
  }
  REQUIRE(found_non_owned);

  // Remove should fail for non-owned stripe
  auto remove_non_owned = cache->remove_sync(non_owned_key);
  REQUIRE_FALSE(remove_non_owned.has_value());
  REQUIRE(remove_non_owned.error() == CacheError::NotOwned);

  cache->stop();
  cleanup_temp_file(cache_path);
}

TEST_CASE("NotOwned error message", "[multiprocess][error]") {
  auto error_code = make_error_code(CacheError::NotOwned);
  REQUIRE(error_code.message() == "stripe not owned by this process");
}

TEST_CASE("InvalidConfiguration error message", "[multiprocess][error]") {
  auto error_code = make_error_code(CacheError::InvalidConfiguration);
  REQUIRE(error_code.message() == "invalid configuration");
}

TEST_CASE("Checksum mismatch treated as cache miss",
          "[multiprocess][checksum]") {
  // This test verifies that when multi-process mode is enabled,
  // a corrupted document (bad checksum) is treated as a cache miss,
  // not an error.

  std::string cache_path = get_temp_path("checksum_test");

  CacheConfig config;
  config.set_multi_process(0,
                           1);  // Single process but multi-process mode enabled
  config.set_enable_checksum(true);

  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  auto add_result =
      cache->add_volume(cache_path, static_cast<size_t>(10 * 1024 * 1024));
  REQUIRE(add_result.has_value());

  auto start_result = cache->start();
  REQUIRE(start_result.has_value());

  // Write a valid document
  CacheKey key("checksum_test_key");
  {
    auto write_result = cache->write_sync(key, 100);
    REQUIRE(write_result.has_value());
    auto &handle = *write_result;
    std::vector<std::byte> data(100, std::byte{0x42});
    handle.write_sync(data);
    handle.close_sync();
  }

  // Verify we can read it
  {
    auto read_result = cache->read_sync(key);
    REQUIRE(read_result.has_value());
  }

  cache->stop();

  // Now corrupt the file by flipping some bytes in the data area.  The cache
  // opens a structural-fingerprint filename (see fingerprint_cache_path), so
  // corrupt THAT file, not the pre-fingerprint path.
  {
    std::fstream file(fingerprint_cache_path(
                          cache_path, static_cast<size_t>(10 * 1024 * 1024),
                          /*stripe_size=*/0, /*mmap_directory=*/true),
                      std::ios::in | std::ios::out | std::ios::binary);
    REQUIRE(file.is_open());

    // Seek past the header (64 bytes) + directory area + document header
    // and corrupt some content bytes
    file.seekp(64 + 16384 * 10 + 200);  // Approximate location in data area
    char garbage[] = {'\xFF', '\xFF', '\xFF', '\xFF'};
    file.write(garbage, sizeof(garbage));
    file.close();
  }

  // Re-open the cache and try to read - should get NotFound (miss) not an error
  {
    CacheConfig config2;
    config2.set_multi_process(0, 1);
    config2.set_enable_checksum(true);

    auto cache2_result = Cache::create(config2);
    REQUIRE(cache2_result.has_value());
    auto &cache2 = *cache2_result;

    auto add2_result =
        cache2->add_volume(cache_path, static_cast<size_t>(10 * 1024 * 1024));
    REQUIRE(add2_result.has_value());

    auto start2_result = cache2->start();
    REQUIRE(start2_result.has_value());

    // Read should fail gracefully (NotFound, not Corrupted)
    // Note: With in-memory directory, we won't find the entry anyway after
    // restart This test verifies the checksum path doesn't crash or return
    // unexpected errors
    {
      auto read_result = cache2->read_sync(key);
      // Either NotFound (directory lost) or the read succeeds/fails gracefully
      // The key point is no crash and no Corrupted error propagated
      if (!read_result.has_value()) {
        // NotFound is expected (in-memory directory lost on restart)
        REQUIRE(read_result.error() == CacheError::NotFound);
      }
    }  // Handle must be destroyed before cache->stop()

    cache2->stop();
  }

  cleanup_temp_file(cache_path);
}

TEST_CASE("Multi-process read retries on transient failure",
          "[multiprocess][retry]") {
  // This test verifies the retry configuration is respected.
  // We can't easily simulate torn reads, but we can verify the config works.

  std::string cache_path = get_temp_path("retry_test");

  SECTION("max_read_retries = 0 means single attempt") {
    MultiProcessConfig mp_config;
    mp_config.set_enabled(true)
        .set_process_index(0)
        .set_total_processes(1)
        .set_max_read_retries(0);

    REQUIRE(mp_config.max_read_retries == 0);

    CacheConfig config;
    config.set_multi_process_config(mp_config);

    auto cache_result = Cache::create(config);
    REQUIRE(cache_result.has_value());
    auto &cache = *cache_result;

    auto add_result =
        cache->add_volume(cache_path, static_cast<size_t>(10 * 1024 * 1024));
    REQUIRE(add_result.has_value());

    auto start_result = cache->start();
    REQUIRE(start_result.has_value());

    // Write and read should work normally
    CacheKey key("retry_test_key");
    {
      auto write_result = cache->write_sync(key, 50);
      REQUIRE(write_result.has_value());
      auto &handle = *write_result;
      std::vector<std::byte> data(50, std::byte{0x11});
      handle.write_sync(data);
      handle.close_sync();
    }

    {
      auto read_result = cache->read_sync(key);
      REQUIRE(read_result.has_value());
    }

    cache->stop();
  }

  SECTION("max_read_retries = 5 is accepted") {
    MultiProcessConfig mp_config;
    mp_config.set_enabled(true)
        .set_process_index(0)
        .set_total_processes(1)
        .set_max_read_retries(5);

    REQUIRE(mp_config.max_read_retries == 5);
    REQUIRE(mp_config.is_valid());

    CacheConfig config;
    config.set_multi_process_config(mp_config);

    auto cache_result = Cache::create(config);
    REQUIRE(cache_result.has_value());
    auto &cache = *cache_result;

    auto add_result =
        cache->add_volume(cache_path, static_cast<size_t>(10 * 1024 * 1024));
    REQUIRE(add_result.has_value());

    auto start_result = cache->start();
    REQUIRE(start_result.has_value());

    // Basic operation should work
    CacheKey key("retry_test_key_5");
    {
      auto write_result = cache->write_sync(key, 50);
      REQUIRE(write_result.has_value());
      auto &handle = *write_result;
      std::vector<std::byte> data(50, std::byte{0x22});
      handle.write_sync(data);
      handle.close_sync();
    }

    {
      auto read_result = cache->read_sync(key);
      REQUIRE(read_result.has_value());
    }

    cache->stop();
  }

  cleanup_temp_file(cache_path);
}

TEST_CASE(
    "Multi-process does not fsync per write (durability via periodic sync)",
    "[multiprocess][durability]") {
  // Multi-process mode must NOT issue a full fsync of the shared
  // volume inode on every metadata write.  Per-write fsync collapses concurrent
  // writers into serialized jbd2 journal commits (an fsync convoy) and stalls
  // the integrator's serving path.  Durability is instead provided by the
  // periodic DirectorySyncer.  Here we disable the periodic sync (10-min
  // interval) so the ONLY fsyncs that could occur are per-write ones, and
  // assert their count stays near zero across a burst of writes.
  std::string cache_path = get_temp_path("no_perwrite_fsync");

  CacheConfig config;
  config.set_multi_process(0, 2);
  config.set_enable_checksum(true);
  config.set_directory_sync_interval(std::chrono::milliseconds(600000));

  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(32 * 1024 * 1024);

  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  const int K = 200;
  uint64_t before = cache->stats().fsyncs;
  for (int i = 0; i < K; ++i) {
    CacheKey key("k-" + std::to_string(i));
    auto write_result = cache->write_sync(key, 64);
    REQUIRE(write_result.has_value());
    auto &handle = *write_result;
    std::vector<std::byte> data(64, std::byte{0x42});
    handle.write_sync(data);
    handle.close_sync();
  }
  uint64_t after = cache->stats().fsyncs;

  INFO("fsync delta across " << K << " writes = " << (after - before));
  // Per-write fsync would add ~K; periodic-sync-only adds ~0 across the burst.
  CHECK((after - before) < static_cast<uint64_t>(K) / 10);

  cache->stop();
  cleanup_temp_file(cache_path);
}

TEST_CASE("Periodic sync makes multi-process writes recoverable after reopen",
          "[multiprocess][durability]") {
  // The complement to the no-per-write-fsync test: with per-write fsync gone,
  // the periodic DirectorySyncer must still make a written entry durable across
  // a close/reopen of the same volume file.
  std::string cache_path = get_temp_path("periodic_recover");
  CacheKey key("recover-key");

  {
    CacheConfig config;
    config.set_multi_process(0, 1);
    config.set_enable_checksum(true);
    config.set_directory_sync_interval(std::chrono::milliseconds(50));

    auto cache = *Cache::create(config);
    VolumeConfig vol;
    vol.path = cache_path;
    vol.size = static_cast<size_t>(16 * 1024 * 1024);
    REQUIRE(cache->add_volume(vol).has_value());
    REQUIRE(cache->start().has_value());

    auto handle = *cache->write_sync(key, 100);
    std::vector<std::byte> data(100, std::byte{0x37});
    handle.write_sync(data);
    handle.close_sync();

    // Wait for at least one periodic sync cycle (50ms interval) to publish
    // the entry durably, then stop.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    cache->stop();
  }

  {  // Reopen the same file in a fresh Cache; the entry must survive.
    CacheConfig config;
    config.set_multi_process(0, 1);
    config.set_enable_checksum(true);

    auto cache = *Cache::create(config);
    VolumeConfig vol;
    vol.path = cache_path;
    vol.size = static_cast<size_t>(16 * 1024 * 1024);
    REQUIRE(cache->add_volume(vol).has_value());
    REQUIRE(cache->start().has_value());

    CHECK(cache->read_sync(key).has_value());
    cache->stop();
  }

  cleanup_temp_file(cache_path);
}

TEST_CASE("ReadHandle lifetime safety - handle survives cache stop",
          "[multiprocess][lifetime]") {
  // This test verifies that ReadHandle destructor doesn't crash when
  // the cache has already been stopped (MappedFile destroyed).
  // The fix uses weak_ptr to detect when MappedFile is gone.

  std::string cache_path = get_temp_path("lifetime_test");

  CacheConfig config;
  config.set_enable_checksum(true);

  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  auto add_result =
      cache->add_volume(cache_path, static_cast<size_t>(10 * 1024 * 1024));
  REQUIRE(add_result.has_value());

  auto start_result = cache->start();
  REQUIRE(start_result.has_value());

  // Write some data
  CacheKey key("lifetime_test_key");
  {
    auto write_result = cache->write_sync(key, 100);
    REQUIRE(write_result.has_value());
    auto &handle = *write_result;
    std::vector<std::byte> data(100, std::byte{0x42});
    handle.write_sync(data);
    handle.close_sync();
  }

  // Get a read handle
  auto read_result = cache->read_sync(key);
  REQUIRE(read_result.has_value());

  // Intentionally keep the handle alive and stop the cache
  // This would crash before the weak_ptr fix
  cache->stop();

  // Now let the read handle go out of scope - should not crash
  // The weak_ptr in VolumeReadHandleImpl will be expired, so unmap is skipped
  // (destructor runs here when read_result goes out of scope)

  cleanup_temp_file(cache_path);
}

TEST_CASE("Multi-process volume isolates on geometry (size) change",
          "[multiprocess][compat]") {
  // Stripe layout (count, offsets, shared mmap directory positions) is derived
  // from the configured volume size.  As of structural-fingerprint filenames
  // (fingerprint_cache_path), a size change that alters the derived geometry
  // yields a DIFFERENT filename, so a differently-sized config opens a
  // DIFFERENT on-disk file: the two geometries never share a ring, so there is
  // no cross-process divergence to reset or refuse.  This SUPERSEDES the old
  // same-file geometry reset gate (which now survives only as the stripe_count
  // hash-collision / pre-fingerprint-legacy backstop in Volume::open_locked).
  // Two sizes that map to the SAME derived geometry share a file and preserve
  // each other's data -- see the M1 regression in test_fingerprint_filenames.
  const size_t k10MB = static_cast<size_t>(10 * 1024 * 1024);
  const size_t k12MB = static_cast<size_t>(12 * 1024 * 1024);
  std::string cache_path = get_temp_path("geometry_change");
  auto fp = [&](size_t size) {
    return fingerprint_cache_path(cache_path, size, /*stripe_size=*/0,
                                  /*mmap_directory=*/true);
  };
  std::remove(fp(k10MB).c_str());
  std::remove(fp(k12MB).c_str());

  CacheKey key("geometry-test-key");
  std::vector<std::byte> test_data(100, std::byte{0x5A});

  auto make_cache = [&](size_t size, bool auto_reset) {
    CacheConfig config;
    config.set_multi_process(0, 1);
    config.set_enable_checksum(true);

    auto cache_result = Cache::create(config);
    REQUIRE(cache_result.has_value());
    auto cache = std::move(*cache_result);

    VolumeConfig vol_config;
    vol_config.path = cache_path;
    vol_config.size = size;
    vol_config.auto_reset_on_incompatible = auto_reset;
    REQUIRE(cache->add_volume(vol_config).has_value());
    return cache;
  };

  // First instance at 10MB: write an entry to the 10MB-geometry file.
  {
    auto cache = make_cache(k10MB, true);
    REQUIRE(cache->start().has_value());

    auto wh = cache->write_sync(key, test_data.size());
    REQUIRE(wh.has_value());
    REQUIRE(wh->write_sync(test_data).has_value());
    REQUIRE(wh->close_sync().has_value());
    REQUIRE(cache->read_sync(key).has_value());
    cache->stop();
  }

  // Reopen at a DIFFERENT size with auto-reset OFF: the differing geometry
  // resolves to a DIFFERENT file, so the open SUCCEEDS cleanly (never refused
  // as incompatible) and the entry is absent -- this is a fresh, isolated file.
  {
    auto cache = make_cache(k12MB, false);
    REQUIRE(cache->start().has_value());
    REQUIRE_FALSE(cache->read_sync(key).has_value());
    cache->stop();
  }

  // The original 10MB file was never touched: reopening at 10MB (same geometry
  // -> same file) still serves the original entry, with no reset.
  {
    auto cache = make_cache(k10MB, false);
    REQUIRE(cache->start().has_value());
    REQUIRE(cache->read_sync(key).has_value());
    cache->stop();
  }

  cleanup_temp_file(fp(k10MB));
  cleanup_temp_file(fp(k12MB));
  cleanup_temp_file(cache_path);
}

TEST_CASE("Cache stop joins an in-flight directory sync",
          "[multiprocess][teardown]") {
  // Regression test for the DirectorySyncer/Volume teardown race: the
  // periodic sync thread iterates the volume list, so Cache::stop() must
  // JOIN that thread (unbounded) before any volume is closed or
  // destroyed, and must do so while NOT holding the volume-list lock the
  // sync pass acquires.  A regression to the old bounded-join-and-leak
  // pattern surfaces here as a use-after-free under ASan; a regression to
  // joining under the exclusive lock deadlocks (the test hangs).
  //
  // A 1 ms sync interval makes stop() land while a sync pass is in flight
  // with high probability, and the rapid create/start/stop/destroy cycles
  // mirror the pattern that exposed the original race.
  std::string cache_path = get_temp_path("sync_teardown");

  for (int cycle = 0; cycle < 20; ++cycle) {
    CacheConfig config;
    config.ram_cache_size = 0;
    config.set_multi_process(0, 1);  // Persistent (mmap'd) directories
    config.set_directory_sync_interval(std::chrono::milliseconds(1));

    auto cache_result = Cache::create(config);
    REQUIRE(cache_result.has_value());
    auto &cache = *cache_result;

    VolumeConfig vol_config;
    vol_config.path = cache_path;
    vol_config.size = static_cast<size_t>(4 * 1024 * 1024);

    REQUIRE(cache->add_volume(vol_config).has_value());
    REQUIRE(cache->start().has_value());

    // A couple of writes so the sync passes have dirty pages to flush.
    for (int i = 0; i < 3; ++i) {
      CacheKey key("teardown-key-" + std::to_string(cycle) + "-" +
                   std::to_string(i));
      std::vector<std::byte> data(1024, std::byte{0x5A});
      auto wh = cache->write_sync(key, data.size());
      if (wh.has_value()) {
        (void)wh->write_sync(data);
        (void)wh->close_sync();
      }
    }

    // Let at least one sync pass start.
    std::this_thread::sleep_for(std::chrono::milliseconds(3));

    cache->stop();
    // ~Cache at scope exit destroys the volumes; with the join in place no
    // sync thread can still be alive to observe that.
  }

  cleanup_temp_file(cache_path);
}

#ifndef _WIN32
// =============================================================================
// Volume::open() must serialize against a concurrent creator's
// initialization.
// =============================================================================
//
// The two-stage O_EXCL open only picks which process CREATES the file;
// before the fix, the loser proceeded immediately and could observe any
// prefix of the creator's init sequence: read a not-yet-written header and
// run a SECOND reset() that zero-fills the shared directory underneath the
// creator's live mmap, race MmapDirectory::init against the creator's
// zero-fill, or fstat a 0-byte file.  The fix holds an exclusive flock
// across validate + reset() + init_stripes(), so a non-creator blocks until
// the creator finished (or died -- the kernel then drops the lock and the
// next opener repairs the partial state).
//
// Deterministic regression guard: the parent simulates a creator caught
// mid-initialization by creating the file with O_EXCL and holding the same
// exclusive lock Volume::open() takes, WITHOUT writing a header.  A child
// process then calls Volume::open() on the same path:
//  - pre-fix:  the child does not wait, "initializes" the volume itself,
//              and reports completion while the creator lock is still held
//              (this test then FAILS on completed_while_locked);
//  - post-fix: the child blocks inside Volume::open() until the parent
//              releases the lock, then completes successfully -- which also
//              covers the creator-died-mid-init path: the child finds an
//              invalid header and runs reset() itself.
TEST_CASE("Volume::open waits for a concurrent creator's initialization",
          "[multiprocess][open]") {
  const std::string path = get_temp_path("open_race_60");
  cleanup_temp_file(path);

  // Parent plays the creator that won the O_EXCL race but has not
  // initialized anything yet and holds the init lock.
  //
  // The init lock is a fcntl OFD exclusive lock on a single past-EOF byte (see
  // the lock substrate in volume.cpp) -- NOT flock.  Simulating it with flock
  // would only conflict on macOS (where flock and fcntl share a lock space);
  // on Linux the two are independent lock spaces, so a flock here would let the
  // child's fcntl init lock through and the test would spuriously pass the
  // creator-holds-lock window.  Take the SAME fcntl lock the child will
  // contend on.
  constexpr off_t kInitLockByte = 0x7FFFFFFFFFFFFFFELL;
  auto init_lock = [&](int fd, int type) {
    struct flock fl{};
    fl.l_type = static_cast<short>(type);
    fl.l_whence = SEEK_SET;
    fl.l_start = kInitLockByte;
    fl.l_len = 1;
    return ::fcntl(fd, F_OFD_SETLK, &fl);
  };
  int creator_fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0666);
  REQUIRE(creator_fd >= 0);
  REQUIRE(init_lock(creator_fd, F_WRLCK) == 0);

  int pipe_fds[2];
  REQUIRE(::pipe(pipe_fds) == 0);

  pid_t pid = fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    // CHILD: open the volume as a non-creator (EEXIST path).  Signal
    // completion through the pipe; the parent checks WHEN it arrives.
    ::close(pipe_fds[0]);
    VolumeConfig config;
    config.path = path;
    config.size = static_cast<size_t>(16 * 1024 * 1024);
    MultiProcessConfig mp;
    mp.set_enabled(true).set_process_index(0).set_total_processes(2);
    Volume volume(config, mp);
    auto open_result = volume.open();
    unsigned char ok = open_result.has_value() ? 1 : 0;
    (void)!::write(pipe_fds[1], &ok, 1);
    volume.close();
    _exit(ok != 0 ? 0 : 1);
  }

  // PARENT
  ::close(pipe_fds[1]);
  int flags = ::fcntl(pipe_fds[0], F_GETFL);
  REQUIRE(flags >= 0);
  REQUIRE(::fcntl(pipe_fds[0], F_SETFL, flags | O_NONBLOCK) == 0);

  // Hold the lock for a window; the child must NOT complete during it.
  bool completed_while_locked = false;
  unsigned char child_result = 0;
  const auto hold_deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (std::chrono::steady_clock::now() < hold_deadline) {
    ssize_t n = ::read(pipe_fds[0], &child_result, 1);
    if (n == 1) {
      completed_while_locked = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  // Release the "creator's" lock; the child must now finish promptly.
  REQUIRE(init_lock(creator_fd, F_UNLCK) == 0);

  bool completed = completed_while_locked;
  const auto finish_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (!completed && std::chrono::steady_clock::now() < finish_deadline) {
    ssize_t n = ::read(pipe_fds[0], &child_result, 1);
    if (n == 1) {
      completed = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  if (!completed) {
    ::kill(pid, SIGKILL);
  }
  int status = 0;
  ::waitpid(pid, &status, 0);
  ::close(pipe_fds[0]);
  ::close(creator_fd);

  INFO(
      "a child Volume::open() completing while the creator init lock is "
      "held means non-creators do not wait for creator initialization");
  REQUIRE_FALSE(completed_while_locked);
  REQUIRE(completed);
  REQUIRE(child_result == 1);
  REQUIRE(WIFEXITED(status));
  REQUIRE(WEXITSTATUS(status) == 0);

  // The volume the child initialized must be openable by another process
  // identity (fresh Volume instance, other process_index).
  {
    VolumeConfig config;
    config.path = path;
    config.size = static_cast<size_t>(16 * 1024 * 1024);
    MultiProcessConfig mp;
    mp.set_enabled(true).set_process_index(1).set_total_processes(2);
    Volume volume(config, mp);
    REQUIRE(volume.open().has_value());
    volume.close();
  }

  cleanup_temp_file(path);
}

// =============================================================================
// The cross-process write lock must not force-release a
// live-but-stalled holder (which usurps its byte-range reservation and causes
// undetectable overlapping pwrites), yet must still recover the lock from a
// genuinely dead holder so a crash cannot deadlock the cache.
//
// The directory lives in MAP_SHARED|MAP_ANONYMOUS memory inherited across
// fork(), so parent and child operate the SAME lock words.  A child acquires
// the write lock and freezes with SIGSTOP mid-critical-section — alive but
// making no progress, exactly the scheduler-preemption / major-page-fault
// stall the audit describes — WITHOUT publishing shared_write_pos (the real
// reservation publishes only at the end of the critical section).
// =============================================================================
namespace {
constexpr size_t kWlBuckets = 64;
constexpr uint64_t kWlReservationBase = 0x4000;

// Map a fresh shared-anonymous directory region for a write-lock race test.
std::pair<void *, size_t> map_shared_directory() {
  const size_t region_size = MmapDirectory::required_size(kWlBuckets);
  void *base = ::mmap(nullptr, region_size, PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  return {base, region_size};
}
}  // namespace

TEST_CASE("Write lock waits on a live holder and recovers a dead one",
          "[multiprocess][writelock]") {
  auto [base, region_size] = map_shared_directory();
  REQUIRE(base != MAP_FAILED);
  auto dir_opt = MmapDirectory::init(
      std::span<std::byte>(static_cast<std::byte *>(base), region_size),
      kWlBuckets);
  REQUIRE(dir_opt.has_value());
  MmapDirectory &dir = *dir_opt;
  dir.set_shared_write_pos(kWlReservationBase);
  // The holder proves its liveness with a lock on this file (the volume
  // file, in production).
  LivenessFile liveness(dir);
  REQUIRE(liveness.ok());

  // Fixed protocol: prove the holder dead before recovering it.
  MmapDirectory::s_write_lock_presume_dead_for_test.store(false);

  int pfd[2];
  REQUIRE(::pipe(pfd) == 0);

  pid_t pid = fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    // CHILD: acquire and stall mid-critical-section, holding the lock without
    // publishing a new shared_write_pos.
    ::close(pfd[0]);
    auto token = dir.acquire_write_lock();
    uint64_t observed_base = dir.get_shared_write_pos();
    (void)!::write(pfd[1], &observed_base, sizeof(observed_base));
    ::raise(SIGSTOP);  // frozen but ALIVE, still holding the lock
    dir.release_write_lock(token);
    _exit(0);
  }

  // PARENT
  ::close(pfd[1]);
  uint64_t child_base = 0;
  REQUIRE(::read(pfd[0], &child_base, sizeof(child_base)) ==
          static_cast<ssize_t>(sizeof(child_base)));
  REQUIRE(child_base == kWlReservationBase);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  REQUIRE(::kill(pid, 0) == 0);  // holder alive (stopped)

  std::atomic<bool> done{false};
  MmapDirectory::WriteLockToken parent_token;
  std::thread contender([&] {
    parent_token = dir.acquire_write_lock();
    done.store(true, std::memory_order_release);
  });

  // The live-but-stalled holder must NOT be usurped: across a window far
  // shorter than the last-resort escalation budget the acquire stays blocked
  // and the holder's reservation base is untouched (no overlap).
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  CHECK_FALSE(done.load(std::memory_order_acquire));
  CHECK(dir.get_shared_write_pos() == kWlReservationBase);

  // Now CRASH the holder mid-critical-section.  Its liveness lock dies with
  // it; the waiter must then recover the lock.
  REQUIRE(::kill(pid, SIGKILL) == 0);
  int status = 0;
  ::waitpid(pid, &status, 0);

  contender.join();
  CHECK(done.load(std::memory_order_acquire));
  CHECK(parent_token.acquired);
  CHECK(parent_token.forced_release);  // recovered from the dead holder
  CHECK(parent_token.live_waits > 0);  // waited on the live holder first
  // Dead-holder recovery must land in the ROUTINE counter; the alertable
  // last-resort escalation (holder not proven dead) must NOT have fired.
  CHECK_FALSE(parent_token.escalated_takeover);

  dir.release_write_lock(parent_token);
  ::close(pfd[0]);
  ::munmap(base, region_size);
}

TEST_CASE(
    "Pre-fix presume-dead usurps a live write-lock holder — the closed bug",
    "[multiprocess][writelock]") {
  auto [base, region_size] = map_shared_directory();
  REQUIRE(base != MAP_FAILED);
  auto dir_opt = MmapDirectory::init(
      std::span<std::byte>(static_cast<std::byte *>(base), region_size),
      kWlBuckets);
  REQUIRE(dir_opt.has_value());
  MmapDirectory &dir = *dir_opt;
  dir.set_shared_write_pos(kWlReservationBase);

  // Opt into the OLD behavior: presume the holder dead after the spin budget.
  MmapDirectory::s_write_lock_presume_dead_for_test.store(true);

  int pfd[2];
  REQUIRE(::pipe(pfd) == 0);

  pid_t pid = fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    ::close(pfd[0]);
    auto token = dir.acquire_write_lock();
    uint64_t observed_base = dir.get_shared_write_pos();
    (void)!::write(pfd[1], &observed_base, sizeof(observed_base));
    ::raise(SIGSTOP);  // frozen but ALIVE, still holding the lock
    dir.release_write_lock(token);
    _exit(0);
  }

  ::close(pfd[1]);
  uint64_t child_base = 0;
  REQUIRE(::read(pfd[0], &child_base, sizeof(child_base)) ==
          static_cast<ssize_t>(sizeof(child_base)));
  REQUIRE(child_base == kWlReservationBase);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  REQUIRE(::kill(pid, 0) == 0);  // holder alive (stopped)

  // Pre-fix: the parent presumes the holder dead after the spin budget and
  // force-releases it — usurping a LIVE holder.
  auto parent_token = dir.acquire_write_lock();
  uint64_t parent_base = dir.get_shared_write_pos();

  // The bug in the flesh: the parent took the lock while the holder was still
  // alive, and would reserve the SAME base — an overlapping reservation that
  // both processes then pwrite into with no wrap event and no epoch bump.
  CHECK(parent_token.forced_release);
  CHECK(::kill(pid, 0) == 0);        // holder STILL alive at usurpation
  CHECK(parent_base == child_base);  // overlapping reservation base

  // Cleanup.
  MmapDirectory::s_write_lock_presume_dead_for_test.store(false);
  REQUIRE(::kill(pid, SIGKILL) == 0);
  int status = 0;
  ::waitpid(pid, &status, 0);
  dir.release_write_lock(parent_token);
  ::close(pfd[0]);
  ::munmap(base, region_size);
}
#endif  // _WIN32
