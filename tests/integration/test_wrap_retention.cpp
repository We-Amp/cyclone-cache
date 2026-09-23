// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Wrap retention (doc/design/wrap-retention.md) and the wrap-window crash
// recovery it depends on.  Test numbers follow section 11.1 of the design so
// a reader can map every case to the claim it pins.

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
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

// ===========================================================================
// Wrap retention: the frontier protocol (design sections 4.3-4.4, 5).
//
// Geometry used below: a 12 MiB volume is one stripe with a data area A of
// ~11.4 MiB, so D2 gives Q = 1 MiB and N = 12.  Every document is exactly
// kDoc = 64 KiB on disk, so pass-k document i sits at S + i * kDoc, sixteen
// to a chunk, and the frontier's moves are fully predictable (FrontierModel
// replays the allocation rules and every case cross-checks it against the
// frontier_advances counter, so a rule change fails loudly instead of
// silently mis-aiming a seam).
// ===========================================================================

namespace {

constexpr size_t kRetVol = size_t{12} << 20;
constexpr uint64_t kDoc = uint64_t{64} * 1024;
constexpr size_t kDocContent = kDoc - 132;  // Document::kHeaderSize

// The (pass, index) -> content map every retention case uses, so a served
// document can always be checked for being exactly what was written.
std::vector<std::byte> doc_content(uint64_t pass, uint64_t index) {
  return make_content(pass * 100003 + index, kDocContent);
}
std::string doc_key(uint64_t pass, uint64_t index) {
  return "p" + std::to_string(pass) + "-" + std::to_string(index);
}

CacheConfig retention_config(bool retention, std::chrono::milliseconds lease,
                             std::chrono::milliseconds ceiling,
                             bool multi_process) {
  CacheConfig config;
  config.set_ram_cache_size(0);
  config.set_enable_checksum(true);
  config.set_wrap_retention(retention);
  config.read_lease_duration = lease;
  config.lease_wrap_ceiling = ceiling;
  if (multi_process) {
    config.set_multi_process(0, 1);
  }
  return config;
}

// A replay of Volume::retention_prepare's frontier rules for fixed-size
// documents written back to back from the start of a pass.
struct FrontierModel {
  uint64_t area = 0;  // A
  uint64_t q = 0;     // Q
  uint64_t n = 0;     // N
  uint64_t f = 0;     // frontier index
  struct Step {
    uint64_t from, to;
    bool mandatory;
  };

  uint64_t frontier_rel(uint64_t fi) const {
    return fi >= n ? area : std::min(area, fi * q);
  }
  uint64_t ceil_index(uint64_t rel) const {
    return std::min(n, (rel + q - 1) / q);
  }
  // Advances made by the write whose end (relative to S) is `need`.
  std::vector<Step> write(uint64_t need) {
    std::vector<Step> steps;
    if (need > frontier_rel(f)) {
      const uint64_t t = ceil_index(need);
      steps.push_back({f, t, true});
      f = t;
    }
    if (f < n && frontier_rel(f) - need < q / 2) {
      const uint64_t t = ceil_index(need + q / 2);
      if (t > f) {
        steps.push_back({f, t, false});
        f = t;
      }
    }
    return steps;
  }
};

struct RetentionVolume {
  TempCacheDir tmp;
  std::unique_ptr<Cache> cache;
  uint64_t area = 0;
  uint64_t per_pass = 0;   // documents per pass
  uint64_t per_chunk = 0;  // documents per chunk
  FrontierGeometry geom;

  RetentionVolume(const char *tag, bool retention,
                  std::chrono::milliseconds lease,
                  std::chrono::milliseconds ceiling, bool multi_process = false)
      : tmp(tag) {
    auto created = Cache::create(
        retention_config(retention, lease, ceiling, multi_process));
    REQUIRE(created.has_value());
    cache = std::move(*created);
    REQUIRE(cache->add_volume(tmp.path(), kRetVol).has_value());
    REQUIRE(cache->start().has_value());
    const auto st = cache->stats();
    REQUIRE(st.stripe_count == 1);
    area = st.stripe_bytes - st.current_bytes;
    geom = retention_geometry(area);
    REQUIRE(geom.chunks == 12);  // the geometry every case below assumes
    REQUIRE(geom.chunk_size == (uint64_t{1} << 20));
    per_pass = area / kDoc;
    per_chunk = geom.chunk_size / kDoc;
  }

