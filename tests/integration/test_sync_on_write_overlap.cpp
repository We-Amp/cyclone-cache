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
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "core/volume.hpp"
#include "cyclone/alternate.hpp"
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
    auto rh =
        volume->read_sync(CacheKey("sync-write-seq-" + std::to_string(i)));
    REQUIRE(rh.has_value());
    REQUIRE(rh->content().size() == kDocBytes);
  }
  for (int t = 0; t < kThreads; ++t) {
    for (int i = 0; i < kPerThread; ++i) {
      auto rh = volume->read_sync(CacheKey(
          "sync-write-par-" + std::to_string(t) + "-" + std::to_string(i)));
      REQUIRE(rh.has_value());
      REQUIRE(rh->content().size() == kDocBytes);
    }
  }
}

TEST_CASE(
    "sync_on_write: a stripe wrap during the unlocked flush does not let the "
    "commit overwrite a newer write of the same key",
    "[volume][regression]") {
  TempCacheDir tmp("syncwrap");
  auto volume = open_volume(tmp.path());
  const CacheKey key("sync-write-wrap-key");
  const auto older = make_content(std::byte{0x11});
  const auto newer = make_content(std::byte{0x22});
  const std::vector<std::byte> filler(size_t{1024}, std::byte{0x33});

  // While the first commit sits in its flush (stripe mutex released), the
  // stripe wraps over the first commit's bytes and a newer write of the same
  // key lands.
  std::atomic<bool> fired{false};
  std::function<void()> hook = [&] {
    if (fired.exchange(true)) {
      return;
    }
    const uint64_t wraps_before = volume->stats().wrap_count;
    for (int i = 0; i < 20000 && volume->stats().wrap_count == wraps_before;
         ++i) {
      REQUIRE(write_doc(*volume,
                        CacheKey("sync-write-wrap-fill-" + std::to_string(i)),
                        filler));
    }
    REQUIRE(volume->stats().wrap_count > wraps_before);
    REQUIRE(write_doc(*volume, key, newer));
  };
  Volume::s_fsync_hook_for_test.store(&hook);
  struct Clear {
    ~Clear() { Volume::s_fsync_hook_for_test.store(nullptr); }
  } clear;

  // The first commit may report success or Busy; what it must never do is
  // point the key at its own, now overwritten, bytes.
  (void)write_doc(*volume, key, older);
  Volume::s_fsync_hook_for_test.store(nullptr);

  auto rh = volume->read_sync(key);
  REQUIRE(rh.has_value());
  REQUIRE(rh->content().size() == kDocBytes);
  REQUIRE(rh->content()[0] == std::byte{0x22});
}

TEST_CASE(
    "sync_on_write: a stripe wrap by another process during the flush of an "
    "alternate commit does not let it overwrite a newer write of the same "
    "alternate",
    "[volume][regression]") {
  TempCacheDir tmp("syncwrapalt");
  auto volume = open_volume(tmp.path());
  // The alternate commit holds its stripe mutex across the flush, so the
  // wrap and the newer write come from another view of the file, as from
  // another process (the write lock is released during the flush).
  auto peer = open_volume(tmp.path());
  const CacheKey key("sync-write-wrap-alt-key");
  const auto older = make_content(std::byte{0x11});
  const auto newer = make_content(std::byte{0x22});
  const std::vector<std::byte> filler(size_t{1024}, std::byte{0x33});
  auto write_alt = [&](Volume &view, std::span<const std::byte> content) {
    auto wh =
        view.write_alternate_sync(key, AlternateId::Brotli, content.size());
    if (!wh) {
      return false;
    }
    return wh->write_sync(content).has_value() && wh->close_sync().has_value();
  };
  REQUIRE(write_doc(*volume, key, make_content(std::byte{0x01})));

  std::atomic<bool> fired{false};
  std::function<void()> hook = [&] {
    if (fired.exchange(true)) {
      return;
    }
    const uint64_t wraps_before = peer->stats().wrap_count;
    for (int i = 0; i < 20000 && peer->stats().wrap_count == wraps_before;
         ++i) {
      REQUIRE(write_doc(
          *peer, CacheKey("sync-write-wrap-alt-fill-" + std::to_string(i)),
          filler));
    }
    REQUIRE(peer->stats().wrap_count > wraps_before);
    REQUIRE(write_alt(*peer, newer));
  };
  Volume::s_fsync_hook_for_test.store(&hook);
  struct Clear {
    ~Clear() { Volume::s_fsync_hook_for_test.store(nullptr); }
  } clear;

  (void)write_alt(*volume, older);
  Volume::s_fsync_hook_for_test.store(nullptr);

  const CompressionAwareSelector selector;
  AlternateSelectionContext ctx;
  ctx.prefer_compressed = true;
  auto rh = volume->read_alternate_sync(key, selector, ctx);
  REQUIRE(rh.has_value());
  REQUIRE(rh->content().size() == kDocBytes);
  REQUIRE(rh->content()[0] == std::byte{0x22});
}

