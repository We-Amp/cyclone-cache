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
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
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
      for (size_t c = 0; c < MmapDirectory::kMaxChunks; ++c) {
        REQUIRE(borrow_slot::count_of<uint32_t>(raw.dir->chunk_borrow_raw(c)) ==
                0);
      }
    }
    const auto after = make_content(4, 4096);
    REQUIRE(write_entry(*reopened, "after-reopen", after));
    auto r = reopened->read_sync(CacheKey("after-reopen"));
    REQUIRE(r.has_value());
    REQUIRE(content_equals(r->content(), after));
    reopened->stop();
  }
}

// ---------------------------------------------------------------------------
// Shared helpers for the layout / gate cases below.
// ---------------------------------------------------------------------------

namespace {

std::unique_ptr<Cache> open_mp_view_with(const std::string &path,
                                         std::chrono::milliseconds lease,
                                         std::chrono::milliseconds ceiling) {
  CacheConfig config;
  config.set_multi_process(0, 1);
  config.set_ram_cache_size(0);
  config.read_lease_duration = lease;
  config.lease_wrap_ceiling = ceiling;
  auto cache = Cache::create(config);
  REQUIRE(cache.has_value());
  REQUIRE((*cache)->add_volume(path, kVolSize).has_value());
  REQUIRE((*cache)->start().has_value());
  return std::move(*cache);
}

uint32_t chunk_count(const MmapDirectory &dir, size_t chunk) {
  return borrow_slot::count_of<uint32_t>(dir.chunk_borrow_raw(chunk));
}
uint8_t chunk_generation(const MmapDirectory &dir, size_t chunk) {
  return borrow_slot::generation_of<uint32_t>(dir.chunk_borrow_raw(chunk));
}

// Overwrite `bytes` at `offset` of a volume FILE through the fd.  Only while
// no process has the file mapped (see test_reset_gate_spawn.cpp).
void poke_file(const std::string &file, uint64_t offset,
               std::span<const std::byte> bytes) {
  std::fstream f(file, std::ios::in | std::ios::out | std::ios::binary);
  REQUIRE(f.is_open());
  f.seekp(static_cast<std::streamoff>(offset));
  f.write(reinterpret_cast<const char *>(bytes.data()),
          static_cast<std::streamsize>(bytes.size()));
  REQUIRE(f.good());
}

uint16_t peek_u16(const std::string &file, uint64_t offset) {
  std::ifstream f(file, std::ios::in | std::ios::binary);
  REQUIRE(f.is_open());
  f.seekg(static_cast<std::streamoff>(offset));
  uint16_t v = 0;
  f.read(reinterpret_cast<char *>(&v), sizeof(v));
  REQUIRE(f.good());
  return v;
}

// Stripe 0's directory header sits right after the 64-byte VolumeHeader;
// its version field follows the 4-byte magic.
constexpr uint64_t kDir0VersionOffset = VolumeHeader::kSize + 4;

// Open `path` as a fresh multi-process Cache; on failure return the error.
std::pair<CacheError, std::unique_ptr<Cache>> try_open_mp(
    const std::string &path, bool auto_reset) {
  CacheConfig config;
  config.set_multi_process(0, 1);
  config.set_ram_cache_size(0);
  auto cache = Cache::create(config);
  REQUIRE(cache.has_value());
  VolumeConfig vc;
  vc.path = path;
  vc.size = kVolSize;
  vc.auto_reset_on_incompatible = auto_reset;
  if (auto added = (*cache)->add_volume(vc); !added.has_value()) {
    return {added.error(), nullptr};
  }
  if (auto started = (*cache)->start(); !started.has_value()) {
    return {started.error(), nullptr};
  }
  return {CacheError::Success, std::move(*cache)};
}

}  // namespace

// ---------------------------------------------------------------------------
// Test 7 -- one ceiling episode clears the leaked counts of EVERY chunk.
//
// A zero-copy reader killed with -9 never releases its borrows: their counts
// stay in the shared per-chunk slots.  The anti-starvation ceiling is the
// backstop; a forced step resets every chunk's slot (generation + 1), not
// only the chunks it exposes, so one episode clears all of them (S4).
// ---------------------------------------------------------------------------