  FrontierModel model() const {
    return FrontierModel{area, geom.chunk_size, geom.chunks, 0};
  }
  bool put(uint64_t pass, uint64_t index) {
    return write_entry(*cache, doc_key(pass, index), doc_content(pass, index));
  }
  // Pass 0: exactly fills the data area (the next document wraps).
  void fill_pass0() {
    for (uint64_t i = 0; i < per_pass; ++i) {
      REQUIRE(put(0, i));
    }
    REQUIRE(cache->stats().write_buffer_wraps == 0);
  }
  std::optional<ReadHandle> read(uint64_t pass, uint64_t index) {
    auto r = cache->read_sync(CacheKey(doc_key(pass, index)));
    if (!r.has_value()) {
      return std::nullopt;
    }
    return std::move(*r);
  }
  bool serves(uint64_t pass, uint64_t index) {
    auto r = read(pass, index);
    return r.has_value() &&
           content_equals(r->content(), doc_content(pass, index));
  }
};

// A pause point shared by a paused thread and the test body.
struct Rendezvous {
  std::mutex m;
  std::condition_variable cv;
  bool arrived = false;
  bool released = false;
  void arrive_and_wait() {
    std::unique_lock<std::mutex> lk(m);
    arrived = true;
    cv.notify_all();
    cv.wait(lk, [&] { return released; });
  }
  void wait_arrived() {
    std::unique_lock<std::mutex> lk(m);
    cv.wait(lk, [&] { return arrived; });
  }
  void release() {
    std::unique_lock<std::mutex> lk(m);
    released = true;
    cv.notify_all();
  }
};

// Installs a writer-seam hook that pauses the FIRST time `seam` fires for
// the `occurrence`-th time (1-based) after installation; uninstalls on exit.
struct WriterPause {
  Rendezvous rv;
  std::atomic<int> seen{0};
  WriterPause(Volume::WriterSeam seam, int occurrence) {
    Volume::s_writer_seam_for_test = [this, seam,
                                      occurrence](Volume::WriterSeam at) {
      if (at == seam && seen.fetch_add(1) + 1 == occurrence) {
        rv.arrive_and_wait();
      }
    };
  }
  ~WriterPause() { Volume::s_writer_seam_for_test = nullptr; }
  WriterPause(const WriterPause &) = delete;
  WriterPause &operator=(const WriterPause &) = delete;
};

// Pauses ONE thread (the one that installs it or a named one) at a reader
// seam, once.
struct ReaderPause {
  Rendezvous rv;
  std::atomic<bool> fired{false};
  std::atomic<std::thread::id> only{};
  explicit ReaderPause(Volume::ReaderSeam seam) {
    Volume::s_reader_seam_for_test = [this, seam](Volume::ReaderSeam at) {
      if (at == seam && std::this_thread::get_id() == only.load() &&
          !fired.exchange(true)) {
        rv.arrive_and_wait();
      }
    };
  }
  // Called first thing on the thread that must pause.
  void bind_this_thread() { only.store(std::this_thread::get_id()); }
  ~ReaderPause() { Volume::s_reader_seam_for_test = nullptr; }
  ReaderPause(const ReaderPause &) = delete;
  ReaderPause &operator=(const ReaderPause &) = delete;
};

}  // namespace

// ---------------------------------------------------------------------------
// Test 1 -- the wrap lowers the shared cursor inside its window (B3), and a
// deferred first advance after a retention wrap neither leaves the cursor
// high nor wraps again.  (The flush-mode F6-E mmap cases in
// test_wrap_phase_aba.cpp run in both modes as part of test 18.)
// ---------------------------------------------------------------------------

TEST_CASE(
    "Retention 1: a deferred first advance after the wrap leaves the shared "
    "cursor at S and the wrap count moved exactly once",
    "[retention][lease][multiprocess]") {
  for (const bool retention : {false, true}) {
    CAPTURE(retention);
    RetentionVolume v("ret1", retention, std::chrono::milliseconds(600000),
                      std::chrono::milliseconds(600000),
                      /*multi_process=*/true);
    v.fill_pass0();
    // Hold a borrow on document 0, in chunk 0: the first chunk the forward
    // fill of the next pass needs.
    auto held = v.read(0, 0);
    REQUIRE(held.has_value());

    const std::string file = volume_file_of(*v.cache);
    RawDirectory raw(file);
    const uint64_t data_start = VolumeHeader::kSize + 724992;  // 177 pages
    for (int attempt = 0; attempt < 4; ++attempt) {
      REQUIRE_FALSE(v.put(1, attempt));  // dropped every time
    }
    const auto st = v.cache->stats();
    if (retention) {
      // The wrap is ungated: it happened, exactly once, and published S.
      REQUIRE(raw.dir->shared_wrap_count() == 1);
      REQUIRE(raw.dir->get_shared_write_pos() == data_start);
      REQUIRE(st.advances_deferred_by_lease >= 4);
      REQUIRE(st.wraps_deferred_by_lease == 0);
    } else {
      // Flush: the wrap itself is gated and never happened.
      REQUIRE(raw.dir->shared_wrap_count() == 0);
      REQUIRE(raw.dir->get_shared_write_pos() > data_start);
      REQUIRE(st.wraps_deferred_by_lease >= 4);
    }
    REQUIRE(st.writes_dropped_by_lease >= 4);
    REQUIRE_FALSE(raw.dir->wrap_intent());
    REQUIRE(content_equals(held->content(), doc_content(0, 0)));

    held.reset();  // closing the borrow frees the step
    REQUIRE(v.put(1, 100));
    REQUIRE(raw.dir->shared_wrap_count() == 1);  // still exactly one wrap
    REQUIRE(v.serves(1, 100));
    v.cache->stop();
  }
}

