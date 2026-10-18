// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// With sync_on_write, one durability flush runs per commit.  The flush must
// not run under the stripe mutex and the cross-process write lock: the
// ordering invariant it serves (document data durable BEFORE the directory
// entry that points at it is inserted) needs it to complete only before the
// directory insert, not before the cursor advance or the lock release --
// and a flush under those locks serializes every writer of the stripe
// behind device latency.

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "core/volume.hpp"
#include "cyclone/config.hpp"
#include "cyclone/error.hpp"
#include "cyclone/key.hpp"
#include "support/temp_cache.hpp"

using namespace cyclone;

namespace {

constexpr size_t kVolumeBytes = size_t{4} * 1024 * 1024;
constexpr size_t kDocBytes = 2048;
constexpr int kThreads = 16;
constexpr int kPerThread = 2;
constexpr int kDocs = kThreads * kPerThread;

// Makes each fsync cost ~2 ms regardless of the filesystem (test seam).
struct FsyncPause {
  FsyncPause() { Volume::s_fsync_pause_us_for_test.store(2000); }
  ~FsyncPause() { Volume::s_fsync_pause_us_for_test.store(0); }
  FsyncPause(const FsyncPause &) = delete;
  FsyncPause &operator=(const FsyncPause &) = delete;
};

std::vector<std::byte> make_content(std::byte fill) {
  return std::vector<std::byte>(kDocBytes, fill);
}

bool write_doc(Volume &volume, const CacheKey &key,
               std::span<const std::byte> content) {
  auto wh = volume.write_sync(key, content.size());
  if (!wh) {
    return false;
  }
  return wh->write_sync(content).has_value() && wh->close_sync().has_value();
}

std::shared_ptr<Volume> open_volume(const std::string &path) {
  VolumeConfig vc;
  vc.path = path;
  vc.size = kVolumeBytes;
  // One stripe: every commit contends on the same stripe mutex (and, with
  // the mmap directory, the same cross-process write lock).
  vc.stripe_size = kVolumeBytes;
  vc.sync_on_write = true;
  vc.verify_checksum_on_read = true;
  MultiProcessConfig mp;
  mp.set_enabled(true).set_process_index(0).set_total_processes(1);
  auto volume = std::make_shared<Volume>(vc, mp);
  REQUIRE(volume->open().has_value());
  return volume;
}

}  // namespace

TEST_CASE(
    "sync_on_write: concurrent commits to one stripe overlap instead of "
    "serializing behind the flush",
    "[volume][concurrent][multiprocess][regression]") {
  FsyncPause pause;
  TempCacheDir tmp("syncwrite");
  auto volume = open_volume(tmp.path());
  const auto content = make_content(std::byte{0x42});

  // Sequential baseline: one commit at a time, flush latency included.
  const auto seq_t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < kDocs; ++i) {
    REQUIRE(write_doc(*volume, CacheKey("sync-write-seq-" + std::to_string(i)),
                      content));
  }
  const auto seq_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - seq_t0)
                          .count();

  // The same commits from kThreads writers at once.  Serialized behind the
  // flush this costs the same as the sequential run; with the flush off the
  // locks it is a small fraction of it.
  std::atomic<int> failures{0};
  const auto par_t0 = std::chrono::steady_clock::now();
  std::vector<std::thread> writers;
  for (int t = 0; t < kThreads; ++t) {
    writers.emplace_back([&, t] {
      for (int i = 0; i < kPerThread; ++i) {
        if (!write_doc(*volume,
                       CacheKey("sync-write-par-" + std::to_string(t) + "-" +
                                std::to_string(i)),
                       content)) {
          failures.fetch_add(1);
        }
      }
    });
  }
  for (auto &writer : writers) {
    writer.join();
  }
  const auto par_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - par_t0)
                          .count();

  REQUIRE(failures.load() == 0);
  CAPTURE(seq_ms, par_ms);
  REQUIRE(seq_ms >= kDocs);  // the seam makes each flush ~2 ms
  REQUIRE(par_ms * 2 < seq_ms);

  // Every commit landed and serves its bytes back (the flush still
  // completes before each directory insert).
  for (int i = 0; i < kDocs; ++i) {
    auto rh = volume->read_sync(CacheKey("sync-write-seq-" + std::to_string(i)));
    REQUIRE(rh.has_value());
    REQUIRE(rh->content().size() == kDocBytes);
  }
  for (int t = 0; t < kThreads; ++t) {
    for (int i = 0; i < kPerThread; ++i) {
      auto rh = volume->read_sync(CacheKey("sync-write-par-" +
                                           std::to_string(t) + "-" +
                                           std::to_string(i)));
      REQUIRE(rh.has_value());
      REQUIRE(rh->content().size() == kDocBytes);
    }
  }
}
