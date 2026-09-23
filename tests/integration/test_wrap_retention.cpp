// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Wrap retention (doc/design/wrap-retention.md) and the wrap-window crash
// recovery it depends on.  Test numbers follow section 11.1 of the design so
// a reader can map every case to the claim it pins.

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "core/mmap_directory.hpp"
#include "core/volume.hpp"
#include "cyclone/cache.hpp"
#include "cyclone/config.hpp"
#include "cyclone/key.hpp"
#include "io/mapped_file.hpp"
#include "support/spawned_peer.hpp"
#include "support/temp_cache.hpp"

using namespace cyclone;

namespace {

constexpr std::chrono::milliseconds kPeerDeadline{60000};
constexpr size_t kVolSize = static_cast<size_t>(4) * 1024 * 1024;
// 64 KiB documents: the 4 MiB single-stripe volume holds 52 of them, and the
// tail left after the last one (~60 KiB) still fits a small document, so a
// writer can commit WITHOUT wrapping while a peer is parked in its window.
constexpr size_t kPeerContent = size_t{64} * 1024 - 132;

std::string peer_exe() {
  const std::string exe = CYCLONE_TEST_PEER_EXE;
  REQUIRE(std::filesystem::exists(exe));
  return exe;
}

std::vector<std::byte> make_content(size_t index, size_t size) {
  std::vector<std::byte> content(size);
  for (size_t j = 0; j < size; ++j) {
    content[j] = static_cast<std::byte>((index * 131 + j) & 0xFF);
  }
  return content;
}

bool content_equals(std::span<const std::byte> got,
                    std::span<const std::byte> want) {
  return got.size() == want.size() && !got.empty() &&
         std::memcmp(got.data(), want.data(), got.size()) == 0;
}

bool write_entry(Cache &cache, const std::string &key_str,
                 std::span<const std::byte> content) {
  auto wh = cache.write_sync(CacheKey(key_str), content.size());
  if (!wh.has_value()) {
    return false;
  }
  if (!wh->write_sync(content).has_value()) {
    return false;
  }
  return wh->close_sync().has_value();
}

// Multi-process single-stripe view (4 MiB auto geometry = one stripe) with
// leases off, so nothing in these cases is ever deferred by a borrow.
std::unique_ptr<Cache> open_mp_view(const std::string &path) {
  CacheConfig config;
  config.set_multi_process(0, 1);
  config.set_ram_cache_size(0);
  config.read_lease_duration = std::chrono::milliseconds(0);
  config.lease_wrap_ceiling = std::chrono::milliseconds(0);
  auto cache = Cache::create(config);
  REQUIRE(cache.has_value());
  REQUIRE((*cache)->add_volume(path, kVolSize).has_value());
  REQUIRE((*cache)->start().has_value());
  REQUIRE((*cache)->stats().stripe_count == 1);
  return std::move(*cache);
}

// A second, raw view of stripe 0's shared directory header, for asserting on
// state no public API exposes (the intent flag, the phase, the pass count).
struct RawDirectory {
  std::shared_ptr<MappedFile> file;
  std::span<std::byte> region;
  std::optional<MmapDirectory> dir;

  explicit RawDirectory(const std::string &volume_file) {
    file = MappedFile::create();
    REQUIRE(file);
    REQUIRE(
        file->open(volume_file, MappedFile::OpenMode::ReadWrite).has_value());
    auto mapped = file->map_region(VolumeHeader::kSize,
                                   MmapDirectory::required_size(16 * 1024),
                                   MappedFile::MapMode::ReadWrite);
    REQUIRE(mapped.has_value());
    region = *mapped;
    auto opened = MmapDirectory::open(region);
    REQUIRE(opened.has_value());
    dir = std::move(*opened);
  }
  ~RawDirectory() {
    dir.reset();
    if (file && !region.empty()) {
      file->unmap_region(region);
    }
  }
  RawDirectory(const RawDirectory &) = delete;
  RawDirectory &operator=(const RawDirectory &) = delete;
};

std::string volume_file_of(Cache &cache) {
  auto files = cache.volume_files();
  REQUIRE(files.size() == 1);
  return files[0].file_path;
}

}  // namespace

// ---------------------------------------------------------------------------
// Test 13 -- a writer that dies inside its wrap window.
//
// Before the stuck-intent fix a writer killed between set_wrap_intent(true)
// and the clear left the flag set for good: every read of the stripe failed
// its revalidation and missed until the next wrap, a whole pass later.  The
// flag (and a phase that may have drifted from the pass count) is now
// repaired by the next writer that force-releases the lock from the PROVEN
// dead holder, and by an exclusive open -- never by an escalated takeover,
// whose holder may still be alive and inside the window.
// ---------------------------------------------------------------------------

namespace {

using Seam = Volume::WriterSeam;

constexpr std::array<Seam, 4> kAllSeams = {
    Seam::kAfterIntentSet, Seam::kAfterGatePassed, Seam::kWrapAfterCursor,
    Seam::kAfterEpochStore};

const char *seam_name(Seam s) {
  switch (s) {
    case Seam::kAfterIntentSet:
      return "after-intent-set";
    case Seam::kAfterGatePassed:
      return "after-gate-passed";
    case Seam::kWrapAfterCursor:
      return "wrap-after-cursor";
    case Seam::kAfterEpochStore:
      return "after-epoch-store";
  }
  return "?";
}

void spawn_seam_peer(SpawnedPeer &peer, const std::string &path, Seam seam,
                     bool crash) {
  REQUIRE(peer.spawn(peer_exe(),
                     {"seam", path, std::to_string(kVolSize),
                      std::to_string(static_cast<int>(seam)),
                      crash ? "crash" : "hang", std::to_string(kPeerContent)}));
}

}  // namespace