// ---------------------------------------------------------------------------
// Test 2 -- the ungated wrap tears nothing; a borrow is protected until the
// frontier would cross ITS chunk, and only that advance waits for it.
// ---------------------------------------------------------------------------

TEST_CASE(
    "Retention 2: the ungated wrap keeps a current-class borrow kOk until "
    "the frontier reaches the borrow's own chunk",
    "[retention][lease]") {
  RetentionVolume v("ret2", true, std::chrono::milliseconds(600000),
                    std::chrono::milliseconds(600000));
  v.fill_pass0();
  const uint64_t cx = 6;
  const uint64_t jx = cx * v.per_chunk + 2;
  auto hx = v.read(0, jx);  // current class: pass 0, chunk 6
  REQUIRE(hx.has_value());
  REQUIRE(hx->renew_lease_strict() == LeaseRenewal::kOk);

  // The wrap: ungated, tears nothing.
  uint64_t i = 0;
  REQUIRE(v.put(1, i++));
  REQUIRE(v.cache->stats().write_buffer_wraps == 1);
  REQUIRE(hx->renew_lease_strict() == LeaseRenewal::kOk);
  REQUIRE(content_equals(hx->content(), doc_content(0, jx)));

  // Fill forward.  Every advance below chunk 6 proceeds; the borrow stays
  // kOk after each; the first advance that would cross chunk 6 waits.
  bool dropped = false;
  while (i < v.per_pass) {
    if (!v.put(1, i)) {
      dropped = true;
      break;
    }
    ++i;
    REQUIRE(hx->renew_lease_strict() == LeaseRenewal::kOk);
  }
  REQUIRE(dropped);
  REQUIRE(i == cx * v.per_chunk);  // the fill stopped exactly at chunk 6
  const auto st = v.cache->stats();
  REQUIRE(st.advances_deferred_by_lease >= 1);
  REQUIRE(st.wraps_deferred_by_lease == 0);
  REQUIRE(st.wraps_forced_past_lease == 0);
  REQUIRE(hx->renew_lease_strict() == LeaseRenewal::kOk);
  REQUIRE(content_equals(hx->content(), doc_content(0, jx)));

  // The rest of the retained pass is still readable -- chunk 6 onwards.
  REQUIRE(v.serves(0, jx + 1));
  REQUIRE(v.serves(0, v.per_pass - 1));
  REQUIRE(v.cache->stats().retained_hits >= 2);
  // Chunk 5 is gone: exposed and (partly) overwritten.
  REQUIRE_FALSE(v.read(0, 5 * v.per_chunk).has_value());

  hx.reset();  // close the borrow: the advance proceeds
  REQUIRE(v.put(1, i));
  v.cache->stop();
}

// ---------------------------------------------------------------------------
// Tests 3 and 4 -- the advance's Dekker handshake, writer paused inside its
// intent window.  The advance under test is the wrapping write's mandatory
// advance over chunk 0 (the wrap's own intent window comes first, so the
// advance's intent is the SECOND kAfterIntentSet and its gate the FIRST
// kAfterGatePassed of that write).
// ---------------------------------------------------------------------------

