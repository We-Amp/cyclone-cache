// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Wrap retention (doc/design/wrap-retention.md) and the wrap-window crash
// recovery it depends on.  Test numbers follow section 11.1 of the design so
// a reader can map every case to the claim it pins.

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
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
#include <set>
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
    auto mapped = file->map_region(
        VolumeHeader::kSize, MmapDirectory::required_size(size_t{16} * 1024),
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
                     bool crash, bool wrap_only = false) {
  std::vector<std::string> args = {"seam",
                                   path,
                                   std::to_string(kVolSize),
                                   std::to_string(static_cast<int>(seam)),
                                   crash ? "crash" : "hang",
                                   std::to_string(kPeerContent)};
  if (wrap_only) {
    args.emplace_back("wrap");
  }
  REQUIRE(peer.spawn(peer_exe(), args));
}

}  // namespace

TEST_CASE("FastDivU64 matches hardware division for every divisor in use",
          "[retention][fastdiv]") {
  // N + 1 in [2, 65], Q from 8-byte multiples up to stripe-sized data
  // areas, plus the edges of the reciprocal (powers of two, 2^64 - 1).
  std::vector<uint64_t> divisors;
  for (uint64_t d = 1; d <= 65; ++d) {
    divisors.push_back(d);
  }
  for (uint64_t d : {uint64_t{1} << 20, (uint64_t{1} << 20) + 8,
                     uint64_t{16} << 20, uint64_t{33554424}, uint64_t{1} << 40,
                     (uint64_t{1} << 63) + 1, ~uint64_t{0}}) {
    divisors.push_back(d);
  }
  uint64_t x = 0x9E3779B97F4A7C15ULL;
  for (const uint64_t d : divisors) {
    const cyclone::FastDivU64 div(d);
    CAPTURE(d);
    for (uint64_t n :
         {uint64_t{0}, uint64_t{1}, d - 1, d, d + 1, (2 * d) - 1, 2 * d,
          ~uint64_t{0}, ~uint64_t{0} - 1, ~uint64_t{0} - d}) {
      CAPTURE(n);
      REQUIRE(div.quot(n) == n / d);
    }
    for (int i = 0; i < 20000; ++i) {
      x ^= x << 13U;
      x ^= x >> 7U;
      x ^= x << 17U;
      const uint64_t n = (i % 2 == 0) ? x : x >> (x & 63U);
      CAPTURE(n);
      REQUIRE(div.quot(n) == n / d);
#if defined(__SIZEOF_INT128__)
      // The 32-bit-target fallback agrees with the 128-bit product.
      __extension__ using U128 = unsigned __int128;
      REQUIRE(cyclone::FastDivU64::mulhi_portable(n, d) ==
              static_cast<uint64_t>((static_cast<U128>(n) * d) >> 64U));
#endif
    }
  }
}

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
    // The escalating writer below must not open an intent window of its own
    // (its clear would hide whether the takeover repaired).  With wrap
    // retention, once the wrap has stored the new G the very next write
    // needs a frontier advance -- which is an intent window -- so the two
    // seams after that point cannot be observed this way.  The repair
    // decision (repair_after_forced_release) does not depend on the seam,
    // and the other two seams cover it.
    if (CacheConfig{}.wrap_retention &&
        (seam == Seam::kAfterGatePassed || seam == Seam::kAfterEpochStore)) {
      continue;
    }
    TempCacheDir tmp("ret13e");
    const std::string path = tmp.path();

    auto view = open_mp_view(path);
    const std::string file = volume_file_of(*view);

    SpawnedPeer peer;
    // Park inside the WRAPPING write's window (not a pass-0 advance's).
    spawn_seam_peer(peer, path, seam, /*crash=*/false, /*wrap_only=*/true);
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

  [[nodiscard]] uint64_t frontier_rel(uint64_t fi) const {
    return fi >= n ? area : std::min(area, fi * q);
  }
  [[nodiscard]] uint64_t ceil_index(uint64_t rel) const {
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

  [[nodiscard]] FrontierModel model() const {
    return FrontierModel{area, geom.chunk_size, geom.chunks, 0};
  }
  [[nodiscard]] bool put(uint64_t pass, uint64_t index) const {
    return write_entry(*cache, doc_key(pass, index), doc_content(pass, index));
  }
  // Pass 0: exactly fills the data area (the next document wraps).
  void fill_pass0() const {
    for (uint64_t i = 0; i < per_pass; ++i) {
      REQUIRE(put(0, i));
    }
    REQUIRE(cache->stats().write_buffer_wraps == 0);
  }
  [[nodiscard]] std::optional<ReadHandle> read(uint64_t pass,
                                               uint64_t index) const {
    auto r = cache->read_sync(CacheKey(doc_key(pass, index)));
    if (!r.has_value()) {
      return std::nullopt;
    }
    return std::move(*r);
  }
  [[nodiscard]] bool serves(uint64_t pass, uint64_t index) const {
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
  std::atomic<std::thread::id> only;
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
// Test 6 -- per-chunk gating, including a forced advance.  The force
// exposes only the chunk it crosses (h2's bytes stay intact), but it resets
// EVERY chunk's borrow slot, so every borrow it uncounted -- h2 included --
// learns kTorn at once (review R2): nothing would defer the later advance
// over h2's chunk any more.
// ---------------------------------------------------------------------------

TEST_CASE(
    "Retention 6: per-chunk gating -- a forced advance exposes only its "
    "chunk, and every borrow it uncounted learns kTorn",
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
  // Chunk 8 is not exposed: h2's bytes are intact and G alone would still
  // say kOk.  But the force reset every chunk's slot (h2's count included),
  // so h2 is no longer protected and must stop aliasing now.
  REQUIRE(content_equals(h2->content(), doc_content(0, c2 * v.per_chunk + 4)));
  REQUIRE(h2->renew_lease_strict() == LeaseRenewal::kTorn);
  REQUIRE_FALSE(h2->renew_lease());
  REQUIRE(v.cache->stats().borrows_outstanding == 0);
  // A fresh read of the same document is a new, counted borrow: kOk.
  auto h3 = v.read(0, c2 * v.per_chunk + 4);
  REQUIRE(h3.has_value());
  REQUIRE(h3->renew_lease_strict() == LeaseRenewal::kOk);
  h3.reset();
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
  const uint64_t dir_region = MmapDirectory::required_size(size_t{16} * 1024);
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
    // Pass-0 snapshot, post-wrap cursor: the entry now carries the new
    // phase (the rewrite took over its slot in place), so the stale snapshot
    // cannot classify it at all, and a surviving stale copy would carry
    // pass 0's phase with a pass-1 stamp at o < W -- rejected by the stamp.
    // Either way: a miss or the genuine new version, never pass-0 bytes.
    if (got.has_value()) {
      REQUIRE(content_equals(got->content(), doc_content(1, 0)));
    } else {
      REQUIRE(v.cache->stats().stamp_rejections >= before);
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

// ===========================================================================
// Wrap retention: uniqueness, purge and chains (design sections 4.5-4.7).
// ===========================================================================

namespace {

class IdSelector : public StorageAlternateSelector {
 public:
  explicit IdSelector(AlternateId want) : _want(want) {}
  [[nodiscard]] std::optional<size_t> select(
      std::span<const AlternateInfo> alternates,
      const AlternateSelectionContext & /*ctx*/) const override {
    for (size_t i = 0; i < alternates.size(); ++i) {
      if (alternates[i].id == _want) {
        return i;
      }
    }
    return std::nullopt;
  }

 private:
  AlternateId _want;
};

bool put_alternate(Cache &cache, const std::string &key, AlternateId id,
                   std::span<const std::byte> content) {
  auto wh = cache.write_alternate_sync(CacheKey(key), id, content.size());
  if (!wh.has_value()) {
    return false;
  }
  if (!wh->write_sync(content).has_value()) {
    return false;
  }
  return wh->close_sync().has_value();
}

std::optional<ReadHandle> read_alternate(Cache &cache, const std::string &key,
                                         AlternateId id) {
  IdSelector want(id);
  AlternateSelectionContext ctx;
  auto r = cache.read_alternate_sync(CacheKey(key), want, ctx);
  if (!r.has_value()) {
    return std::nullopt;
  }
  return std::move(*r);
}

size_t alternate_count(Cache &cache, const std::string &key) {
  auto alts = cache.list_alternates_sync(CacheKey(key));
  return alts.has_value() ? alts->size() : 0;
}

uint64_t read_u64(const std::string &file, uint64_t offset) {
  std::ifstream f(file, std::ios::in | std::ios::binary);
  REQUIRE(f.is_open());
  f.seekg(static_cast<std::streamoff>(offset));
  uint64_t v = 0;
  f.read(reinterpret_cast<char *>(&v), sizeof(v));
  REQUIRE(f.good());
  return v;
}

// Pass 0 with chosen indices written as alternates of `key` (same on-disk
// size as every other document, so the layout is unchanged).
void fill_pass0_with(
    RetentionVolume &v, const std::string &key,
    std::initializer_list<std::pair<uint64_t, AlternateId>> alternates) {
  for (uint64_t i = 0; i < v.per_pass; ++i) {
    bool written = false;
    for (const auto &[index, id] : alternates) {
      if (index == i) {
        REQUIRE(put_alternate(*v.cache, key, id, doc_content(0, i)));
        written = true;
      }
    }
    if (!written) {
      REQUIRE(v.put(0, i));
    }
  }
  REQUIRE(v.cache->stats().write_buffer_wraps == 0);
}

}  // namespace

// ---------------------------------------------------------------------------
// Test 9 -- one resolvable entry per key (B2): rewrites of retained keys,
// same-offset fixed-size rewrites, and alternate writes over retained and
// wrap-raced heads.  No duplicate survives; no self-loop forms.
// ---------------------------------------------------------------------------

TEST_CASE(
    "Retention 9: a rewrite of a retained key updates its entry in place and "
    "leaves exactly one resolving entry",
    "[retention][uniqueness]") {
  RetentionVolume v("ret9a", true, std::chrono::milliseconds(0),
                    std::chrono::milliseconds(0));
  v.fill_pass0();
  REQUIRE(v.put(1, 0));                    // wrap
  const uint64_t k = 6 * v.per_chunk + 5;  // retained, chunk 6
  REQUIRE(v.serves(0, k));
  const uint64_t entries_before = v.cache->stats().current_entries;
  const auto v2 = doc_content(7, k);
  REQUIRE(write_entry(*v.cache, doc_key(0, k), v2));
  // Rule 1: the retained entry was ELECTED and updated in place.
  REQUIRE(v.cache->stats().current_entries == entries_before);
  {
    auto r = v.read(0, k);
    REQUIRE(r.has_value());
    REQUIRE(content_equals(r->content(), v2));
  }
  // Removing the key removes every entry of it: nothing resurfaces, not
  // even after the frontier sweeps the old copy's chunk and the ring wraps.
  REQUIRE(v.cache->remove_sync(CacheKey(doc_key(0, k))).has_value());
  REQUIRE_FALSE(v.read(0, k).has_value());
  for (uint64_t i = 1; i < 2 * v.per_pass; ++i) {
    REQUIRE(v.put(2, i));
    if (i % 32 == 0) {
      REQUIRE_FALSE(v.read(0, k).has_value());
    }
  }
  REQUIRE(v.cache->stats().write_buffer_wraps >= 2);
  REQUIRE_FALSE(v.read(0, k).has_value());
  v.cache->stop();
}

TEST_CASE(
    "Retention 9: a fixed-size rewrite that lands on its own retained offset "
    "resolves to the new document only",
    "[retention][uniqueness]") {
  RetentionVolume v("ret9b", true, std::chrono::milliseconds(0),
                    std::chrono::milliseconds(0));
  v.fill_pass0();
  const uint64_t j = 3 * v.per_chunk + 7;
  // Pass 1: documents 0 .. j-1, then the key of pass-0 document j, which
  // lands at exactly j * kDoc -- its own retained copy's offset.
  for (uint64_t i = 0; i < j; ++i) {
    REQUIRE(v.put(1, i));
  }
  const auto v2 = doc_content(8, j);
  REQUIRE(write_entry(*v.cache, doc_key(0, j), v2));
  {
    auto r = v.read(0, j);
    REQUIRE(r.has_value());
    REQUIRE(content_equals(r->content(), v2));
  }
  REQUIRE(v.cache->remove_sync(CacheKey(doc_key(0, j))).has_value());
  REQUIRE_FALSE(v.read(0, j).has_value());
  // Two more passes: the stale entry (if any survived in the bucket) never
  // resolves the key again.
  for (uint64_t i = 0; i < 2 * v.per_pass; ++i) {
    REQUIRE(v.put(3, i));
  }
  REQUIRE_FALSE(v.read(0, j).has_value());
  v.cache->stop();
}

TEST_CASE(
    "Retention 9: an alternate write over a retained or wrap-raced head "
    "carries the chain forward, updates the head in place and forms no "
    "self-loop",
    "[retention][uniqueness][alternate]") {
  SECTION("retained head, the carried copy lands on the old head's offset") {
    RetentionVolume v("ret9c", true, std::chrono::milliseconds(600000),
                      std::chrono::milliseconds(300));
    const uint64_t c = 5;
    const uint64_t h = c * v.per_chunk;  // head at exactly the chunk start
    const std::string key = "self-loop-K";
    fill_pass0_with(v, key, {{h, AlternateId::Original}});
    // A borrow elsewhere in chunk c pins the frontier at the chunk start, so
    // the retained head stays admissible while the cursor walks up to it.
    auto pin = v.read(0, h + 1);
    REQUIRE(pin.has_value());
    for (uint64_t i = 0; i < h; ++i) {
      REQUIRE(v.put(1, i));
    }
    uint64_t old_head_abs = 0;
    {
      auto alts = v.cache->list_alternates_sync(CacheKey(key));
      REQUIRE(alts.has_value());
      REQUIRE(alts->size() == 1);
      old_head_abs = (*alts)[0].disk_offset;
    }
    // The cursor sits exactly on the head's offset.  The first attempt's
    // mandatory advance defers (and starts the episode); past the ceiling
    // the second is forced.  A DIFFERENT id, so the retained Original is
    // carried: the slot is [Original copy][Brotli], and the copy lands
    // exactly ON the retained Original's offset.
    const auto newer = doc_content(9, h);
    const auto refusals0 = v.cache->stats().alternate_wrap_refusals;
    REQUIRE_FALSE(put_alternate(*v.cache, key, AlternateId::Brotli, newer));
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    REQUIRE(put_alternate(*v.cache, key, AlternateId::Brotli, newer));
    REQUIRE(v.cache->stats().wraps_forced_past_lease == 1);
    // Nothing was refused (D5 holds without it); one node was carried.
    const auto st = v.cache->stats();
    REQUIRE(st.alternate_wrap_refusals == refusals0);
    REQUIRE(st.alternates_carried_forward == 1);
    REQUIRE(st.alternate_carry_bytes == kDoc);
    REQUIRE(st.alternates_carry_dropped == 0);

    auto alts = v.cache->list_alternates_sync(CacheKey(key));
    REQUIRE(alts.has_value());
    REQUIRE(alts->size() == 2);
    REQUIRE((*alts)[0].id == AlternateId::Brotli);
    REQUIRE((*alts)[1].id == AlternateId::Original);
    REQUIRE((*alts)[1].disk_offset == old_head_abs);
    REQUIRE((*alts)[0].disk_offset == old_head_abs + kDoc);
    // No self-loop: the head links one document DOWN, to the copy, and the
    // copy ends the chain.
    const std::string file = volume_file_of(*v.cache);
    const uint64_t head_link = read_u64(
        file, (*alts)[0].disk_offset + Document::kNextAlternateOffsetPos);
    const uint64_t copy_link = read_u64(
        file, (*alts)[1].disk_offset + Document::kNextAlternateOffsetPos);
    REQUIRE(head_link != 0);
    REQUIRE(copy_link == 0);
    // head_link is stripe-relative; the head's own relative offset is kDoc
    // above it.
    REQUIRE((*alts)[0].disk_offset - (*alts)[1].disk_offset == kDoc);
    auto r = read_alternate(*v.cache, key, AlternateId::Brotli);
    REQUIRE(r.has_value());
    REQUIRE(content_equals(r->content(), newer));
    r.reset();
    // The retained Original now resolves from its current-pass copy.
    auto o = read_alternate(*v.cache, key, AlternateId::Original);
    REQUIRE(o.has_value());
    REQUIRE(content_equals(o->content(), doc_content(0, h)));
    o.reset();
    pin.reset();
    v.cache->stop();
  }
  SECTION("wrap-raced head: carried after the wrap, updated in place") {
    RetentionVolume v("ret9d", true, std::chrono::milliseconds(0),
                      std::chrono::milliseconds(0));
    const std::string key = "raced-K";
    // Pass 0 ends with the key's Brotli + Original chain in its last chunk;
    // the next alternate write wraps.
    fill_pass0_with(v, key,
                    {{v.per_pass - 2, AlternateId::Brotli},
                     {v.per_pass - 1, AlternateId::Original}});
    REQUIRE(alternate_count(*v.cache, key) == 2);
    const uint64_t entries_before = v.cache->stats().current_entries;
    const auto newer = doc_content(10, 1);
    REQUIRE(put_alternate(*v.cache, key, AlternateId::Original, newer));
    REQUIRE(v.cache->stats().write_buffer_wraps == 1);
    // The allocation wrapped while the write planned a link to the live
    // Brotli.  The wrap made that chain retained, so the write gave its slot
    // back and retried: the retry carried Brotli (the superseded Original
    // is not carried) instead of refusing the link.
    const auto st = v.cache->stats();
    REQUIRE(st.alternate_wrap_refusals == 0);
    REQUIRE(st.alternates_carried_forward == 1);
    // One entry, updated in place (B2a): no stale head beside it.
    REQUIRE(st.current_entries == entries_before);
    REQUIRE(alternate_count(*v.cache, key) == 2);
    {
      auto b = read_alternate(*v.cache, key, AlternateId::Brotli);
      REQUIRE(b.has_value());
      REQUIRE(content_equals(b->content(), doc_content(0, v.per_pass - 2)));
    }
    // Still the same chain after the frontier sweeps the old head's chunk:
    // the carried copy is current, so the sweep cannot touch it.
    for (uint64_t i = 1; i + 4 < v.per_pass; ++i) {
      REQUIRE(v.put(1, i));
    }
    REQUIRE(alternate_count(*v.cache, key) == 2);
    auto r = read_alternate(*v.cache, key, AlternateId::Original);
    REQUIRE(r.has_value());
    REQUIRE(content_equals(r->content(), newer));
    r.reset();
    auto b = read_alternate(*v.cache, key, AlternateId::Brotli);
    REQUIRE(b.has_value());
    REQUIRE(content_equals(b->content(), doc_content(0, v.per_pass - 2)));
    b.reset();
    v.cache->stop();
  }
}

// ---------------------------------------------------------------------------
// Test 10 -- purging a retained key removes every entry of it; removing an
// alternate from a retained chain removes the whole entry (S7).
// ---------------------------------------------------------------------------

TEST_CASE(
    "Retention 10: purging a retained key, and removing an alternate from a "
    "retained chain, removes the whole entry",
    "[retention][uniqueness][alternate]") {
  RetentionVolume v("ret10", true, std::chrono::milliseconds(0),
                    std::chrono::milliseconds(0));
  const std::string chain_key = "retained-chain";
  const uint64_t b = 10 * v.per_chunk;
  fill_pass0_with(v, chain_key,
                  {{b, AlternateId::Brotli}, {b + 1, AlternateId::Original}});
  REQUIRE(v.put(1, 0));  // wrap: everything above is retained now

  // Plain key in chunk 9.
  const uint64_t k = 9 * v.per_chunk + 2;
  REQUIRE(v.serves(0, k));
  const uint64_t entries = v.cache->stats().current_entries;
  REQUIRE(v.cache->remove_sync(CacheKey(doc_key(0, k))).has_value());
  REQUIRE(v.cache->stats().current_entries == entries - 1);
  REQUIRE_FALSE(v.read(0, k).has_value());
  REQUIRE_FALSE(v.cache->remove_sync(CacheKey(doc_key(0, k))).has_value());

  // The retained chain walks (retained -> retained, downward) ...
  REQUIRE(alternate_count(*v.cache, chain_key) == 2);
  REQUIRE(read_alternate(*v.cache, chain_key, AlternateId::Brotli));
  // ... and removing ONE alternate removes the whole entry.
  REQUIRE(
      v.cache->remove_alternate_sync(CacheKey(chain_key), AlternateId::Brotli)
          .has_value());
  REQUIRE(alternate_count(*v.cache, chain_key) == 0);
  REQUIRE_FALSE(read_alternate(*v.cache, chain_key, AlternateId::Original));
  REQUIRE_FALSE(read_alternate(*v.cache, chain_key, AlternateId::Brotli));
  v.cache->stop();
}

// ---------------------------------------------------------------------------
// Test 11 -- retained chain hops: retained -> retained downward works and
// the borrow sits on the SERVED node's chunk; targets below F and upward
// hops are rejected.
// ---------------------------------------------------------------------------

TEST_CASE(
    "Retention 11: retained chains walk downward, borrow on the served "
    "node's chunk, and reject targets below the frontier and upward hops",
    "[retention][alternate]") {
  RetentionVolume v("ret11", true, std::chrono::milliseconds(600000),
                    std::chrono::milliseconds(600000));
  const std::string key = "hop-K";
  const uint64_t tail = 2 * v.per_chunk + 3;  // Brotli, chunk 2
  const uint64_t head = 7 * v.per_chunk + 3;  // Original (head), chunk 7
  fill_pass0_with(v, key,
                  {{tail, AlternateId::Brotli}, {head, AlternateId::Original}});
  REQUIRE(v.put(1, 0));  // wrap

  // retained -> retained, downward: served.
  auto held = read_alternate(*v.cache, key, AlternateId::Brotli);
  REQUIRE(held.has_value());
  REQUIRE(content_equals(held->content(), doc_content(0, tail)));
  REQUIRE(v.cache->stats().retained_hits >= 1);
  // The borrow counts in the served node's chunk (2), not the head's (7):
  // the fill stops exactly at chunk 2.
  uint64_t i = 1;
  while (v.put(1, i)) {
    ++i;
  }
  REQUIRE(i == 2 * v.per_chunk);
  held.reset();

  // Past chunk 2 (but short of the head's chunk 7): the hop's target is
  // below F -- rejected; the head alone remains.
  for (; i < 5 * v.per_chunk; ++i) {
    REQUIRE(v.put(1, i));
  }
  REQUIRE_FALSE(read_alternate(*v.cache, key, AlternateId::Brotli));
  REQUIRE(alternate_count(*v.cache, key) == 1);
  REQUIRE(read_alternate(*v.cache, key, AlternateId::Original));

  // An UPWARD link out of the retained head is never followed: point it at
  // a higher retained document and the walk ends at the head.
  auto alts = v.cache->list_alternates_sync(CacheKey(key));
  REQUIRE(alts.has_value());
  const uint64_t head_abs = (*alts)[0].disk_offset;
  // Links are stripe-relative: S (relative) is stripe_bytes - A on this
  // single-stripe volume, and the head is pass-0 document `head`.
  const uint64_t head_rel =
      (v.cache->stats().stripe_bytes - v.area) + head * kDoc;
  const uint64_t upward_rel = head_rel + 3 * kDoc;
  const std::string file = volume_file_of(*v.cache);
  poke_file(file, head_abs + Document::kNextAlternateOffsetPos,
            std::as_bytes(std::span<const uint64_t>(&upward_rel, 1)));
  REQUIRE(alternate_count(*v.cache, key) == 1);
  REQUIRE(read_alternate(*v.cache, key, AlternateId::Original));
  v.cache->stop();
}

// ---------------------------------------------------------------------------
// Section 4.7 -- the insert victim order.
// ---------------------------------------------------------------------------

namespace {
class TableAdmission final : public InsertAdmission {
 public:
  std::array<AdmitClass, 8> by_offset{};  // indexed by offset / 1000
  void refresh() override {}
  [[nodiscard]] AdmitClass classify(uint64_t rel, bool) const override {
    return by_offset[rel / 1000];
  }
};
DirEntry entry(uint64_t offset, uint16_t tag) {
  DirEntry e;
  e.set_offset(offset);
  e.set_tag(tag);
  e.set_approx_size(4096);
  return e;
}
}  // namespace

TEST_CASE(
    "Retention 4.7: the insert victim order is verified, empty, "
    "inadmissible, collider, then the oldest admissible",
    "[retention][directory]") {
  TableAdmission adm;
  adm.by_offset = {AdmitClass::kReject,   AdmitClass::kCurrent,
                   AdmitClass::kCurrent,  AdmitClass::kRetained,
                   AdmitClass::kRetained, AdmitClass::kReject,
                   AdmitClass::kCurrent,  AdmitClass::kCurrent};
  constexpr uint16_t kTag = 7;
  constexpr uint64_t kAny = Directory::kMatchAnyTag;
  bool collided = false;
  bool full = false;

  // 1. The verified entry wins over everything, whatever its class.
  std::array<DirEntry, 4> b = {entry(1000, 1), DirEntry{}, entry(3000, kTag),
                               entry(4000, kTag)};
  auto c =
      choose_insert_slot(b.data(), 4, kTag, 4000, kAny, adm, &collided, &full);
  REQUIRE(c.slot == 3);
  REQUIRE(c.verified);
  // 2. Then an empty slot.
  c = choose_insert_slot(b.data(), 4, kTag, Directory::kNoVerifiedEntry, kAny,
                         adm, &collided, &full);
  REQUIRE(c.slot == 1);
  REQUIRE_FALSE(c.replaces);
  // 3. Then an inadmissible one (offset 5000 is rejected by the table).
  b = {entry(1000, 1), entry(5000, 2), entry(3000, kTag), entry(6000, 3)};
  c = choose_insert_slot(b.data(), 4, kTag, Directory::kNoVerifiedEntry, kAny,
                         adm, &collided, &full);
  REQUIRE(c.slot == 1);
  REQUIRE(c.replaces);
  REQUIRE_FALSE(collided);
  // 4. Then a live tag collider (counted).
  b = {entry(1000, 1), entry(2000, 2), entry(3000, kTag), entry(6000, 3)};
  c = choose_insert_slot(b.data(), 4, kTag, Directory::kNoVerifiedEntry, kAny,
                         adm, &collided, &full);
  REQUIRE(c.slot == 2);
  REQUIRE(collided);
  // 5. Then the oldest admissible: the retained one nearest F first ...
  collided = false;
  b = {entry(1000, 1), entry(4000, 2), entry(3000, 4), entry(6000, 3)};
  c = choose_insert_slot(b.data(), 4, kTag, Directory::kNoVerifiedEntry, kAny,
                         adm, &collided, &full);
  REQUIRE(c.slot == 2);  // retained at 3000, below the one at 4000
  REQUIRE(full);
  // ... else the lowest-offset current one.
  full = false;
  b = {entry(7000, 1), entry(2000, 2), entry(6000, 4), entry(1000, 3)};
  c = choose_insert_slot(b.data(), 4, kTag, Directory::kNoVerifiedEntry, kAny,
                         adm, &collided, &full);
  REQUIRE(c.slot == 3);
  REQUIRE(full);
}

// ---------------------------------------------------------------------------
// Test 18 (TSan hammer variants of tests 3-6) -- the protocol under real
// concurrency instead of seams: one writer drives wraps and frontier
// advances as fast as it can while readers borrow documents all over the
// stripe (current, retained, and the chunks right at the frontier), hold
// some of them across many strict renews, and check every byte they are
// ever handed.  Run under ThreadSanitizer this is the data-race check of the
// advance's Dekker handshake (3, 4), the reader's own-chunk exposure check
// (5) and the per-chunk gate with forced advances (6).
//
// The invariant checked on every borrow: while the borrow has only ever been
// told kOk (or kCopyNow), its bytes are exactly what was written for its key.
// ---------------------------------------------------------------------------

namespace {

struct HammerResult {
  uint64_t served = 0;
  uint64_t held_checks = 0;
  uint64_t torn_seen = 0;
  uint64_t forced_tears = 0;  // documented: holds past a forced step
  bool mismatch = false;
};

// `writer` and `reader` may be the same Cache (single process) or two views
// of one file (cross-view).
HammerResult hammer(Cache &writer, Cache &reader, std::chrono::milliseconds run,
                    unsigned readers) {
  std::atomic<uint64_t> latest{0};
  std::atomic<bool> stop{false};
  std::atomic<bool> mismatch{false};
  std::atomic<uint64_t> served{0};
  std::atomic<uint64_t> held_checks{0};
  std::atomic<uint64_t> torn_seen{0};
  std::atomic<uint64_t> forced_tears{0};

  std::thread w([&] {
    for (uint64_t n = 1; !stop.load(); ++n) {
      (void)write_entry(writer, "hammer-" + std::to_string(n),
                        doc_content(7, n));
      latest.store(n);
    }
  });
  std::vector<std::thread> rs;
  rs.reserve(readers);
  for (unsigned r = 0; r < readers; ++r) {
    rs.emplace_back([&, r] {
      uint64_t rng = 0x9E3779B97F4A7C15ULL * (r + 1);
      auto next = [&rng] {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        return rng;
      };
      while (!stop.load()) {
        const uint64_t top = latest.load();
        if (top < 2) {
          std::this_thread::yield();
          continue;
        }
        // Mostly the last ~pass and a half: current, retained, and the
        // chunks the frontier is about to expose.
        const uint64_t back = next() % 300;
        const uint64_t n = top > back ? top - back : 1;
        // A CEILING-FORCED step (sampled before the read) is the one
        // documented way a borrow loses its protection early: it resets
        // every chunk's count, and a borrow taken late in a deferral
        // episode may have almost no headroom left.  A mismatch that
        // coincides with one is that contract, counted, not a bug.
        const uint64_t forced0 = writer.stats().wraps_forced_past_lease;
        auto classify_mismatch = [&] {
          if (writer.stats().wraps_forced_past_lease == forced0) {
            mismatch.store(true);
          } else {
            forced_tears.fetch_add(1);
          }
        };
        auto rh = reader.read_sync(CacheKey("hammer-" + std::to_string(n)));
        if (!rh.has_value()) {
          continue;
        }
        served.fetch_add(1);
        const auto want = doc_content(7, n);
        if (!content_equals(rh->content(), want)) {
          classify_mismatch();
          continue;
        }
        // Hold every fourth borrow across a burst of strict renews; one in
        // sixty-four for ~150 ms, past the ceiling, so forced advances (and
        // forced wraps) tear some of them -- which must be reported, never
        // served silently.
        //
        // The one documented exception: a CEILING-FORCED step resets every
        // chunk's count, so a borrow it did not expose becomes unprotected
        // while its renews still say kOk until G passes it ("holds longer
        // than the ceiling are unprotected").  A mismatch that coincides
        // with a forced step is that contract, counted, not a bug.
        if (next() % 4 == 0) {
          const bool long_hold = next() % 16 == 0;
          for (int k = 0; k < (long_hold ? 150 : 16) && !stop.load(); ++k) {
            if (long_hold) {
              std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            const LeaseRenewal lr = rh->renew_lease_strict();
            if (lr == LeaseRenewal::kTorn) {
              torn_seen.fetch_add(1);
              break;  // exposed: the bytes may be refilled now
            }
            if (!content_equals(rh->content(), want)) {
              classify_mismatch();
              break;
            }
            held_checks.fetch_add(1);
            std::this_thread::yield();
          }
        }
      }
    });
  }
  // At least `run`, and until the ring has wrapped twice (sanitizer builds
  // write far slower), capped at 60 s.
  const auto start = std::chrono::steady_clock::now();
  while (
      std::chrono::steady_clock::now() - start < run ||
      (writer.stats().write_buffer_wraps < 2 &&
       std::chrono::steady_clock::now() - start < std::chrono::seconds(60))) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  stop.store(true);
  w.join();
  for (auto &t : rs) {
    t.join();
  }
  return {served.load(), held_checks.load(), torn_seen.load(),
          forced_tears.load(), mismatch.load()};
}

}  // namespace

TEST_CASE(
    "Retention 18: hammer -- concurrent advances, wraps and borrows never "
    "hand out torn bytes (single process)",
    "[retention][hammer][concurrent]") {
  const bool retention = GENERATE(false, true);
  CAPTURE(retention);
  // A lease long enough never to lapse mid-check even under a sanitizer (a
  // lapsed lease is unprotected by contract), and a SHORT ceiling: held
  // borrows gate advances and wraps, and the long holds get forced past,
  // all within the run.
  RetentionVolume v("ret18", retention, std::chrono::milliseconds(2000),
                    std::chrono::milliseconds(60));
  const auto result =
      hammer(*v.cache, *v.cache, std::chrono::milliseconds(1500), 4);
  CAPTURE(result.served, result.held_checks, result.torn_seen,
          result.forced_tears);
  REQUIRE_FALSE(result.mismatch);
  REQUIRE(result.served > 0);
  const auto st = v.cache->stats();
  REQUIRE(st.write_buffer_wraps >= 1);
  if (retention) {
    REQUIRE(st.frontier_advances > 0);
  }
  v.cache->stop();
}

TEST_CASE(
    "Retention 18: hammer -- a reader view never sees torn bytes while a "
    "writer view wraps and advances (cross-view, shared slots)",
    "[retention][hammer][concurrent][multiprocess]") {
  const bool retention = GENERATE(false, true);
  CAPTURE(retention);
  TempCacheDir tmp("ret18mp");
  auto open_view = [&](uint32_t index) {
    auto config = retention_config(retention, std::chrono::milliseconds(2000),
                                   std::chrono::milliseconds(60), false);
    config.set_multi_process(index, 2);
    auto c = Cache::create(config);
    REQUIRE(c.has_value());
    REQUIRE((*c)->add_volume(tmp.path(), kRetVol).has_value());
    REQUIRE((*c)->start().has_value());
    return std::move(*c);
  };
  auto writer = open_view(0);  // owns the single stripe
  auto reader = open_view(1);
  const auto result =
      hammer(*writer, *reader, std::chrono::milliseconds(1500), 4);
  CAPTURE(result.served, result.held_checks, result.torn_seen,
          result.forced_tears);
  REQUIRE_FALSE(result.mismatch);
  REQUIRE(result.served > 0);
  reader->stop();
  writer->stop();
}

// ---------------------------------------------------------------------------
// Independent-review reproductions (R1-R3, and two guards that passed).
// Each case reads the mode from the (env-overridable) config default, so a
// run of the suite in each mode covers both; mode-specific cases SKIP in the
// other mode.
// ---------------------------------------------------------------------------

namespace {

std::vector<std::byte> rv_fill(size_t tag, size_t n) {
  std::vector<std::byte> v(n);
  for (size_t j = 0; j < n; ++j)
    v[j] = static_cast<std::byte>((tag * 131 + j * 7) & 0xFF);
  return v;
}
bool rv_eq(std::span<const std::byte> a, std::span<const std::byte> b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0;
}
bool rv_put(Cache &c, const std::string &k, std::span<const std::byte> v) {
  auto wh = c.write_sync(CacheKey(k), v.size());
  if (!wh) return false;
  if (!wh->write_sync(v)) return false;
  return wh->close_sync().has_value();
}
bool rv_put_alt(Cache &c, const std::string &k, AlternateId id,
                std::span<const std::byte> v) {
  auto wh = c.write_alternate_sync(CacheKey(k), id, v.size());
  if (!wh) return false;
  if (!wh->write_sync(v)) return false;
  return wh->close_sync().has_value();
}
std::set<int> rv_ids(Cache &c, const std::string &k) {
  std::set<int> s;
  auto l = c.list_alternates_sync(CacheKey(k));
  if (!l) return s;
  for (auto &a : *l) s.insert(static_cast<int>(a.id));
  return s;
}
bool rv_serves_alt(Cache &c, const std::string &k, AlternateId id,
                   std::span<const std::byte> want) {
  DefaultStorageSelector sel;
  std::array<AlternateId, 1> acc{id};
  AlternateSelectionContext ctx;
  ctx.acceptable_alternates = acc;
  auto r = c.read_alternate_sync(CacheKey(k), sel, ctx);
  return r.has_value() && rv_eq(r->content(), want);
}

constexpr size_t kRvVol = size_t{16} * 1024 * 1024;  // one stripe

std::unique_ptr<Cache> rv_open(const std::string &path, bool mp,
                               std::chrono::milliseconds lease,
                               std::chrono::milliseconds ceiling) {
  CacheConfig cfg;
  if (mp) cfg.set_multi_process(0, 1);
  cfg.set_ram_cache_size(0);
  cfg.read_lease_duration = lease;
  cfg.lease_wrap_ceiling = ceiling;
  auto c = Cache::create(cfg);
  REQUIRE(c.has_value());
  REQUIRE((*c)->add_volume(path, kRvVol).has_value());
  REQUIRE((*c)->start().has_value());
  return std::move(*c);
}

}  // namespace

// PageSpeed shape: Original first, then optimized alternates written later
// by the optimization engine, re-recorded, one removed, interleaved with
// unrelated traffic.  Must survive everything short of the frontier
// reaching it.  Run in both modes (CYCLONE_TEST_WRAP_RETENTION).
TEST_CASE(
    "Retention review: a PageSpeed-style alternate chain across churn and "
    "two wraps, carried forward by alternate writes",
    "[retention][review][alternate]") {
  for (bool mp : {false, true}) {
    CAPTURE(mp);
    TempCacheDir tmp(mp ? "rv-alt-mp" : "rv-alt");
    auto c = rv_open(tmp.path(), mp, std::chrono::milliseconds(2000),
                     std::chrono::milliseconds(200));
    const bool retain = CacheConfig{}.wrap_retention;
    REQUIRE(c->stats().stripe_count == 1);
    const std::string K = "http://example.com/app.js";
    auto orig = rv_fill(1, 30000);
    auto gz = rv_fill(2, 9000);
    auto br = rv_fill(3, 8000);
    auto webp = rv_fill(4, 7000);
    // Fillers first so K lands mid-stripe (about 6 MB into the 16 MiB
    // stripe): far enough in that the next pass can place its own copy of
    // the chain (carried at about 3.6 MB, below) above the chunks the
    // FOLLOWING wrap exposes first.
    size_t fi = 0;
    auto filler = rv_fill(99, 60000);
    for (int i = 0; i < 100; ++i)
      REQUIRE(rv_put(*c, "f" + std::to_string(fi++), filler));
    REQUIRE(rv_put_alt(*c, K, AlternateId::Original, orig));
    for (int i = 0; i < 5; ++i)
      REQUIRE(rv_put(*c, "f" + std::to_string(fi++), filler));
    REQUIRE(rv_put_alt(*c, K, AlternateId::Gzip, gz));
    REQUIRE(rv_put_alt(*c, K, AlternateId::Brotli, br));
    for (int i = 0; i < 5; ++i)
      REQUIRE(rv_put(*c, "f" + std::to_string(fi++), filler));
    REQUIRE(rv_put_alt(*c, K, AlternateId::WebP, webp));
    // Re-record Gzip twice (supersede mid-chain), Original once (tail node).
    auto gz2 = rv_fill(5, 9100);
    REQUIRE(rv_put_alt(*c, K, AlternateId::Gzip, gz2));
    auto gz3 = rv_fill(6, 9200);
    REQUIRE(rv_put_alt(*c, K, AlternateId::Gzip, gz3));
    auto orig2 = rv_fill(7, 30100);
    REQUIRE(rv_put_alt(*c, K, AlternateId::Original, orig2));
    REQUIRE(rv_ids(*c, K) == std::set<int>{0, 1, 3, 16});
    REQUIRE(rv_serves_alt(*c, K, AlternateId::Original, orig2));
    REQUIRE(rv_serves_alt(*c, K, AlternateId::Gzip, gz3));
    REQUIRE(rv_serves_alt(*c, K, AlternateId::Brotli, br));
    REQUIRE(rv_serves_alt(*c, K, AlternateId::WebP, webp));
    REQUIRE(
        c->remove_alternate_sync(CacheKey(K), AlternateId::Brotli).has_value());
    REQUIRE(rv_ids(*c, K) == std::set<int>{0, 3, 16});
    REQUIRE(rv_serves_alt(*c, K, AlternateId::Original, orig2));
    const auto chain_depth = c->stats().alternate_max_chain_depth;
    CAPTURE(chain_depth);

    // Churn until exactly one wrap has happened and a bit more.
    const uint64_t w0 = c->stats().write_buffer_wraps;
    while (c->stats().write_buffer_wraps == w0) {
      REQUIRE(rv_put(*c, "f" + std::to_string(fi++), filler));
    }
    for (int i = 0; i < 60; ++i)
      (void)rv_put(*c, "g" + std::to_string(i), filler);
    if (retain) {
      // Retained: the whole chain must still resolve past its head.
      CHECK(rv_ids(*c, K) == std::set<int>{0, 3, 16});
      CHECK(rv_serves_alt(*c, K, AlternateId::Original, orig2));
      CHECK(rv_serves_alt(*c, K, AlternateId::Gzip, gz3));
      CHECK(rv_serves_alt(*c, K, AlternateId::WebP, webp));
      // Optimization engine records a NEW alternate onto the retained key.
      // D5 forbids linking the retained chain, so the write CARRIES it: the
      // Original, Gzip and WebP are rewritten into the current pass beside
      // the AVIF head (review R4, resolved), and every one of them keeps
      // resolving with its exact bytes.
      const auto st0 = c->stats();
      auto avif = rv_fill(8, 5000);
      REQUIRE(rv_put_alt(*c, K, AlternateId::AVIF, avif));
      const auto st1 = c->stats();
      CHECK(rv_ids(*c, K) == std::set<int>{0, 3, 16, 17});
      CHECK(rv_serves_alt(*c, K, AlternateId::AVIF, avif));
      CHECK(rv_serves_alt(*c, K, AlternateId::Original, orig2));
      CHECK(rv_serves_alt(*c, K, AlternateId::Gzip, gz3));
      CHECK(rv_serves_alt(*c, K, AlternateId::WebP, webp));
      CHECK(st1.alternate_wrap_refusals == st0.alternate_wrap_refusals);
      // Write amplification of this write: exactly the three carried
      // documents (header + content, 8-byte padded):
      //   Original 30100 + 132 -> 30232, Gzip 9200 + 132 -> 9336,
      //   WebP 7000 + 132 -> 7136: 46704 bytes beside a 5136-byte AVIF.
      auto padded = [](size_t content) {
        return (uint64_t{content} + 132 + 7) & ~uint64_t{7};
      };
      const uint64_t carried_bytes =
          padded(orig2.size()) + padded(gz3.size()) + padded(webp.size());
      CHECK(carried_bytes == 46704);
      CHECK(st1.alternates_carried_forward - st0.alternates_carried_forward ==
            3);
      CHECK(st1.alternate_carry_bytes - st0.alternate_carry_bytes ==
            carried_bytes);
      CHECK(st1.alternates_carry_dropped == st0.alternates_carry_dropped);
      // Once per key per pass: the chain is CURRENT now, so the next
      // optimized alternate links normally and copies nothing.
      auto br2 = rv_fill(9, 6000);
      REQUIRE(rv_put_alt(*c, K, AlternateId::Brotli, br2));
      const auto st2 = c->stats();
      CHECK(st2.alternate_carry_bytes == st1.alternate_carry_bytes);
      CHECK(rv_ids(*c, K) == std::set<int>{0, 1, 3, 16, 17});

      // And across the NEXT wrap too: the whole chain is retained again,
      // and one more alternate write carries all five.
      const uint64_t w1 = c->stats().write_buffer_wraps;
      while (c->stats().write_buffer_wraps == w1) {
        REQUIRE(rv_put(*c, "f" + std::to_string(fi++), filler));
      }
      CHECK(rv_ids(*c, K) == std::set<int>{0, 1, 3, 16, 17});
      auto jxl = rv_fill(10, 4000);
      REQUIRE(rv_put_alt(*c, K, AlternateId::JpegXL, jxl));
      const auto st3 = c->stats();
      CHECK(st3.alternates_carried_forward - st2.alternates_carried_forward ==
            5);
      CHECK(rv_ids(*c, K) == std::set<int>{0, 1, 3, 16, 17, 18});
      CHECK(rv_serves_alt(*c, K, AlternateId::Original, orig2));
      CHECK(rv_serves_alt(*c, K, AlternateId::Brotli, br2));
      CHECK(rv_serves_alt(*c, K, AlternateId::Gzip, gz3));
      CHECK(rv_serves_alt(*c, K, AlternateId::WebP, webp));
      CHECK(rv_serves_alt(*c, K, AlternateId::AVIF, avif));
      CHECK(rv_serves_alt(*c, K, AlternateId::JpegXL, jxl));
      CHECK(st3.alternate_wrap_refusals == st0.alternate_wrap_refusals);
    } else {
      CHECK(rv_ids(*c, K).empty());  // flush: the whole pass is gone
    }
    c->stop();
  }
}

// Retention: a borrow of a RETAINED document in the last chunk, held while
// pass 1 fills (and advances, mandatory + early) through every lower chunk.
// Strict renew must stay kOk and the bytes intact; the advance into its own
// chunk must defer (not force) while the lease is live; closing unblocks.
TEST_CASE(
    "Retention review: a retained borrow in the last chunk survives every "
    "lower advance with kOk",
    "[retention][review][lease]") {
  if (!CacheConfig{}.wrap_retention) {
    SKIP("retention mode only");
  }
  for (bool mp : {false, true}) {
    CAPTURE(mp);
    TempCacheDir tmp(mp ? "rv-bor-mp" : "rv-bor");
    auto c = rv_open(tmp.path(), mp, std::chrono::milliseconds(600000),
                     std::chrono::milliseconds(600000));
    REQUIRE(c->stats().stripe_count == 1);
    constexpr size_t kContent = size_t{64} * 1024 - 132;
    // Pass 0: fill until the next write would wrap.
    std::vector<std::string> keys;
    size_t i = 0;
    for (;;) {
      const auto st = c->stats();
      if (st.current_bytes + (kContent + 132 + 7) / 8 * 8 > st.stripe_bytes)
        break;
      keys.push_back("p0-" + std::to_string(i));
      REQUIRE(rv_put(*c, keys.back(), rv_fill(i, kContent)));
      ++i;
    }
    REQUIRE(c->stats().write_buffer_wraps == 0);
    // Last document of pass 0: in the last chunk.
    const size_t held_idx = keys.size() - 1;
    auto h = c->read_sync(CacheKey(keys[held_idx]));
    REQUIRE(h.has_value());
    REQUIRE(h->renew_lease_strict() == LeaseRenewal::kOk);
    // A second borrow in the second-to-last chunk region, released early.
    const auto adv0 = c->stats().frontier_advances;
    size_t j = 0;
    size_t renews = 0;
    bool dropped = false;
    for (; j < keys.size() * 2; ++j) {
      if (!rv_put(*c, "p1-" + std::to_string(j),
                  rv_fill(10000 + j, kContent))) {
        dropped = true;
        break;
      }
      REQUIRE(h->renew_lease_strict() == LeaseRenewal::kOk);
      REQUIRE(h->renew_lease());
      REQUIRE(rv_eq(h->content(), rv_fill(held_idx, kContent)));
      ++renews;
    }
    const auto st = c->stats();
    CAPTURE(j, renews, st.frontier_advances - adv0, st.early_advances_skipped,
            st.advances_deferred_by_lease, st.wraps_forced_past_lease);
    REQUIRE(dropped);
    REQUIRE(st.write_buffer_wraps == 1);
    REQUIRE(st.frontier_advances - adv0 >= 2);
    REQUIRE(st.wraps_forced_past_lease == 0);
    REQUIRE(st.advances_deferred_by_lease >= 1);
    // Still readable, served from the retained pass.
    auto again = c->read_sync(CacheKey(keys[held_idx]));
    REQUIRE(again.has_value());
    REQUIRE(rv_eq(again->content(), rv_fill(held_idx, kContent)));
    again = make_unexpected(CacheError::NotFound);
    h = make_unexpected(CacheError::NotFound);  // close
    REQUIRE(rv_put(*c, "after-close", rv_fill(1, kContent)));
    c->stop();
  }
}

// Retention, multi-process: a writer that dies at kWrapAfterCursor (shared W
// already lowered to S, G not yet bumped).  The next writer force-releases
// the lock and repairs the intent -- and then writes at S in the SAME pass,
// over current-class documents.  A live, lease-protected borrow of the
// document at S must see a torn verdict (or keep intact bytes); it must
// never read kOk over overwritten bytes.
TEST_CASE(
    "Retention review R1: a writer crash at kWrapAfterCursor, then a "
    "forced-release repair, never tears a live borrow silently",
    "[retention][review][crash][multiprocess]") {
  if (!CacheConfig{}.wrap_retention) {
    SKIP("retention mode only (flush wraps are gated before the W publish)");
  }
  constexpr size_t kPeerVol = size_t{4} * 1024 * 1024;
  constexpr size_t kPeerContent = size_t{64} * 1024 - 132;
  TempCacheDir tmp("rv-crash");
  const std::string path = tmp.path();
  CacheConfig cfg;
  cfg.set_multi_process(0, 1);
  cfg.set_ram_cache_size(0);
  cfg.read_lease_duration = std::chrono::milliseconds(600000);
  cfg.lease_wrap_ceiling = std::chrono::milliseconds(600000);
  auto created = Cache::create(cfg);
  REQUIRE(created.has_value());
  auto view = std::move(*created);
  REQUIRE(view->add_volume(path, kPeerVol).has_value());
  REQUIRE(view->start().has_value());
  REQUIRE(view->stats().stripe_count == 1);

  const auto k0 = rv_fill(11, 4096);
  REQUIRE(rv_put(*view, "k0", k0));  // lands at S
  auto h = view->read_sync(CacheKey("k0"));
  REQUIRE(h.has_value());
  REQUIRE(h->renew_lease_strict() == LeaseRenewal::kOk);

  SpawnedPeer peer;
  std::vector<std::string> args = {
      "seam",
      path,
      std::to_string(kPeerVol),
      std::to_string(static_cast<int>(Volume::WriterSeam::kWrapAfterCursor)),
      "crash",
      std::to_string(kPeerContent),
      "wrap"};
  REQUIRE(peer.spawn(peer_exe(), args));
  auto code = peer.wait_exit(std::chrono::milliseconds(60000));
  REQUIRE(code.has_value());
  REQUIRE(*code == 42);
  // k0's bytes are intact at this point.
  REQUIRE(rv_eq(h->content(), k0));

  // Next write: proven-dead forced release -> repair -> allocation.  The
  // repair COMPLETES the committed wrap (G enters the new pass with the
  // frontier at S), so this write needs the advance over chunk 0, which
  // defers for k0's live borrow: a dropped write, never an overwrite.
  const uint64_t wraps0 = view->stats().write_buffer_wraps;
  const auto k1 = rv_fill(12, 4096);
  const bool wrote = rv_put(*view, "k1", k1);
  const auto verdict = h->renew_lease_strict();
  const bool intact = rv_eq(h->content(), k0);
  CAPTURE(wrote, static_cast<int>(verdict), intact);
  const auto st = view->stats();
  CAPTURE(st.write_buffer_wraps, st.frontier_advances,
          st.advances_deferred_by_lease);
  // The safety property: never kOk over torn bytes.
  CHECK((intact || verdict == LeaseRenewal::kTorn));
  CHECK_FALSE(wrote);                          // deferred for the borrow
  CHECK(verdict == LeaseRenewal::kOk);         // k0 intact and protected
  CHECK(st.write_buffer_wraps == wraps0 + 1);  // the repair's completion
  // Closing the borrow lets the advance, and the write, through.
  h = make_unexpected(CacheError::NotFound);
  CHECK(rv_put(*view, "k1", k1));
  view->stop();
}

// Retention: a mandatory advance deferred at chunk c starts the episode
// clock; the borrow goes away and an EARLY advance crosses c (no mandatory
// advance ever passes, so the clock is never reset).  Much later a borrow
// in chunk c' defers the next mandatory advance for the FIRST time -- it
// must start a fresh episode, not be force-torn at once by the stale one.
TEST_CASE(
    "Retention review R3: a stale deferral episode does not force-tear a "
    "fresh borrow",
    "[retention][review][lease][episode]") {
  if (!CacheConfig{}.wrap_retention) {
    SKIP("retention mode only");
  }
  TempCacheDir tmp("rv-episode");
  auto c = rv_open(tmp.path(), false, std::chrono::milliseconds(600000),
                   std::chrono::milliseconds(300));
  REQUIRE(c->stats().stripe_count == 1);
  constexpr size_t kContent = 60000;
  constexpr uint64_t kDocBytes = (kContent + 132 + 7) / 8 * 8;
  constexpr uint64_t kQ = uint64_t{1} << 20;
  std::vector<std::string> p0;
  for (size_t i = 0;; ++i) {
    const auto st = c->stats();
    if (st.current_bytes + kDocBytes > st.stripe_bytes) break;
    p0.push_back("e0-" + std::to_string(i));
    REQUIRE(rv_put(*c, p0.back(), rv_fill(i, kContent)));
  }
  auto first_in_chunk = [&](uint64_t chunk) {
    return static_cast<size_t>((chunk * kQ + kDocBytes - 1) / kDocBytes) + 1;
  };
  const size_t b1_idx = first_in_chunk(3);
  const size_t b2_idx = first_in_chunk(6);
  auto b1 = c->read_sync(CacheKey(p0[b1_idx]));
  REQUIRE(b1.has_value());
  // Pass 1 until the mandatory advance into chunk 3 is deferred.
  size_t j = 0;
  for (;; ++j) {
    REQUIRE(j < p0.size());
    if (!rv_put(*c, "e1-" + std::to_string(j), rv_fill(5000 + j, kContent)))
      break;
  }
  REQUIRE(c->stats().advances_deferred_by_lease == 1);
  b1 = make_unexpected(CacheError::NotFound);  // release B1
  // A tiny document fits in the runway; its EARLY advance crosses chunk 3.
  const auto adv_before = c->stats().frontier_advances;
  REQUIRE(rv_put(*c, "tiny", rv_fill(1, 100)));
  CAPTURE(c->stats().frontier_advances - adv_before);
  std::this_thread::sleep_for(std::chrono::milliseconds(600));
  auto b2 = c->read_sync(CacheKey(p0[b2_idx]));
  REQUIRE(b2.has_value());
  REQUIRE(b2->renew_lease_strict() == LeaseRenewal::kOk);
  const auto forced0 = c->stats().wraps_forced_past_lease;
  const auto def0 = c->stats().advances_deferred_by_lease;
  // Fill until the first mandatory advance into chunk 6 is attempted.
  for (++j;; ++j) {
    REQUIRE(j < 4 * p0.size());
    const auto st0 = c->stats();
    const bool ok =
        rv_put(*c, "e1-" + std::to_string(j), rv_fill(5000 + j, kContent));
    const auto st1 = c->stats();
    if (st1.wraps_forced_past_lease != st0.wraps_forced_past_lease ||
        st1.advances_deferred_by_lease != st0.advances_deferred_by_lease ||
        !ok) {
      break;
    }
  }
  const auto st = c->stats();
  CAPTURE(st.wraps_forced_past_lease - forced0,
          st.advances_deferred_by_lease - def0);
  // Correct: the first contact defers (fresh episode), no force.
  CHECK(st.wraps_forced_past_lease == forced0);
  CHECK(b2->renew_lease_strict() == LeaseRenewal::kOk);
  b2 = make_unexpected(CacheError::NotFound);
  c->stop();
}

// Retention: a ceiling-forced advance at chunk c1 resets EVERY chunk's
// borrow slot (S4).  A live borrow B in a far chunk c2 keeps renewing kOk
// (its chunk is not exposed) but is no longer counted, so the later NORMAL
// advance over c2 passes its gate and pwrites under B.  Correct: either B
// learns at once (renew != kOk after the force), or the advance over c2
// still defers for B.  Both mp and single-process.
TEST_CASE(
    "Retention review R2: after a ceiling-forced advance a far borrow is "
    "either torn at once or still counted",
    "[retention][review][lease][forced]") {
  if (!CacheConfig{}.wrap_retention) {
    SKIP("retention mode only");
  }
  for (bool mp : {false, true}) {
    CAPTURE(mp);
    TempCacheDir tmp(mp ? "rv-force-mp" : "rv-force");
    auto c = rv_open(tmp.path(), mp, std::chrono::milliseconds(600000),
                     std::chrono::milliseconds(200));
    REQUIRE(c->stats().stripe_count == 1);
    constexpr size_t kContent = 60000;
    constexpr uint64_t kDocBytes = (kContent + 132 + 7) / 8 * 8;
    constexpr uint64_t kQ = uint64_t{1} << 20;
    std::vector<std::string> p0;
    for (size_t i = 0;; ++i) {
      const auto st = c->stats();
      if (st.current_bytes + kDocBytes > st.stripe_bytes) break;
      p0.push_back("x0-" + std::to_string(i));
      REQUIRE(rv_put(*c, p0.back(), rv_fill(i, kContent)));
    }
    auto first_in_chunk = [&](uint64_t chunk) {
      return static_cast<size_t>((chunk * kQ + kDocBytes - 1) / kDocBytes) + 1;
    };
    const size_t l_idx = first_in_chunk(2);
    const size_t b_idx = first_in_chunk(8);
    REQUIRE(b_idx < p0.size());
    auto L = c->read_sync(CacheKey(p0[l_idx]));
    auto B = c->read_sync(CacheKey(p0[b_idx]));
    REQUIRE(L.has_value());
    REQUIRE(B.has_value());
    size_t j = 0;
    for (;; ++j) {
      REQUIRE(j < p0.size());
      if (!rv_put(*c, "x1-" + std::to_string(j), rv_fill(7000 + j, kContent)))
        break;
    }
    REQUIRE(c->stats().advances_deferred_by_lease >= 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    const auto forced0 = c->stats().wraps_forced_past_lease;
    // Retry until the ceiling forces the advance over chunk 2.
    for (int k = 0; k < 50 && c->stats().wraps_forced_past_lease == forced0;
         ++k) {
      (void)rv_put(*c, "x1-" + std::to_string(j), rv_fill(7000 + j, kContent));
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    REQUIRE(c->stats().wraps_forced_past_lease == forced0 + 1);
    ++j;
    const auto after_force = B->renew_lease_strict();
    CAPTURE(static_cast<int>(after_force));
    L = make_unexpected(CacheError::NotFound);
    const auto def0 = c->stats().advances_deferred_by_lease;
    const auto forced1 = c->stats().wraps_forced_past_lease;
    bool deferred_for_b = false;
    size_t written = 0;
    for (; j < 4 * p0.size(); ++j) {
      if (!rv_put(*c, "x1-" + std::to_string(j), rv_fill(7000 + j, kContent))) {
        deferred_for_b = true;
        break;
      }
      ++written;
      if (!rv_eq(B->content(), rv_fill(b_idx, kContent))) break;  // overwritten
    }
    const bool intact = rv_eq(B->content(), rv_fill(b_idx, kContent));
    const auto final_verdict = B->renew_lease_strict();
    CAPTURE(written, deferred_for_b, intact, static_cast<int>(final_verdict),
            c->stats().advances_deferred_by_lease - def0,
            c->stats().wraps_forced_past_lease - forced1);
    CHECK((after_force != LeaseRenewal::kOk || deferred_for_b));
    B = make_unexpected(CacheError::NotFound);
    c->stop();
  }
}

// Flush-mode twin of the stale-episode case: the bug predates retention.
TEST_CASE(
    "Retention review R3 (flush): a stale wrap-deferral episode does not "
    "force-tear a fresh borrow",
    "[retention][review][lease][episode]") {
  if (CacheConfig{}.wrap_retention) {
    SKIP("flush mode only");
  }
  TempCacheDir tmp("rv-episode-fl");
  auto c = rv_open(tmp.path(), false, std::chrono::milliseconds(600000),
                   std::chrono::milliseconds(300));
  REQUIRE(c->stats().stripe_count == 1);
  constexpr size_t kContent = 60000;
  constexpr uint64_t kDocBytes = (kContent + 132 + 7) / 8 * 8;
  std::vector<std::string> p0;
  for (size_t i = 0;; ++i) {
    const auto st = c->stats();
    if (st.current_bytes + kDocBytes > st.stripe_bytes) break;
    p0.push_back("q0-" + std::to_string(i));
    REQUIRE(rv_put(*c, p0.back(), rv_fill(i, kContent)));
  }
  auto b1 = c->read_sync(CacheKey(p0[3]));
  REQUIRE(b1.has_value());
  REQUIRE_FALSE(rv_put(*c, "big-1", rv_fill(1, kContent)));  // wrap deferred
  REQUIRE(c->stats().wraps_deferred_by_lease == 1);
  b1 = make_unexpected(CacheError::NotFound);
  REQUIRE(rv_put(*c, "tiny", rv_fill(2, 100)));  // fits the tail, no wrap
  std::this_thread::sleep_for(std::chrono::milliseconds(600));
  auto b2 = c->read_sync(CacheKey(p0[5]));
  REQUIRE(b2.has_value());
  const auto forced0 = c->stats().wraps_forced_past_lease;
  (void)rv_put(*c, "big-2", rv_fill(3, kContent));  // first contact with B2
  CAPTURE(c->stats().wraps_forced_past_lease - forced0);
  CHECK(c->stats().wraps_forced_past_lease == forced0);
  b2 = make_unexpected(CacheError::NotFound);
  c->stop();
}

// ===========================================================================
// Review R4 -- an alternate write over a RETAINED head carries the chain
// forward (design section 4.5, "Carry-forward").  D5 still holds: the new
// head links only to current-pass copies written in its own slot.
// Every case below forces retention on, so it runs identically in both
// suite modes.
// ===========================================================================

namespace {

std::unique_ptr<Cache> carry_open(const std::string &path, bool mp,
                                  size_t max_object_size = 0) {
  CacheConfig cfg;
  if (mp) cfg.set_multi_process(0, 1);
  cfg.set_ram_cache_size(0);
  cfg.set_wrap_retention(true);
  cfg.read_lease_duration = std::chrono::milliseconds(0);
  cfg.lease_wrap_ceiling = std::chrono::milliseconds(0);
  if (max_object_size != 0) cfg.max_object_size = max_object_size;
  auto c = Cache::create(cfg);
  REQUIRE(c.has_value());
  REQUIRE((*c)->add_volume(path, kRvVol).has_value());
  REQUIRE((*c)->start().has_value());
  REQUIRE((*c)->stats().stripe_count == 1);
  return std::move(*c);
}

// Churn `c` with `filler` documents until the stripe has wrapped once more.
void churn_to_next_wrap(Cache &c, size_t &fi,
                        std::span<const std::byte> filler) {
  const uint64_t w0 = c.stats().write_buffer_wraps;
  while (c.stats().write_buffer_wraps == w0) {
    REQUIRE(rv_put(c, "cf" + std::to_string(fi++), filler));
  }
}

struct CarryChain {
  std::string key = "http://example.com/hero.jpg";
  std::vector<std::byte> orig = rv_fill(31, 800000);
  std::vector<std::byte> gz = rv_fill(32, 200000);
  std::vector<std::byte> webp = rv_fill(33, 150000);
  std::vector<std::byte> avif = rv_fill(34, 50000);

  // Pass 0: fillers to about 6 MB, then the chain, then churn into pass 1
  // and a few fillers.  The chain ends up RETAINED, well above the chunks
  // the next writes expose.
  void build(Cache &c, size_t &fi, std::span<const std::byte> filler) const {
    for (int i = 0; i < 100; ++i) {
      REQUIRE(rv_put(c, "cf" + std::to_string(fi++), filler));
    }
    REQUIRE(rv_put_alt(c, key, AlternateId::Original, orig));
    REQUIRE(rv_put_alt(c, key, AlternateId::Gzip, gz));
    REQUIRE(rv_put_alt(c, key, AlternateId::WebP, webp));
    churn_to_next_wrap(c, fi, filler);
    for (int i = 0; i < 5; ++i) {
      REQUIRE(rv_put(c, "cf" + std::to_string(fi++), filler));
    }
  }
  // The old chain, complete and exact.
  [[nodiscard]] bool old_complete(Cache &c) const {
    return rv_ids(c, key) == std::set<int>{0, 3, 16} &&
           rv_serves_alt(c, key, AlternateId::Original, orig) &&
           rv_serves_alt(c, key, AlternateId::Gzip, gz) &&
           rv_serves_alt(c, key, AlternateId::WebP, webp);
  }
  // The new chain, complete and exact (AVIF content as the caller wrote it).
  [[nodiscard]] bool new_complete(Cache &c,
                                  std::span<const std::byte> avif_now) const {
    return rv_ids(c, key) == std::set<int>{0, 3, 16, 17} &&
           rv_serves_alt(c, key, AlternateId::Original, orig) &&
           rv_serves_alt(c, key, AlternateId::Gzip, gz) &&
           rv_serves_alt(c, key, AlternateId::WebP, webp) &&
           rv_serves_alt(c, key, AlternateId::AVIF, avif_now);
  }
};

}  // namespace

TEST_CASE(
    "Retention R4: an alternate write over a retained head keeps every "
    "alternate of the key resolving, in single- and multi-process mode",
    "[retention][carry][alternate]") {
  for (bool mp : {false, true}) {
    CAPTURE(mp);
    TempCacheDir tmp(mp ? "r4-keep-mp" : "r4-keep");
    auto c = carry_open(tmp.path(), mp);
    const auto filler = rv_fill(99, 60000);
    size_t fi = 0;
    const CarryChain ch;
    ch.build(*c, fi, filler);
    REQUIRE(ch.old_complete(*c));
    REQUIRE(c->stats().retained_hits > 0);  // served from the retained pass

    const auto st0 = c->stats();
    REQUIRE(rv_put_alt(*c, ch.key, AlternateId::AVIF, ch.avif));
    const auto st1 = c->stats();
    REQUIRE(ch.new_complete(*c, ch.avif));
    REQUIRE(st1.alternates_carried_forward - st0.alternates_carried_forward ==
            3);
    REQUIRE(st1.alternates_carry_dropped == st0.alternates_carry_dropped);
    REQUIRE(st1.alternate_wrap_refusals == st0.alternate_wrap_refusals);
    // Every node of the new chain is CURRENT: it survives the frontier
    // sweeping the old chain's chunks (fill the rest of this pass).
    const uint64_t w = c->stats().write_buffer_wraps;
    for (int i = 0; i < 150 && c->stats().write_buffer_wraps == w; ++i) {
      REQUIRE(rv_put(*c, "cf" + std::to_string(fi++), filler));
    }
    REQUIRE(ch.new_complete(*c, ch.avif));
    c->stop();
  }
}

TEST_CASE(
    "Retention R4: a peer view sees the carried chain complete, never a "
    "prefix of it",
    "[retention][carry][alternate][multiprocess]") {
  TempCacheDir tmp("r4-peer");
  auto open_view = [&](uint32_t index) {
    CacheConfig cfg;
    cfg.set_multi_process(index, 2);
    cfg.set_ram_cache_size(0);
    cfg.set_wrap_retention(true);
    cfg.read_lease_duration = std::chrono::milliseconds(0);
    cfg.lease_wrap_ceiling = std::chrono::milliseconds(0);
    auto c = Cache::create(cfg);
    REQUIRE(c.has_value());
    REQUIRE((*c)->add_volume(tmp.path(), kRvVol).has_value());
    REQUIRE((*c)->start().has_value());
    return std::move(*c);
  };
  auto writer = open_view(0);  // owns the single stripe
  auto reader = open_view(1);  // owns nothing
  const auto filler = rv_fill(99, 60000);
  size_t fi = 0;
  const CarryChain ch;
  ch.build(*writer, fi, filler);
  REQUIRE(ch.old_complete(*reader));
  // The reader is not the owner: it cannot write, so it cannot carry.
  REQUIRE_FALSE(rv_put_alt(*reader, ch.key, AlternateId::AVIF, ch.avif));
  REQUIRE(ch.old_complete(*reader));

  // Freeze the owner right before its publish: the peer still sees the old
  // chain, complete (the carried copies are not reachable yet).
  std::atomic<bool> parked{false};
  std::atomic<bool> release{false};
  Volume::s_carry_seam_for_test = [&](Volume::CarrySeam at) {
    if (at == Volume::CarrySeam::kFilled) {
      parked.store(true);
      while (!release.load()) {
        std::this_thread::yield();
      }
    }
  };
  std::atomic<bool> wrote{false};
  std::thread w([&] {
    wrote.store(rv_put_alt(*writer, ch.key, AlternateId::AVIF, ch.avif));
  });
  while (!parked.load()) {
    std::this_thread::yield();
  }
  CHECK(ch.old_complete(*reader));
  release.store(true);
  w.join();
  Volume::s_carry_seam_for_test = nullptr;
  REQUIRE(wrote.load());
  REQUIRE(ch.new_complete(*reader, ch.avif));
  REQUIRE(ch.new_complete(*writer, ch.avif));
  reader->stop();
  writer->stop();
}

TEST_CASE(
    "Retention R4: the carry is capped -- the Original and the newest "
    "alternates are kept, the oldest are dropped and counted",
    "[retention][carry][alternate]") {
  SECTION("byte cap") {
    // max_object_size = 40 KiB caps one carry at 40 KiB: four 10 KB
    // alternates (10136 bytes on disk each) of the six retained ones.
    TempCacheDir tmp("r4-cap-bytes");
    auto c = carry_open(tmp.path(), false, size_t{40} * 1024);
    const auto filler = rv_fill(99, 30000);
    size_t fi = 0;
    for (int i = 0; i < 200; ++i) {
      REQUIRE(rv_put(*c, "cf" + std::to_string(fi++), filler));
    }
    const std::string key = "http://example.com/capped";
    const std::array<AlternateId, 6> order = {
        AlternateId::Original, AlternateId::Brotli, AlternateId::Zstd,
        AlternateId::Gzip,     AlternateId::WebP,   AlternateId::JpegXL};
    for (size_t i = 0; i < order.size(); ++i) {
      REQUIRE(rv_put_alt(*c, key, order[i], rv_fill(40 + i, 10000)));
    }
    churn_to_next_wrap(*c, fi, filler);
    REQUIRE(rv_ids(*c, key) == std::set<int>{0, 1, 2, 3, 16, 18});

    const auto st0 = c->stats();
    REQUIRE(rv_put_alt(*c, key, AlternateId::AVIF, rv_fill(50, 10000)));
    const auto st1 = c->stats();
    // Kept: the Original (always first), then the newest three (JpegXL,
    // WebP, Gzip).  Dropped: the two oldest others (Zstd, Brotli).
    REQUIRE(rv_ids(*c, key) == std::set<int>{0, 3, 16, 17, 18});
    REQUIRE(st1.alternates_carried_forward - st0.alternates_carried_forward ==
            4);
    REQUIRE(st1.alternate_carry_bytes - st0.alternate_carry_bytes ==
            4 * uint64_t{10136});
    REQUIRE(st1.alternates_carry_dropped - st0.alternates_carry_dropped == 2);
    REQUIRE(rv_serves_alt(*c, key, AlternateId::Original, rv_fill(40, 10000)));
    REQUIRE(rv_serves_alt(*c, key, AlternateId::Gzip, rv_fill(43, 10000)));
    REQUIRE(rv_serves_alt(*c, key, AlternateId::WebP, rv_fill(44, 10000)));
    REQUIRE(rv_serves_alt(*c, key, AlternateId::JpegXL, rv_fill(45, 10000)));
    REQUIRE(rv_serves_alt(*c, key, AlternateId::AVIF, rv_fill(50, 10000)));
    c->stop();
  }
  SECTION("count cap") {
    // kMaxAlternatesPerKey distinct ids retained; a NEW id joins them.  The
    // carry keeps kMaxAlternatesPerKey - 1 (the Original and the newest
    // others), drops the oldest other one, and the write succeeds -- it is
    // not refused with TooManyAlternates.
    TempCacheDir tmp("r4-cap-count");
    auto c = carry_open(tmp.path(), false);
    const auto filler = rv_fill(99, 60000);
    size_t fi = 0;
    for (int i = 0; i < 100; ++i) {
      REQUIRE(rv_put(*c, "cf" + std::to_string(fi++), filler));
    }
    const std::string key = "http://example.com/many";
    constexpr int kIds = kMaxAlternatesPerKey;
    for (int id = 0; id < kIds; ++id) {
      REQUIRE(rv_put_alt(*c, key, static_cast<AlternateId>(id),
                         rv_fill(200 + id, 100)));
    }
    churn_to_next_wrap(*c, fi, filler);
    REQUIRE(rv_ids(*c, key).size() == kIds);
    const auto st0 = c->stats();
    REQUIRE(rv_put_alt(*c, key, static_cast<AlternateId>(kIds),
                       rv_fill(200 + kIds, 100)));
    const auto st1 = c->stats();
    std::set<int> want;
    for (int id = 0; id <= kIds; ++id) {
      if (id != 1) want.insert(id);  // id 1: the oldest non-Original
    }
    REQUIRE(rv_ids(*c, key) == want);
    REQUIRE(st1.alternates_carried_forward - st0.alternates_carried_forward ==
            kIds - 1);
    REQUIRE(st1.alternates_carry_dropped - st0.alternates_carry_dropped == 1);
    REQUIRE(rv_serves_alt(*c, key, AlternateId::Original, rv_fill(200, 100)));
    REQUIRE(rv_serves_alt(*c, key, static_cast<AlternateId>(kIds),
                          rv_fill(200 + kIds, 100)));
    // A current chain of kMaxAlternatesPerKey ids refuses a new one, as
    // before: the carry does not lift the per-key limit.
    auto refused = c->write_alternate_sync(
        CacheKey(key), static_cast<AlternateId>(kIds + 1), 100);
    if (refused.has_value()) {
      const auto body = rv_fill(9, 100);
      REQUIRE(refused->write_sync(body).has_value());
      auto closed = refused->close_sync();
      REQUIRE_FALSE(closed.has_value());
      REQUIRE(closed.error() == CacheError::TooManyAlternates);
    }
    c->stop();
  }
}

namespace {

const char *kCarrySteps[] = {"copied", "intent", "gate",
                             "epoch",  "tear",   "filled"};

void spawn_carry_peer(SpawnedPeer &peer, const std::string &path,
                      const char *step, bool crash, const std::string &key,
                      size_t content_bytes) {
  REQUIRE(peer.spawn(peer_exe(), {"carry", path, std::to_string(kRvVol), step,
                                  crash ? "crash" : "hang", key,
                                  std::to_string(content_bytes)}));
}

}  // namespace

TEST_CASE(
    "Retention R4: a writer killed at any step of a carry-forward leaves the "
    "old retained chain complete, and the next write carries it",
    "[retention][carry][crash][multiprocess]") {
  for (const char *step : kCarrySteps) {
    CAPTURE(step);
    TempCacheDir tmp("r4-crash");
    const std::string path = tmp.path();
    // The parent view stays open across the crash (no exclusive reopen):
    // the only repair available is the next writer's forced release.
    auto view = carry_open(path, /*mp=*/true);
    const auto filler = rv_fill(99, 60000);
    size_t fi = 0;
    const CarryChain ch;
    ch.build(*view, fi, filler);
    REQUIRE(ch.old_complete(*view));

    const auto peer_avif =
        std::vector<std::byte>(ch.avif.size(), std::byte{0x5A});
    SpawnedPeer peer;
    spawn_carry_peer(peer, path, step, /*crash=*/true, ch.key, ch.avif.size());
    auto code = peer.wait_exit(kPeerDeadline);
    REQUIRE(code.has_value());
    REQUIRE(*code == 42);  // died AT the step

    // The next write repairs whatever the dead writer held (write lock,
    // wrap intent) -- then the key must resolve to the OLD chain, complete:
    // nothing of the dead carry was ever published.
    REQUIRE(rv_put(*view, "after-crash", filler));
    CHECK(ch.old_complete(*view));
    CHECK_FALSE(rv_serves_alt(*view, ch.key, AlternateId::AVIF, peer_avif));

    // And the next alternate write carries it, complete.
    REQUIRE(rv_put_alt(*view, ch.key, AlternateId::AVIF, ch.avif));
    CHECK(ch.new_complete(*view, ch.avif));
    CHECK(view->stats().alternates_carried_forward == 3);
    view->stop();
  }
}

TEST_CASE(
    "Retention R4: while a peer's carry-forward is parked at a step, readers "
    "see the old chain complete; released, the new chain complete",
    "[retention][carry][crash][multiprocess]") {
  // The steps with no wrap intent held (inside an advance's intent window
  // every read of the stripe retries by design, so there is nothing to
  // observe there but misses).
  for (const char *step : {"copied", "tear", "filled"}) {
    CAPTURE(step);
    TempCacheDir tmp("r4-park");
    const std::string path = tmp.path();
    auto view = carry_open(path, /*mp=*/true);
    const auto filler = rv_fill(99, 60000);
    size_t fi = 0;
    const CarryChain ch;
    ch.build(*view, fi, filler);

    SpawnedPeer peer;
    spawn_carry_peer(peer, path, step, /*crash=*/false, ch.key, ch.avif.size());
    auto ready = peer.wait_ready(kPeerDeadline);
    REQUIRE(ready.has_value());
    REQUIRE(*ready == "READY");
    CHECK(ch.old_complete(*view));
    peer.request_exit();  // _exit(0) without finishing the write
    auto code = peer.wait_exit(kPeerDeadline);
    REQUIRE(code.has_value());
    REQUIRE(rv_put(*view, "after-park", filler));
    CHECK(ch.old_complete(*view));
    view->stop();
  }
}

// ---------------------------------------------------------------------------
// R4 hammer: readers walk and read a key's chain -- including its EXISTING
// Original -- while one writer churns the ring and writes new alternates
// onto the same keys, so carry-forwards (retention) and plain prepends
// (flush) run under live readers.  Every (key, id) is always written with
// the same bytes, so any served alternate must be exactly those bytes; a
// miss is allowed (flush drops whole passes; a key may be mid-rewrite).
// Under TSan this is the data-race check of the carry's copy (plain loads
// of retained bytes) against lock-free readers.
// ---------------------------------------------------------------------------

TEST_CASE(
    "Retention R4: hammer -- readers walk alternate chains while alternate "
    "writes carry them forward across wraps",
    "[retention][carry][hammer][concurrent]") {
  const bool retention = GENERATE(false, true);
  CAPTURE(retention);
  TempCacheDir tmp("r4-hammer");
  CacheConfig cfg;
  cfg.set_ram_cache_size(0);
  cfg.set_wrap_retention(retention);
  cfg.read_lease_duration = std::chrono::milliseconds(2000);
  cfg.lease_wrap_ceiling = std::chrono::milliseconds(60);
  auto created = Cache::create(cfg);
  REQUIRE(created.has_value());
  auto &c = **created;
  REQUIRE(c.add_volume(tmp.path(), kRetVol).has_value());
  REQUIRE(c.start().has_value());

  constexpr size_t kKeys = 4;
  const std::array<AlternateId, 6> ids = {
      AlternateId::Original, AlternateId::Gzip, AlternateId::Brotli,
      AlternateId::WebP,     AlternateId::AVIF, AlternateId::JpegXL};
  auto key_of = [](size_t k) { return "r4h-" + std::to_string(k); };
  auto body_of = [](size_t k, size_t i) {
    return rv_fill(1000 + k * 16 + i, 3000 + 1500 * i);
  };

  std::atomic<bool> stop{false};
  std::atomic<bool> mismatch{false};
  std::atomic<uint64_t> served{0};
  std::atomic<uint64_t> original_reads{0};
  std::atomic<uint64_t> original_misses{0};
  std::atomic<uint64_t> forced_tears{0};

  std::thread writer([&] {
    const auto filler = rv_fill(77, 60000);
    size_t fi = 0;
    size_t round = 0;
    while (!stop.load()) {
      for (int f = 0; f < 6 && !stop.load(); ++f) {
        (void)rv_put(c, "r4f" + std::to_string(fi++), filler);
      }
      const size_t k = round % kKeys;
      const size_t i = (round / kKeys) % ids.size();
      ++round;
      // The Original first when the key has none (flush drops it with its
      // pass); otherwise the next alternate in the cycle.
      const size_t pick = rv_ids(c, key_of(k)).count(0) == 0 ? 0 : i;
      (void)rv_put_alt(c, key_of(k), ids[pick], body_of(k, pick));
    }
  });
  std::vector<std::thread> readers;
  readers.reserve(4);
  for (unsigned r = 0; r < 4; ++r) {
    readers.emplace_back([&, r] {
      uint64_t rng = 0x9E3779B97F4A7C15ULL * (r + 1);
      auto next = [&rng] {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        return rng;
      };
      while (!stop.load()) {
        const size_t k = next() % kKeys;
        // Half the reads target the Original, the one alternate that
        // already exists while the others are written.
        const size_t i = next() % 2 == 0 ? 0 : next() % ids.size();
        const uint64_t forced0 = c.stats().wraps_forced_past_lease;
        DefaultStorageSelector sel;
        std::array<AlternateId, 1> acc{ids[i]};
        AlternateSelectionContext ctx;
        ctx.acceptable_alternates = acc;
        auto rh = c.read_alternate_sync(CacheKey(key_of(k)), sel, ctx);
        if (i == 0) original_reads.fetch_add(1);
        if (!rh.has_value()) {
          if (i == 0) original_misses.fetch_add(1);
          continue;
        }
        served.fetch_add(1);
        const auto want = body_of(k, i);
        bool ok = rv_eq(rh->content(), want);
        // Hold some borrows across strict renews, like the main hammer.
        for (int h = 0; ok && h < 8 && next() % 4 == 0; ++h) {
          if (rh->renew_lease_strict() == LeaseRenewal::kTorn) break;
          ok = rv_eq(rh->content(), want);
          std::this_thread::yield();
        }
        if (!ok) {
          if (c.stats().wraps_forced_past_lease == forced0) {
            mismatch.store(true);
          } else {
            forced_tears.fetch_add(1);
          }
        }
        // The chain as a list: every listed alternate has its exact size.
        if (next() % 8 == 0) {
          auto alts = c.list_alternates_sync(CacheKey(key_of(k)));
          if (alts.has_value()) {
            for (const auto &a : *alts) {
              size_t idx = ids.size();
              for (size_t j = 0; j < ids.size(); ++j) {
                if (ids[j] == a.id) idx = j;
              }
              if (idx == ids.size() ||
                  a.content_length != body_of(k, idx).size()) {
                mismatch.store(true);
              }
            }
          }
        }
      }
    });
  }
  const auto start = std::chrono::steady_clock::now();
  while (
      std::chrono::steady_clock::now() - start <
          std::chrono::milliseconds(1500) ||
      (c.stats().write_buffer_wraps < 3 &&
       std::chrono::steady_clock::now() - start < std::chrono::seconds(90))) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  stop.store(true);
  writer.join();
  for (auto &t : readers) t.join();

  const auto st = c.stats();
  INFO("served " << served.load() << ", Original reads "
                 << original_reads.load() << " misses "
                 << original_misses.load() << ", forced tears "
                 << forced_tears.load() << ", wraps " << st.write_buffer_wraps
                 << ", carried " << st.alternates_carried_forward << " ("
                 << st.alternate_carry_bytes << " B), dropped "
                 << st.alternates_carry_dropped);
  REQUIRE_FALSE(mismatch.load());
  REQUIRE(served.load() > 0);
  REQUIRE(st.write_buffer_wraps >= 3);
  if (retention) {
    REQUIRE(st.alternates_carried_forward > 0);
  } else {
    REQUIRE(st.alternates_carried_forward == 0);
    REQUIRE(st.alternate_carry_bytes == 0);
  }
  c.stop();
}