TEST_CASE(
    "Retention 13: a proven-dead writer's stuck wrap intent is repaired by "
    "the next forced release; a crash at every writer seam",
    "[retention][crash][multiprocess]") {
  for (Seam seam : kAllSeams) {
    CAPTURE(seam_name(seam));
    TempCacheDir tmp("ret13");
    const std::string path = tmp.path();

    // Parent view stays open across the crash, so no later open is exclusive
    // and the ONLY repair available is the forced release.
    auto view = open_mp_view(path);
    const auto k0 = make_content(1, 4096);
    REQUIRE(write_entry(*view, "k0", k0));

    SpawnedPeer peer;
    spawn_seam_peer(peer, path, seam, /*crash=*/true);
    auto code = peer.wait_exit(kPeerDeadline);
    REQUIRE(code.has_value());
    REQUIRE(*code == 42);  // died AT the seam, holding the write lock

    const std::string file = volume_file_of(*view);
    {
      RawDirectory raw(file);
      REQUIRE(raw.dir->wrap_intent());  // the stuck flag the fix repairs
    }
    // While it is stuck, a disk read cannot pass its revalidation.
    REQUIRE_FALSE(view->read_sync(CacheKey("k0")).has_value());

    // The next write finds the lock held by a dead PID: proven-dead forced
    // release, which repairs the wrap state before anything else.
    const auto k1 = make_content(2, 4096);
    REQUIRE(write_entry(*view, "k1", k1));
    {
      RawDirectory raw(file);
      REQUIRE_FALSE(raw.dir->wrap_intent());
      REQUIRE(raw.dir->current_phase() ==
              ((raw.dir->shared_wrap_count() & 1U) != 0));
    }

    // Reads work again.  k1 was written after the repair and must serve;
    // k0 either serves its own bytes (the crash came before anything was
    // published) or misses (the dead writer had already lowered the cursor
    // or bumped the epoch) -- never anything else.
    {
      auto r1 = view->read_sync(CacheKey("k1"));
      REQUIRE(r1.has_value());
      REQUIRE(content_equals(r1->content(), k1));
    }
    {
      auto r0 = view->read_sync(CacheKey("k0"));
      if (r0.has_value()) {
        REQUIRE(content_equals(r0->content(), k0));
      } else {
        REQUIRE(r0.error() == CacheError::NotFound);
      }
    }
    view->stop();
  }
}

TEST_CASE(
    "Retention 13: an escalated takeover of a live stalled writer does NOT "
    "clear its wrap intent; an exclusive open does",
    "[retention][crash][multiprocess]") {
  for (Seam seam : kAllSeams) {
    CAPTURE(seam_name(seam));
    TempCacheDir tmp("ret13e");
    const std::string path = tmp.path();

    auto view = open_mp_view(path);
    const std::string file = volume_file_of(*view);

    SpawnedPeer peer;
    spawn_seam_peer(peer, path, seam, /*crash=*/false);
    auto ready = peer.wait_ready(kPeerDeadline);
    REQUIRE(ready.has_value());
    REQUIRE(*ready == "READY");  // parked INSIDE the window, alive

    // Take the lock over from the live holder via the last-resort
    // escalation, in test time.  A small document fits the tail the peer
    // left, so this write itself neither wraps nor advances.
    MmapDirectory::s_write_lock_max_live_waits_for_test.store(2);
    const auto small = make_content(3, 512);
    const bool wrote = write_entry(*view, "after-escalation", small);
    MmapDirectory::s_write_lock_max_live_waits_for_test.store(0);
    REQUIRE(wrote);
    {
      RawDirectory raw(file);
      // The holder is alive and may still publish: its flag must survive.
      REQUIRE(raw.dir->wrap_intent());
    }

    // Let the peer die without ever leaving the window, then close the last
    // view.  The next open is exclusive (no live peer) and repairs.
    peer.request_exit();
    auto code = peer.wait_exit(kPeerDeadline);
    REQUIRE(code.has_value());
    view->stop();
    view.reset();

    auto reopened = open_mp_view(path);
    {
      RawDirectory raw(file);
      REQUIRE_FALSE(raw.dir->wrap_intent());
      REQUIRE(raw.dir->current_phase() ==
              ((raw.dir->shared_wrap_count() & 1U) != 0));
      REQUIRE(raw.dir->lease_expiry_ns() == 0);
      REQUIRE(borrow_slot::count(raw.dir->borrow_slot_raw()) == 0);
    }
    const auto after = make_content(4, 4096);
    REQUIRE(write_entry(*reopened, "after-reopen", after));
    auto r = reopened->read_sync(CacheKey("after-reopen"));
    REQUIRE(r.has_value());
    REQUIRE(content_equals(r->content(), after));
    reopened->stop();
  }
}