TEST_CASE(
    "Retention 3: advance Dekker -- a reader that counted itself before the "
    "gate load defers the advance; one that counts after it backs off",
    "[retention][lease][dekker]") {
  SECTION("reader increments BEFORE the gate load: the advance defers") {
    RetentionVolume v("ret3a", true, std::chrono::milliseconds(600000),
                      std::chrono::milliseconds(600000));
    v.fill_pass0();
    WriterPause wp(Volume::WriterSeam::kAfterIntentSet, 2);
    std::atomic<bool> write_ok{true};
    std::thread writer([&] { write_ok.store(v.put(1, 0)); });
    wp.rv.wait_arrived();  // intent set, gate not yet loaded

    // Reader of document 1 (retained, chunk 0) pauses after it counted
    // itself and stamped the lease, before loading the intent.
    std::optional<ReadHandle> got;
    ReaderPause rp(Volume::ReaderSeam::kAfterBorrow);
    std::thread reader2([&] {
      rp.bind_this_thread();
      got = v.read(0, 1);
    });
    rp.rv.wait_arrived();
    wp.rv.release();  // gate loads: sees the count, defers
    writer.join();
    REQUIRE_FALSE(write_ok.load());
    REQUIRE(v.cache->stats().advances_deferred_by_lease >= 1);
    rp.rv.release();  // reader loads intent (clear) and G (not exposed)
    reader2.join();
    REQUIRE(got.has_value());
    REQUIRE(content_equals(got->content(), doc_content(0, 1)));
    REQUIRE(got->renew_lease_strict() == LeaseRenewal::kOk);
    got.reset();
    v.cache->stop();
  }
  SECTION("reader increments AFTER the gate load: the reader backs off") {
    RetentionVolume v("ret3b", true, std::chrono::milliseconds(600000),
                      std::chrono::milliseconds(600000));
    v.fill_pass0();
    WriterPause wp(Volume::WriterSeam::kAfterGatePassed, 1);
    std::atomic<bool> write_ok{false};
    std::thread writer([&] { write_ok.store(v.put(1, 0)); });
    wp.rv.wait_arrived();  // gate passed (no borrow counted), G not stored
    // The reader counts itself now -- too late for the gate -- then sees
    // the intent and backs off (both attempts): a clean miss.
    auto r = v.read(0, 1);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(v.cache->stats().borrows_outstanding == 0);
    wp.rv.release();
    writer.join();
    REQUIRE(write_ok.load());
    // Chunk 0 is exposed now: position rejects document 1 outright.
    REQUIRE_FALSE(v.read(0, 1).has_value());
    REQUIRE(v.serves(1, 0));
    v.cache->stop();
  }
}

TEST_CASE(
    "Retention 4: writer paused after the G store -- exposed chunks miss, a "
    "borrow elsewhere gets kCopyNow, then kOk once the intent clears",
    "[retention][lease][dekker]") {
  RetentionVolume v("ret4", true, std::chrono::milliseconds(600000),
                    std::chrono::milliseconds(600000));
  v.fill_pass0();
  // The model must agree with the implementation: pass 0 advances through
  // empty space by the same rules.
  uint64_t model_advances = 0;
  {
    FrontierModel pass0 = v.model();
    for (uint64_t i = 0; i < v.per_pass; ++i) {
      model_advances += pass0.write((i + 1) * kDoc).size();
    }
    REQUIRE(v.cache->stats().frontier_advances == model_advances);
  }
  // Find the pass-1 write whose single (early) advance exposes chunk 1.
  FrontierModel model = v.model();
  uint64_t target = 0;
  for (uint64_t i = 0; i < v.per_pass; ++i) {
    auto steps = model.write((i + 1) * kDoc);
    if (steps.size() == 1 && steps[0].from == 1) {
      target = i;
      break;
    }
    model_advances += steps.size();
  }
  REQUIRE(target > 0);
  for (uint64_t i = 0; i < target; ++i) {
    REQUIRE(v.put(1, i));
  }
  REQUIRE(v.cache->stats().frontier_advances == model_advances);
  const uint64_t retained_in_3 = 3 * v.per_chunk + 1;
  auto h3 = v.read(0, retained_in_3);  // retained, chunk 3: not exposed
  REQUIRE(h3.has_value());

  WriterPause wp(Volume::WriterSeam::kAfterEpochStore, 1);
  std::atomic<bool> write_ok{false};
  std::thread writer([&] { write_ok.store(v.put(1, target)); });
  wp.rv.wait_arrived();  // G stored (chunk 1 exposed), intent still set

  // A document in the exposed chunk: rejected by position.
  REQUIRE_FALSE(v.read(0, v.per_chunk + 1).has_value());
  // The borrow in chunk 3: intent set but not exposed -> copy now.
  REQUIRE(h3->renew_lease_strict() == LeaseRenewal::kCopyNow);
  REQUIRE(h3->renew_lease());  // copy-then-verify path: still valid
  // A fresh read of chunk 3 backs off on the intent.
  REQUIRE_FALSE(v.read(0, retained_in_3 + 1).has_value());

  wp.rv.release();
  writer.join();
  REQUIRE(write_ok.load());
  REQUIRE(h3->renew_lease_strict() == LeaseRenewal::kOk);
  REQUIRE(content_equals(h3->content(), doc_content(0, retained_in_3)));
  REQUIRE(v.serves(0, retained_in_3 + 1));
  h3.reset();
  v.cache->stop();
}

