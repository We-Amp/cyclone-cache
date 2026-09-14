// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Hard-kill / reopen regression tests for the persistent (mmap'd) directory
//
// What these tests PIN:
//   - Reopening a volume after a hard process kill (_exit with no teardown)
//     succeeds — no crash, no corruption error.
//   - Every entry readable after reopen passes the read gauntlet with exactly
//     the content that was written.
//   - Readable count never exceeds written count.
//   - The periodic directory sync runs as an OWNED duty (counter advances
//     with hit tracking disabled) and completes end-to-end (the counter only
//     increments after every region flush and the fsync succeed).
//   - The directory_sync_interval config contract (default, builder,
//     0 = disabled).
//
// What these tests CANNOT pin:
//   - True power-loss durability: user space cannot drop the OS page cache,
//     so the parent reads the child's pages back regardless of any fsync.
//   - The data-sync-before-entry-publish ordering in the commit paths: that
//     invariant is pinned by the ordering comments in volume.cpp and code
//     review, not by this test.

#ifndef _WIN32  // fork()-based; not portable to Windows

#include <sys/wait.h>
#include <unistd.h>

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "cyclone/cache.hpp"
#include "cyclone/config.hpp"
#include "cyclone/key.hpp"
#include "support/temp_cache.hpp"

using namespace cyclone;

namespace {

constexpr size_t kVolumeSizeBytes = static_cast<size_t>(32 * 1024 * 1024);
constexpr int kSyncedEntries = 24;  // Covered by a completed directory sync
constexpr int kTailEntries = 8;     // Written right before the hard kill
constexpr auto kChildSyncInterval = std::chrono::milliseconds(50);

CacheKey entry_key(int index) {
  return CacheKey("power_loss_key_" + std::to_string(index));
}

// Deterministic per-entry payload with a mix of sizes (small / medium /
// large) so the read-back check catches any offset/size corruption.
std::vector<std::byte> entry_content(int index) {
  size_t size = 0;
  switch (index % 3) {
    case 0:
      size = 200;
      break;
    case 1:
      size = static_cast<size_t>(8 * 1024);
      break;
    default:
      size = static_cast<size_t>(100 * 1024);
      break;
  }
  std::vector<std::byte> data(size);
  for (size_t i = 0; i < size; ++i) {
    data[i] =
        static_cast<std::byte>((static_cast<size_t>(index) * 131 + i) & 0xFF);
  }
  return data;
}

// Cache config for the persistent-directory mode under test.  Multi-process
// mode is what enables the on-disk (mmap'd) directory; process 0-of-1 keeps
// every stripe owned.
CacheConfig persistent_config() {
  CacheConfig config;
  config.set_multi_process(0, 1);
  config.set_enable_checksum(true);
  config.ram_cache_size = 0;  // Force every read through the disk gauntlet
  config.optimization_config.enabled = false;
  return config;
}

// Poll the aggregated directory-sync counter until it reaches `target`.
// Bounded deadline instead of a fixed sleep so the wait is deterministic:
// the counter only advances when sync_directory() completed successfully.
bool wait_for_directory_syncs(Cache &cache, uint64_t target,
                              std::chrono::seconds deadline_budget) {
  const auto deadline = std::chrono::steady_clock::now() + deadline_budget;
  while (std::chrono::steady_clock::now() < deadline) {
    if (cache.stats().directory_syncs >= target) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}

// Runs in the forked child.  No Catch2 assertions here — failures are
// reported through distinct exit codes the parent turns into REQUIREs.
[[noreturn]] void child_write_and_die(const std::string &cache_path) {
  CacheConfig config = persistent_config();
  // The periodic directory sync must be an owned duty: disable hit tracking
  // so its flush-cycle fsync cannot mask a missing directory sync.
  config.enable_hit_tracking = false;
  config.directory_sync_interval = kChildSyncInterval;

  auto cache_result = Cache::create(config);
  if (!cache_result) _exit(2);
  auto &cache = *cache_result;

  if (!cache->add_volume(cache_path, kVolumeSizeBytes)) _exit(3);
  if (!cache->start()) _exit(4);

  auto write_entry = [&](int i) -> bool {
    auto content = entry_content(i);
    auto handle = cache->write_sync(entry_key(i), content.size());
    if (!handle) return false;
    if (!handle->write_sync(std::span<const std::byte>(content))) return false;
    if (!handle->close_sync()) return false;
    return true;
  };

  for (int i = 0; i < kSyncedEntries; ++i) {
    if (!write_entry(i)) _exit(5);
  }

  // Sync-invocation evidence: wait for the counter to advance by 2 past the
  // post-write baseline.  One increment could belong to a sync pass that was
  // already in flight during the last insert; two guarantee at least one
  // full sync_directory() pass started after the batch above was published
  // and completed.  This is what fails if the periodic sync is broken or
  // stubbed out — the reopen in the parent cannot detect that (page cache).
  uint64_t baseline = cache->stats().directory_syncs;
  if (!wait_for_directory_syncs(*cache, baseline + 2,
                                std::chrono::seconds(10))) {
    _exit(7);
  }

  // Tail writes inside the current sync window — on a real power loss these
  // are the entries the bounded-loss contract allows to disappear.
  for (int i = kSyncedEntries; i < kSyncedEntries + kTailEntries; ++i) {
    if (!write_entry(i)) _exit(6);
  }

  // Hard kill: no pending handles, no stop(), no destructors.
  _exit(0);
}

}  // namespace

TEST_CASE("Persistent directory reopens intact after a hard process kill",
          "[powerloss][multiprocess][lifecycle]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  pid_t pid = fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    child_write_and_die(cache_path);  // Never returns
  }