TEST_CASE(
    "Retention 7: one ceiling episode clears the leaked borrow counts of "
    "every chunk after the holder is killed",
    "[retention][lease][multiprocess]") {
  TempCacheDir tmp("ret7");
  const std::string path = tmp.path();
  // Long lease (the dead holder's stamp must read as live), short ceiling.
  auto view = open_mp_view_with(path, std::chrono::milliseconds(600000),
                                std::chrono::milliseconds(300));
  const std::string file = volume_file_of(*view);

  std::vector<std::string> keys;
  for (int i = 0; i < 4; ++i) {
    keys.push_back("held-" + std::to_string(i));
    REQUIRE(write_entry(*view, keys.back(), make_content(i, 4096)));
  }

  SpawnedPeer holder;
  std::vector<std::string> args = {"borrow", path, std::to_string(kVolSize)};
  args.insert(args.end(), keys.begin(), keys.end());
  REQUIRE(holder.spawn(peer_exe(), args));
  auto ready = holder.wait_ready(kPeerDeadline);
  REQUIRE(ready.has_value());
  REQUIRE(*ready == "READY");

  RawDirectory raw(file);
  uint32_t held_total = 0;
  for (size_t c = 0; c < MmapDirectory::kMaxChunks; ++c) {
    held_total += chunk_count(*raw.dir, c);
  }
  REQUIRE(held_total == keys.size());
  // Counts leaked in other chunks too (a dead reader of documents all over
  // the stripe): the reset must clear them in the same episode.
  REQUIRE(raw.dir->chunk_borrow_acquire(5).counted);
  REQUIRE(raw.dir->chunk_borrow_acquire(MmapDirectory::kMaxChunks - 1).counted);

  holder.kill();  // SIGKILL: the four borrows are never released

  // Flood until the ring wraps AND the step that reuses the held bytes has
  // gone through.  The dead holder's counts defer it until the ceiling
  // passes, then one forced step goes through.
  const auto filler = make_content(9, kPeerContent);
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(30);
  for (size_t i = 0; view->stats().wraps_forced_past_lease == 0 &&
                     std::chrono::steady_clock::now() < deadline;
       ++i) {
    (void)write_entry(*view, "flood-" + std::to_string(i), filler);
  }
  const auto st = view->stats();
  REQUIRE(st.write_buffer_wraps >= 1);
  REQUIRE(st.writes_dropped_by_lease >= 1);
  REQUIRE(st.wraps_forced_past_lease >= 1);

  for (size_t c = 0; c < MmapDirectory::kMaxChunks; ++c) {
    CAPTURE(c);
    REQUIRE(chunk_count(*raw.dir, c) == 0);
  }
  REQUIRE(chunk_generation(*raw.dir, 5) == 1);
  REQUIRE(chunk_generation(*raw.dir, MmapDirectory::kMaxChunks - 1) == 1);
  REQUIRE(view->stats().borrows_outstanding == 0);
  view->stop();
}

// ---------------------------------------------------------------------------
// Test 12 -- a peer's open never writes the shared frontier state (B4).
//
// init_stripes runs in EVERY opener, including while peers are live.  Only
// an exclusive open may touch G, the chunk slots, the lease or the intent;
// a shared open maps them as they are.
// ---------------------------------------------------------------------------

TEST_CASE(
    "Retention 12: a shared open leaves G, the chunk slots, the lease and "
    "the intent exactly as the live peer left them",
    "[retention][multiprocess]") {
  TempCacheDir tmp("ret12");
  const std::string path = tmp.path();
  auto first = open_mp_view(path);
  REQUIRE(write_entry(*first, "a", make_content(1, 4096)));
  const std::string file = volume_file_of(*first);

  RawDirectory raw(file);
  // Plant distinctive live-peer state.
  raw.dir->set_exposure_gen(0x1234);
  REQUIRE(raw.dir->chunk_borrow_acquire(2).counted);
  REQUIRE(raw.dir->chunk_borrow_acquire(40).counted);
  raw.dir->stamp_lease_expiry(0xABCDEF, 0xABCDEF);
  raw.dir->set_wrap_intent(true);
  const uint64_t pos = raw.dir->get_shared_write_pos();
  std::vector<std::byte> before(raw.region.begin(), raw.region.end());

  auto second = open_mp_view(path);  // shared: `first` is live

  REQUIRE(raw.dir->exposure_gen() == 0x1234);
  REQUIRE(chunk_count(*raw.dir, 2) == 1);
  REQUIRE(chunk_count(*raw.dir, 40) == 1);
  REQUIRE(raw.dir->lease_expiry_ns() == 0xABCDEF);
  REQUIRE(raw.dir->wrap_intent());
  REQUIRE(raw.dir->get_shared_write_pos() == pos);
  // Byte-for-byte: the whole directory (header, versions, entries and the
  // retention region) is untouched by the second open.
  REQUIRE(std::memcmp(before.data(), raw.region.data(), before.size()) == 0);

  raw.dir->set_wrap_intent(false);
  second->stop();
  first->stop();
}