// ---------------------------------------------------------------------------
// Test 5 -- a reader paused between the CRC and acquire_borrow while the
// writer advances over its chunk and overwrites its bytes must reject.
// ---------------------------------------------------------------------------

TEST_CASE(
    "Retention 5: a reader paused before its borrow while the writer "
    "advances over its chunk and pwrites rejects, never serves",
    "[retention][lease][dekker]") {
  RetentionVolume v("ret5", true, std::chrono::milliseconds(600000),
                    std::chrono::milliseconds(600000));
  v.fill_pass0();
  REQUIRE(v.put(1, 0));                     // wrap; chunk 0 exposed
  const uint64_t victim = v.per_chunk + 1;  // retained, chunk 1

  std::optional<ReadHandle> got;
  bool done = false;
  ReaderPause rp(Volume::ReaderSeam::kBeforeBorrow);
  std::thread reader([&] {
    rp.bind_this_thread();
    got = v.read(0, victim);
    done = true;
  });
  rp.rv.wait_arrived();  // mapped, key + stamp + CRC verified, no borrow

  // The fill crosses chunk 1 (the gate sees no count: the reader has not
  // registered) and overwrites the victim's bytes in place.
  for (uint64_t i = 1; i <= victim + 1; ++i) {
    REQUIRE(v.put(1, i));
  }
  rp.rv.release();
  reader.join();
  REQUIRE(done);
  REQUIRE_FALSE(got.has_value());  // rejected: exposed, then by position
  REQUIRE(v.cache->stats().borrows_outstanding == 0);
  REQUIRE(v.serves(1, victim));  // the overwriter is intact
  v.cache->stop();
}

// ---------------------------------------------------------------------------
// Test 6 -- per-chunk gating, including a forced advance: kTorn only for
// borrows in the chunks the advance exposed; borrows elsewhere stay kOk.
// ---------------------------------------------------------------------------

TEST_CASE(
    "Retention 6: per-chunk gating -- a forced advance tears only the "
    "borrows in the chunks it exposes",
    "[retention][lease]") {
  RetentionVolume v("ret6", true, std::chrono::milliseconds(600000),
                    std::chrono::milliseconds(300));
  v.fill_pass0();
  const uint64_t c1 = 3;
  const uint64_t c2 = 8;
  auto h1 = v.read(0, c1 * v.per_chunk + 4);
  auto h2 = v.read(0, c2 * v.per_chunk + 4);
  REQUIRE(h1.has_value());
  REQUIRE(h2.has_value());

  // Advances over chunks 0..2 proceed: neither borrow is in them.
  uint64_t i = 0;
  while (v.put(1, i)) {
    ++i;
  }
  REQUIRE(i == c1 * v.per_chunk);  // stopped at the first borrowed chunk
  REQUIRE(v.cache->stats().write_buffer_wraps == 1);
  REQUIRE(h1->renew_lease_strict() == LeaseRenewal::kOk);
  REQUIRE(h2->renew_lease_strict() == LeaseRenewal::kOk);

  // Past the ceiling the next mandatory advance is forced over chunk 3.
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  REQUIRE(v.put(1, i));
  REQUIRE(v.cache->stats().wraps_forced_past_lease == 1);
  REQUIRE(h1->renew_lease_strict() == LeaseRenewal::kTorn);
  REQUIRE_FALSE(h1->renew_lease());
  REQUIRE(h2->renew_lease_strict() == LeaseRenewal::kOk);  // chunk 8: intact
  REQUIRE(content_equals(h2->content(), doc_content(0, c2 * v.per_chunk + 4)));
  // The force reset every chunk's slot (h2's count included: a hold longer
  // than the ceiling is unprotected, as documented).
  REQUIRE(v.cache->stats().borrows_outstanding == 0);
  h1.reset();
  h2.reset();
  v.cache->stop();
}

// ---------------------------------------------------------------------------
// Test 8 -- the early (optional) advance is a pure gate check: no deferral
// clock, no published deadline, no force, however long a borrow sits in
// its way.  Only the mandatory advance starts an episode.
// ---------------------------------------------------------------------------