  // Wait for the child with a timeout so a wedged child fails the test
  // instead of hanging the suite.
  bool exited = false;
  int status = 0;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(30);
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
  REQUIRE(exited);
  REQUIRE(WIFEXITED(status));
  // Exit code 7 = the directory-sync counter never advanced in the child:
  // the owned periodic sync did not run (or did not complete successfully).
  REQUIRE(WEXITSTATUS(status) == 0);

  // Reopen the hard-killed volume in this (fresh) process.
  {
    CacheConfig config = persistent_config();
    // Deliberately ask for read verification OFF: persistent mode must force
    // it back on (torn DirEntry stores would otherwise be served as
    // garbage) — see Cache::add_volume().
    config.set_verify_checksum_on_read(false);

    auto cache_result = Cache::create(config);
    REQUIRE(cache_result.has_value());
    auto &cache = *cache_result;

    REQUIRE(cache->add_volume(cache_path, kVolumeSizeBytes).has_value());

    // Reopen after a hard kill must succeed.
    REQUIRE(cache->start().has_value());

    constexpr int kTotalEntries = kSyncedEntries + kTailEntries;
    int readable = 0;
    for (int i = 0; i < kTotalEntries; ++i) {
      auto read_result = cache->read_sync(entry_key(i));

      // The synced batch must be present after reopen.  NOTE: because the
      // parent shares the OS page cache with the killed child, presence here
      // pins the reopen/read path — it is NOT evidence the entries reached
      // stable media (that is what the sync-counter check in the child is
      // for).  Tail entries happen to be readable here too for the same
      // page-cache reason; on a real power loss they may be lost, so no
      // assertion requires them.
      if (i < kSyncedEntries) {
        INFO("synced entry " << i << " must be readable after reopen");
        REQUIRE(read_result.has_value());
      }

      if (!read_result.has_value()) {
        continue;  // A miss is an acceptable outcome for tail entries
      }

      // Every readable entry passed the read gauntlet — its content must be
      // exactly what was written.
      auto expected = entry_content(i);
      auto content = read_result->content();
      REQUIRE(content.size() == expected.size());
      REQUIRE(std::memcmp(content.data(), expected.data(), expected.size()) ==
              0);
      ++readable;
    }

    // Never more readable entries than were written.
    REQUIRE(readable <= kTotalEntries);
    REQUIRE(readable >= kSyncedEntries);

    cache->stop();
  }
}

TEST_CASE("Directory sync interval configuration", "[powerloss][config]") {
  SECTION("Default is 30 seconds") {
    CacheConfig config;
    REQUIRE(config.directory_sync_interval == std::chrono::milliseconds(30000));
  }

  SECTION("Builder sets the interval") {
    CacheConfig config;
    config.set_directory_sync_interval(std::chrono::milliseconds(250));
    REQUIRE(config.directory_sync_interval == std::chrono::milliseconds(250));
  }

  SECTION("Periodic sync advances the observable counter") {
    TempCacheDir tmp;
    std::string cache_path = tmp.path();

    CacheConfig config = persistent_config();
    config.enable_hit_tracking = false;  // Counter must advance on its own
    config.set_directory_sync_interval(std::chrono::milliseconds(20));

    auto cache_result = Cache::create(config);
    REQUIRE(cache_result.has_value());
    auto &cache = *cache_result;

    REQUIRE(cache->add_volume(cache_path, kVolumeSizeBytes).has_value());
    REQUIRE(cache->start().has_value());

    REQUIRE(wait_for_directory_syncs(*cache, 2, std::chrono::seconds(10)));

    cache->stop();
  }

  SECTION("Interval 0 (disabled) still starts and stops cleanly") {
    TempCacheDir tmp;
    std::string cache_path = tmp.path();

    CacheConfig config = persistent_config();
    config.set_directory_sync_interval(std::chrono::milliseconds(0));

    auto cache_result = Cache::create(config);
    REQUIRE(cache_result.has_value());
    auto &cache = *cache_result;

    REQUIRE(cache->add_volume(cache_path, kVolumeSizeBytes).has_value());
    REQUIRE(cache->start().has_value());

    auto content = entry_content(0);
    auto handle = cache->write_sync(entry_key(0), content.size());
    REQUIRE(handle.has_value());
    REQUIRE(
        handle->write_sync(std::span<const std::byte>(content)).has_value());
    REQUIRE(handle->close_sync().has_value());

    auto read_result = cache->read_sync(entry_key(0));
    REQUIRE(read_result.has_value());

    // Disabled means disabled: no periodic syncs happen.
    REQUIRE(cache->stats().directory_syncs == 0);

    cache->stop();
  }
}

#endif  // !_WIN32