TEST_CASE(
    "sync_on_write: an alternate removed while another alternate write of "
    "the key sits in its flush stays removed",
    "[volume][alternate][regression]") {
  // An alternate write plans its new head's link (and any splice of the
  // chain) under the stripe mutex.  A removal of another alternate of the
  // key changes only a link inside the chain, not the directory bucket, so
  // the publish cannot see it: if the removal could run between the plan
  // and the publish, the new head would link the removed alternate back in
  // after the removal returned success.  The alternate commit therefore
  // keeps its durability flush under the stripe mutex: the removal waits.
  TempCacheDir tmp("syncalt-remove");
  auto volume = open_volume(tmp.path());
  const CacheKey key("sync-write-alt-remove-key");
  auto write_alt = [&](AlternateId id, std::byte fill) {
    const auto content = make_content(fill);
    auto wh = volume->write_alternate_sync(key, id, content.size());
    return wh && wh->write_sync(content).has_value() &&
           wh->close_sync().has_value();
  };
  // H(A) -> K(B): B first, then A, which links to it.
  REQUIRE(write_alt(AlternateId::Gzip, std::byte{0x0B}));
  REQUIRE(write_alt(AlternateId::Brotli, std::byte{0x0A}));

  std::atomic<std::thread::id> writer_id{};
  std::atomic<bool> fired{false};
  std::atomic<bool> start_remove{false};
  std::atomic<bool> removed{false};
  std::atomic<bool> remove_ok{false};
  std::function<void()> hook = [&] {
    if (std::this_thread::get_id() != writer_id.load() ||
        fired.exchange(true)) {
      return;
    }
    // W sits in its flush: let R remove B, and give it time to finish if
    // nothing stops it.
    start_remove.store(true);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    while (!removed.load() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  };
  std::thread remover([&] {
    while (!start_remove.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    remove_ok.store(
        volume->remove_alternate_sync(key, AlternateId::Gzip).has_value());
    removed.store(true);
  });
  Volume::s_fsync_hook_for_test.store(&hook);
  struct Clear {
    ~Clear() { Volume::s_fsync_hook_for_test.store(nullptr); }
  } clear;
  // W: rewrite A.  Its plan links the new head past the old A, to K.
  writer_id.store(std::this_thread::get_id());
  const bool rewrote = write_alt(AlternateId::Brotli, std::byte{0x1A});
  if (!start_remove.load()) {
    start_remove.store(true);  // the hook never ran: do not hang the thread
  }
  remover.join();
  Volume::s_fsync_hook_for_test.store(nullptr);

  REQUIRE(fired.load());
  REQUIRE(rewrote);
  REQUIRE(remove_ok.load());
  // The removal returned success: B is gone, A serves the rewrite.
  auto listed = volume->list_alternates_sync(key);
  REQUIRE(listed.has_value());
  for (const AlternateInfo &info : *listed) {
    CAPTURE(static_cast<int>(info.id));
    CHECK(info.id != AlternateId::Gzip);
  }
  const DefaultStorageSelector selector;
  AlternateSelectionContext gz;
  const std::array<AlternateId, 1> only_gz{AlternateId::Gzip};
  gz.acceptable_alternates = only_gz;
  CHECK_FALSE(volume->read_alternate_sync(key, selector, gz).has_value());
  AlternateSelectionContext br;
  const std::array<AlternateId, 1> only_br{AlternateId::Brotli};
  br.acceptable_alternates = only_br;
  auto rh = volume->read_alternate_sync(key, selector, br);
  REQUIRE(rh.has_value());
  CHECK(rh->content()[0] == std::byte{0x1A});
}