TEST_CASE(
    "Retention 8: an early advance blocked by a borrow never starts the "
    "deferral clock, publishes no deadline and never forces",
    "[retention][lease]") {
  RetentionVolume v("ret8", true, std::chrono::milliseconds(600000),
                    std::chrono::milliseconds(100));
  v.fill_pass0();
  REQUIRE(v.put(1, 0));                 // wrap; frontier at chunk 1
  auto h = v.read(0, v.per_chunk + 2);  // retained, chunk 1
  REQUIRE(h.has_value());
  REQUIRE(h->ns_until_forced_wrap() == UINT64_MAX);

  // Writes 1..15 fit below chunk 1: only EARLY advances want chunk 1.
  // Sleep past the ceiling half way: still no force, no deadline.
  for (uint64_t i = 1; i < v.per_chunk; ++i) {
    if (i == v.per_chunk / 2) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    REQUIRE(v.put(1, i));
    REQUIRE(h->ns_until_forced_wrap() == UINT64_MAX);
  }
  auto st = v.cache->stats();
  REQUIRE(st.early_advances_skipped >= 1);
  REQUIRE(st.writes_dropped_by_lease == 0);
  REQUIRE(st.wraps_forced_past_lease == 0);
  REQUIRE(st.borrows_outstanding == 1);  // no reset
  REQUIRE(h->renew_lease_strict() == LeaseRenewal::kOk);

  // The next write needs chunk 1: the MANDATORY advance defers and starts
  // the episode -- only now is a deadline published.
  REQUIRE_FALSE(v.put(1, v.per_chunk));
  REQUIRE(h->ns_until_forced_wrap() != UINT64_MAX);
  st = v.cache->stats();
  REQUIRE(st.advances_deferred_by_lease == 1);
  REQUIRE(st.wraps_forced_past_lease == 0);
  h.reset();
  v.cache->stop();
}

// ---------------------------------------------------------------------------
// Test 14 (mode legs) -- a volume created in one eviction mode refuses to
// be opened in the other beside a live peer, reports IncompatibleVersion
// with auto-reset off, and is cold-reset otherwise.
// ---------------------------------------------------------------------------

TEST_CASE(
    "Retention 14: an eviction-mode mismatch is refused with a live peer, "
    "IncompatibleVersion without auto-reset, and a cold reset otherwise",
    "[retention][layout][multiprocess]") {
  for (const bool created_retaining : {true, false}) {
    CAPTURE(created_retaining);
    TempCacheDir tmp("ret14m");
    const std::string path = tmp.path();
    std::string file;
    {
      auto c = Cache::create(
          retention_config(created_retaining, std::chrono::milliseconds(5000),
                           std::chrono::milliseconds(60000), true));
      REQUIRE(c.has_value());
      REQUIRE((*c)->add_volume(path, kRetVol).has_value());
      REQUIRE((*c)->start().has_value());
      REQUIRE(write_entry(**c, "m", make_content(5, 4096)));
      file = volume_file_of(**c);
      (*c)->stop();
    }
    const uint64_t kRetainChunksOffset = 40;  // VolumeHeader::retain_chunks
    const uint16_t persisted = peek_u16(file, kRetainChunksOffset);
    REQUIRE((persisted != 0) == created_retaining);

    auto open_other = [&](bool auto_reset) {
      auto c = Cache::create(
          retention_config(!created_retaining, std::chrono::milliseconds(5000),
                           std::chrono::milliseconds(60000), true));
      REQUIRE(c.has_value());
      VolumeConfig vc;
      vc.path = path;
      vc.size = kRetVol;
      vc.auto_reset_on_incompatible = auto_reset;
      if (auto added = (*c)->add_volume(vc); !added.has_value()) {
        return std::make_pair(added.error(), std::unique_ptr<Cache>());
      }
      if (auto started = (*c)->start(); !started.has_value()) {
        return std::make_pair(started.error(), std::unique_ptr<Cache>());
      }
      return std::make_pair(CacheError::Success, std::move(*c));
    };

    {
      SpawnedPeer holder;
      REQUIRE(holder.spawn(peer_exe(), {"hold", file}));
      auto ready = holder.wait_ready(kPeerDeadline);
      REQUIRE(ready.has_value());
      REQUIRE(*ready == "READY");
      auto [err, c] = open_other(true);
      REQUIRE_FALSE(c);
      REQUIRE(err == CacheError::ResetRefusedLivePeer);
      REQUIRE(peek_u16(file, kRetainChunksOffset) == persisted);
      holder.request_exit();
      REQUIRE(holder.wait_exit(kPeerDeadline).has_value());
    }
    {
      auto [err, c] = open_other(false);
      REQUIRE_FALSE(c);
      REQUIRE(err == CacheError::IncompatibleVersion);
      REQUIRE(peek_u16(file, kRetainChunksOffset) == persisted);
    }
    {
      auto [err, c] = open_other(true);
      REQUIRE(err == CacheError::Success);
      REQUIRE(c);
      REQUIRE((peek_u16(file, kRetainChunksOffset) != 0) == !created_retaining);
      REQUIRE_FALSE(c->read_sync(CacheKey("m")).has_value());  // cold
      c->stop();
    }
  }
}