// ---------------------------------------------------------------------------
// Test 14 (layout legs) -- a directory of another MmapDirectory version.
//
// kVersion is mixed into the fingerprint, so a v1 peer resolves to a
// different file.  A v1 directory reached anyway (legacy or explicit path)
// goes through the live-peer reset gate like any incompatible format: it is
// refused while a peer holds the file, IncompatibleVersion with auto-reset
// off, and a cold reset with no peer -- and it is NEVER init()ed in place
// under a live peer.
// ---------------------------------------------------------------------------

TEST_CASE(
    "Retention 14: a kVersion 1 peer resolves to a different file, and a "
    "wrong-version directory is never re-initialised under a live peer",
    "[retention][layout][multiprocess]") {
  // The v1 name for the golden geometry (see test_fingerprint_filenames).
  REQUIRE(fingerprint_cache_path("cyclone.dat", size_t{256} << 20, 0,
                                 /*mmap_directory=*/true) !=
          "cyclone-8-93cd38a16f167c85.dat");

  TempCacheDir tmp("ret14v");
  const std::string path = tmp.path();
  std::string file;
  {
    auto view = open_mp_view(path);
    REQUIRE(write_entry(*view, "old", make_content(3, 4096)));
    file = volume_file_of(*view);
    view->stop();
  }
  const uint16_t v1 = 1;
  poke_file(file, kDir0VersionOffset,
            std::as_bytes(std::span<const uint16_t>(&v1, 1)));

  // A live peer holds the file: refuse, and leave the foreign directory
  // exactly as it is.
  {
    SpawnedPeer holder;
    REQUIRE(holder.spawn(peer_exe(), {"hold", file}));
    auto ready = holder.wait_ready(kPeerDeadline);
    REQUIRE(ready.has_value());
    REQUIRE(*ready == "READY");
    auto [err, cache] = try_open_mp(path, true);
    REQUIRE_FALSE(cache);
    REQUIRE(err == CacheError::ResetRefusedLivePeer);
    REQUIRE(peek_u16(file, kDir0VersionOffset) == 1);  // never init()ed
    holder.request_exit();
    REQUIRE(holder.wait_exit(kPeerDeadline).has_value());
  }
  // Auto-reset off: IncompatibleVersion, still untouched.
  {
    auto [err, cache] = try_open_mp(path, false);
    REQUIRE_FALSE(cache);
    REQUIRE(err == CacheError::IncompatibleVersion);
    REQUIRE(peek_u16(file, kDir0VersionOffset) == 1);
  }
  // No peer: cold reset.  The directory is ours again and the old entry is
  // gone with it.
  {
    auto [err, cache] = try_open_mp(path, true);
    REQUIRE(err == CacheError::Success);
    REQUIRE(cache);
    REQUIRE(peek_u16(file, kDir0VersionOffset) == MmapDirectory::kVersion);
    REQUIRE_FALSE(cache->read_sync(CacheKey("old")).has_value());
    cache->stop();
  }
}

// ---------------------------------------------------------------------------
// Test 15 (geometry leg) -- N and Q are a pure function of the data area.
// ---------------------------------------------------------------------------

TEST_CASE(
    "Retention 15: the frontier geometry depends only on the data area, and "
    "N is clamped for small stripes",
    "[retention][layout]") {
  // D2: Q = max(1 MiB, round_up(ceil(A / 64), 8)),
  //     N = clamp(ceil(A / Q), 1, 64).
  for (uint64_t a :
       {uint64_t{4096}, uint64_t{1} << 20, (uint64_t{1} << 20) + 8,
        uint64_t{3469248}, uint64_t{32} << 20, (uint64_t{32} << 20) - 724992,
        uint64_t{128} << 20, uint64_t{1} << 30, uint64_t{1} << 40}) {
    CAPTURE(a);
    const FrontierGeometry g = retention_geometry(a);
    REQUIRE(g.chunks >= 1);
    REQUIRE(g.chunks <= MmapDirectory::kMaxChunks);
    REQUIRE(g.chunk_size >= kMinFrontierChunk);
    REQUIRE(g.chunk_size % 8 == 0);
    REQUIRE(g.chunk_size * g.chunks >= a);       // the chunks cover the area
    REQUIRE(g.chunk_size * (g.chunks - 1) < a);  // and none is empty
  }
  REQUIRE(retention_geometry(uint64_t{1} << 20).chunks == 1);  // flush-like
  REQUIRE(retention_geometry(4096).chunks == 1);
  REQUIRE(retention_geometry(uint64_t{1} << 40).chunks == 64);
}