// ---------------------------------------------------------------------------
// Test 15 (process leg) -- N and Q do not depend on per-process settings:
// two live views with different max_object_size share one retaining ring
// (a disagreement would make the second open a mode mismatch and refuse).
// A stripe of 1 MiB or less gets N = 1 and behaves like flush.
// ---------------------------------------------------------------------------

TEST_CASE(
    "Retention 15: views with different max_object_size share one retaining "
    "ring; N = 1 behaves like flush",
    "[retention][layout][multiprocess]") {
  {
    TempCacheDir tmp("ret15");
    auto open_view = [&](size_t max_object) {
      auto config = retention_config(true, std::chrono::milliseconds(5000),
                                     std::chrono::milliseconds(60000), true);
      config.max_object_size = max_object;
      auto c = Cache::create(config);
      REQUIRE(c.has_value());
      REQUIRE((*c)->add_volume(tmp.path(), kRetVol).has_value());
      REQUIRE((*c)->start().has_value());
      return std::move(*c);
    };
    auto a = open_view(size_t{64} << 20);
    auto b = open_view(size_t{1} << 20);  // live peer: a mismatch refuses
    REQUIRE(write_entry(*a, "shared", make_content(8, 4096)));
    {
      auto r = b->read_sync(CacheKey("shared"));
      REQUIRE(r.has_value());
      REQUIRE(content_equals(r->content(), make_content(8, 4096)));
    }
    b->stop();
    a->stop();
  }
  {
    // ~1.6 MiB single-process volume: A < 1 MiB, so N = 1.  After the wrap
    // the first write's mandatory advance exposes the whole previous pass,
    // exactly like a flush.
    TempCacheDir tmp("ret15n1");
    auto c =
        Cache::create(retention_config(true, std::chrono::milliseconds(0),
                                       std::chrono::milliseconds(0), false));
    REQUIRE(c.has_value());
    REQUIRE((*c)->add_volume(tmp.path(), size_t{1600} * 1024).has_value());
    REQUIRE((*c)->start().has_value());
    const auto st = (*c)->stats();
    const uint64_t area = st.stripe_bytes - st.current_bytes;
    REQUIRE(retention_geometry(area).chunks == 1);
    const auto small = make_content(4, 16 * 1024 - 132);
    uint64_t i = 0;
    while ((*c)->stats().write_buffer_wraps == 0) {
      REQUIRE(write_entry(**c, "n1-" + std::to_string(i++), small));
    }
    // Every document of the pass that just ended is gone.
    for (uint64_t j = 0; j + 1 < i; ++j) {
      REQUIRE_FALSE(
          (*c)->read_sync(CacheKey("n1-" + std::to_string(j))).has_value());
    }
    REQUIRE((*c)->stats().retained_hits == 0);
    (*c)->stop();
  }
}

// ---------------------------------------------------------------------------
// Test 16 -- lost timeline.  Directory, header and data persist
// independently across a power loss (sync_on_write is off).  Rewind the
// directory to an earlier state over newer data, then run past the next
// wrap: whatever is admitted is a genuine, CRC-valid document of its key --
// possibly an older version (accepted, section 4.12) -- never a wrong-key or
// torn serve.
// ---------------------------------------------------------------------------

TEST_CASE(
    "Retention 16: after a lost-timeline power loss every admitted document "
    "is a genuine, CRC-valid version of its key",
    "[retention][powerloss][multiprocess]") {
  TempCacheDir tmp("ret16");
  const std::string path = tmp.path();
  constexpr uint64_t kKeys = 64;
  // content(key, version): the served bytes identify both.
  auto content_of = [](uint64_t key, uint64_t version) {
    return make_content(key * 1009 + version, kDocContent);
  };
  auto key_of = [](uint64_t key) { return "lt-" + std::to_string(key); };
  auto open = [&]() {
    auto c =
        Cache::create(retention_config(true, std::chrono::milliseconds(0),
                                       std::chrono::milliseconds(0), true));
    REQUIRE(c.has_value());
    REQUIRE((*c)->add_volume(path, kRetVol).has_value());
    REQUIRE((*c)->start().has_value());
    return std::move(*c);
  };
  const uint64_t dir_region = MmapDirectory::required_size(16 * 1024);
  std::vector<char> saved_dir(dir_region);
  std::string file;
  uint64_t max_version = 0;
  {
    auto c = open();
    file = volume_file_of(*c);
    for (uint64_t k = 0; k < kKeys; ++k) {
      REQUIRE(write_entry(*c, key_of(k), content_of(k, 0)));
    }
    c->stop();
    // The "persisted" directory: taken now, restored after more writes.
    std::ifstream in(file, std::ios::binary);
    in.seekg(VolumeHeader::kSize);
    in.read(saved_dir.data(), static_cast<std::streamsize>(dir_region));
    REQUIRE(in.good());
  }
  {
    auto c = open();
    // Rewrite every key several times: past a wrap, into the next pass.
    for (uint64_t v = 1; c->stats().write_buffer_wraps < 1 || v < 8; ++v) {
      for (uint64_t k = 0; k < kKeys; ++k) {
        (void)write_entry(*c, key_of(k), content_of(k, v));
      }
      max_version = v;
    }
    c->stop();
  }
  // Power loss with reordered persistence: the old directory over new data.
  poke_file(file, VolumeHeader::kSize,
            std::as_bytes(std::span<const char>(saved_dir)));

  auto check_all = [&](Cache &c) {
    uint64_t served = 0;
    for (uint64_t k = 0; k < kKeys; ++k) {
      auto r = c.read_sync(CacheKey(key_of(k)));
      if (!r.has_value()) {
        REQUIRE((r.error() == CacheError::NotFound ||
                 r.error() == CacheError::Corrupted));
        continue;
      }
      bool genuine = false;
      for (uint64_t v = 0; v <= max_version + 16 && !genuine; ++v) {
        genuine = content_equals(r->content(), content_of(k, v));
      }
      CAPTURE(k);
      REQUIRE(genuine);
      ++served;
    }
    return served;
  };
  {
    auto c = open();  // exclusive open over the rewound directory
    (void)check_all(*c);
    // Run past the next wrap, rewriting only half the keys: the other half
    // can only be served from whatever the lost timeline left behind.
    for (uint64_t v = max_version + 1; c->stats().write_buffer_wraps < 2; ++v) {
      for (uint64_t k = 0; k < kKeys / 2; ++k) {
        (void)write_entry(*c, key_of(k), content_of(k, v));
      }
      max_version = v;
    }
    const uint64_t served = check_all(*c);
    REQUIRE(served >= kKeys / 2);  // non-vacuous: the rewritten half serves
    c->stop();
  }
}

// ---------------------------------------------------------------------------
// Test 17 -- the snapshot loads G, THEN W.  A wrap or an advance landing
// between the two loads can only make admission stricter (section 5.4).
// ---------------------------------------------------------------------------

TEST_CASE(
    "Retention 17: a snapshot straddling a wrap or an advance never admits "
    "a wrong or torn document",
    "[retention][dekker]") {
  SECTION("a wrap between the G and W loads") {
    RetentionVolume v("ret17w", true, std::chrono::milliseconds(0),
                      std::chrono::milliseconds(0));
    v.fill_pass0();
    // Rewrite document 0's key in the NEXT pass: it lands back at S, the
    // same offset the stale pass-0 entry points at.  A reader whose
    // snapshot says "pass 0" but whose cursor is post-wrap sees that entry
    // as current (o < W) -- only the pass stamp can reject it.
    std::optional<ReadHandle> got;
    ReaderPause rp(Volume::ReaderSeam::kSnapshotGen);
    std::thread reader([&] {
      rp.bind_this_thread();
      got = v.read(0, 0);
    });
    rp.rv.wait_arrived();  // G = pass 0 loaded; W not yet
    const uint64_t before = v.cache->stats().stamp_rejections;
    REQUIRE(write_entry(*v.cache, doc_key(0, 0), doc_content(1, 0)));
    REQUIRE(v.cache->stats().write_buffer_wraps == 1);
    rp.rv.release();
    reader.join();
    if (got.has_value()) {
      // Only ever the genuine current version (never pass-0 bytes torn).
      REQUIRE(content_equals(got->content(), doc_content(1, 0)));
    } else {
      REQUIRE(v.cache->stats().stamp_rejections > before);
    }
    got.reset();
    v.cache->stop();
  }
  SECTION("an advance between the G and W loads") {
    RetentionVolume v("ret17a", true, std::chrono::milliseconds(0),
                      std::chrono::milliseconds(0));
    v.fill_pass0();
    REQUIRE(v.put(1, 0));                     // wrap; frontier at chunk 1
    const uint64_t victim = v.per_chunk + 3;  // retained, chunk 1
    std::optional<ReadHandle> got;
    ReaderPause rp(Volume::ReaderSeam::kSnapshotGen);
    std::thread reader([&] {
      rp.bind_this_thread();
      got = v.read(0, victim);
    });
    rp.rv.wait_arrived();
    // Advance over chunk 1 and overwrite the victim before W is loaded.
    for (uint64_t i = 1; i <= victim + 1; ++i) {
      REQUIRE(v.put(1, i));
    }
    rp.rv.release();
    reader.join();
    REQUIRE_FALSE(got.has_value());  // stale F_snap, fresh W: rejected
    REQUIRE(v.serves(1, victim));
    v.cache->stop();
  }
}
