// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Regression + demonstrator coverage for wrap-time directory invariants
// around the single-bit GC phase (Volume::evict_if_needed ->
// Directory::toggle_phase; surviving entries are not scrubbed on wrap):
//
//   (A) 2-wrap phase ABA, offset REUSED.  An entry that survives two wraps
//       of its stripe reads as current-phase again and aliases a region the
//       later passes reused for a DIFFERENT document.  Survival is
//       guaranteed BY CONSTRUCTION here: every flood key is chosen so its
//       directory bucket differs from the victim's, so no insert ever
//       touches the victim's bucket (stale-slot reclaim, in-place-update
//       election and collision eviction all operate on the inserted key's
//       bucket only — see Directory::insert).  Invariant pinned: the read
//       path re-verifies the stored 256-bit first_key (a DirEntry carries
//       only a 12-bit tag), so the aliased entry resolves to a clean miss,
//       never a stale or foreign serve.  Single-threaded scope.
//
//   (B) Checksum-validation cache staleness.  The 64K-slot
//       offset->top16(CRC32) cache is never invalidated on wrap; after
//       same-offset reuse, a new document whose checksum shares the cached
//       verdict's top 16 bits (2^-16) SKIPS re-verification.  The collision
//       is constructed deterministically below so the skip path really
//       fires.  NOTE ON WHAT IS OBSERVED: no counter distinguishes
//       "CRC skipped" from "CRC re-verified", so this test exercises the
//       skip path without observing it directly; its load-bearing
//       assertions are that the served bytes are the new document's real
//       bytes and the old key misses cleanly.  Single-threaded scope.
//
//   (C) 2-wrap trailing-gap survivor vs the UNGATED FORWARD FILL, closed by
//       the phase-ABA POSITIONAL READ GUARD.  With variable-size documents, a
//       doc J can sit in the trailing gap that two successive passes leave
//       unfilled (its bytes intact), while its entry survives in a cold
//       bucket.  After two phase toggles the entry reads current-phase and J
//       sits AHEAD of write_pos.  Formerly a reader took a legitimate zero-copy
//       borrow through it (full key matches — the bytes are real) and the
//       writer's subsequent NORMAL FORWARD FILL crossed J's offset and
//       overwrote it in place: no wrap fired, so neither lease_permits_wrap nor
//       the wrap-intent/epoch revalidation ran (both live inside
//       allocate_write_slot's wrap branch only), voiding the lease premise
//       that "any overwrite of a borrowed region is a wrap".  The fix adds a
//       positional guard at the read/probe choke point (Stripe::probe_each):
//       a current-phase entry whose offset is at/ahead of the stripe write
//       cursor is a definitional phase-ABA survivor and is rejected BEFORE any
//       borrow is handed out — the one leg the full-key + CRC gauntlet cannot
//       supply, since J's bytes are genuinely J's until the fill lands.  This
//       test now asserts that restored invariant UNCONDITIONALLY: the post-two-
//       wrap read of J is a clean miss (even at the exact offset == cursor
//       boundary), no borrow is ever outstanding, and the forward fill that
//       reuses X therefore has nothing to tear.  Single-threaded scope; the
//       cross-view (shared-cursor) variant is (D).
//
//   (D) MP variant of (C): the guard must read the SHARED write cursor (a
//       non-owning view's local cursor is frozen), proven by a two-view
//       serve/reject pair.
//
//   (E) ONE-wrap dark chain tail: the guard's chain-HOP leg
//       (Stripe::admit_hop).  A head committed across a wrap keeps
//       next_alternate_offset pointing at the pre-wrap old head — a same-key,
//       intact, ahead-of-cursor node the probe leg never sees (hops bypass
//       the directory).  Walks must never enumerate/select/borrow it.
//
//   (F) Reopen persistence: the shared cursor the guard compares against is
//       recovered from the mmap header on reopen; behind-cursor entries must
//       still serve after close + reopen of a wrapped MP-directory volume.
//
// The lease protocol has its own coverage in test_lease_pinning.cpp;
// (A) and (B) disable leases so wraps are never deferred, while (C) runs
// with a LONG lease precisely to show the forward fill ignores it.  Unlike
// the 1-wrap coverage in test_eviction.cpp, checksums are ENABLED
// throughout so the CRC leg of the read gauntlet participates.

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "core/volume.hpp"
#include "cyclone/alternate.hpp"
#include "cyclone/cache.hpp"
#include "cyclone/config.hpp"
#include "cyclone/key.hpp"

#ifndef _WIN32
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifdef _WIN32
#include <process.h>
#define ABA_GETPID _getpid
#else
#define ABA_GETPID getpid
#endif

using namespace cyclone;

namespace {

// Remove the raw path AND the structural-fingerprint sibling(s)
// ("<stem>-<fmt>-<hash><ext>") Cache::add_volume() actually opens -- a plain
// std::remove(raw) no longer clears the file the Volume uses under
// fingerprinting, so a leftover on a persistent /tmp would be reopened stale
// and skew the precise wrap-count / trailing-gap geometry.  Test-only dir walk.
void remove_cache_files(const std::string &raw_path) {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::remove(raw_path, ec);
  const fs::path p(raw_path);
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

std::string create_temp_file(const std::string &name, size_t size_mb) {
  // Unique per process (getpid) AND per call (counter) so no two runs on a
  // persistent /tmp -- and no two cases in one run -- share a cache file.
  static std::atomic<int> counter{0};
  const std::string path =
      (std::filesystem::temp_directory_path() /
       ("cyclone_aba_" + name + "_" + std::to_string(ABA_GETPID()) + "_" +
        std::to_string(counter.fetch_add(1)) + ".dat"))
          .string();
  remove_cache_files(path);
  FILE *f = std::fopen(path.c_str(), "wb");
  if (f != nullptr) {
    std::fseek(f, static_cast<long>(size_mb * 1024 * 1024 - 1), SEEK_SET);
    std::fputc(0, f);
    std::fclose(f);
  }
  return path;
}

// CRC-32C, byte-for-byte identical to Document::compute_checksum (see
// src/core/document.cpp and the convention block in src/core/crc32c.hpp).
// Written out here rather than called, so the forged checksums this file
// plants come from an independent implementation.  Exposed in raw-state form
// so test (B) can brute-force a top-16 collision cheaply (prefix state
// computed once, only a 4-byte tail re-hashed per candidate).
uint32_t oracle_crc32c_update(uint32_t state, std::span<const std::byte> data) {
  static const std::array<uint32_t, 256> table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t n = 0; n < 256; ++n) {
      uint32_t c = n;
      for (int k = 0; k < 8; ++k) {
        c = ((c & 1) != 0u) ? (0x82f63b78 ^ (c >> 1)) : (c >> 1);
      }
      t[n] = c;
    }
    return t;
  }();
  for (std::byte b : data) {
    state = table[(state ^ static_cast<uint8_t>(b)) & 0xFF] ^ (state >> 8);
  }
  return state;
}

uint32_t oracle_crc32c(std::span<const std::byte> data) {
  return oracle_crc32c_update(0xFFFFFFFFu, data) ^ 0xFFFFFFFFu;
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
  if (got.size() != want.size()) {
    return false;
  }
  for (size_t j = 0; j < got.size(); ++j) {
    if (got[j] != want[j]) {
      return false;
    }
  }
  return !got.empty();
}

bool write_entry(Cache &cache, const std::string &key_str,
                 std::span<const std::byte> content) {
  CacheKey key(key_str);
  auto wh = cache.write_sync(key, content.size());
  if (!wh.has_value()) {
    return false;
  }
  if (!wh->write_sync(content).has_value()) {
    return false;
  }
  return wh->close_sync().has_value();
}

// In-memory Directory bucket count: kDirectoryEntriesPerSegment in
// src/core/volume.cpp (~line 149).  Not exported; mirrored here so tests
// can pick flood keys whose bucket differs from a victim's — the
// structural guarantee that the victim's DirEntry survives untouched.  If
// the constant changes upstream, bucket disjointness silently degrades to
// "very probably disjoint"; the assertions themselves stay valid.
constexpr uint32_t kDirectoryBuckets = 16 * 1024;

uint32_t bucket_of(const CacheKey &key) {
  return key.bucket_hash() % kDirectoryBuckets;
}

// Next key with the given prefix whose directory bucket differs from
// avoid_bucket (single-stripe volumes only: all keys share the stripe).
std::string next_disjoint_key(const std::string &prefix, size_t &counter,
                              uint32_t avoid_bucket) {
  for (;;) {
    std::string key = prefix + std::to_string(counter++);
    if (bucket_of(CacheKey(key)) != avoid_bucket) {
      return key;
    }
  }
}

// Flood a single-stripe cache with bucket-disjoint keys carrying `content`
// until write_buffer_wraps reaches `target_wraps`.  Returns the key whose
// write TRIGGERED the target wrap.  A wrap resets write_pos to the
// data-area start before reserving the triggering document's offset
// (Volume::allocate_write_slot), so that document lands at the same
// absolute offset as the very first document ever written.
std::string flood_until_wrap(Cache &cache, size_t &next_index,
                             uint64_t target_wraps,
                             std::span<const std::byte> content,
                             uint32_t avoid_bucket) {
  std::string trigger_key;
  for (size_t guard = 0; guard < 5'000'000; ++guard) {
    std::string key = next_disjoint_key("flood-", next_index, avoid_bucket);
    (void)write_entry(cache, key, content);
    if (cache.stats().write_buffer_wraps >= target_wraps) {
      trigger_key = key;
      break;
    }
  }
  return trigger_key;
}

// Small single-stripe volume: sub-32MB auto-sizing yields one stripe, so
// every wrap targets the same region and offsets are reproducible.
constexpr size_t kVolMB = 4;
constexpr size_t kDocSize = static_cast<size_t>(16 * 1024);

// Selector that picks exactly one AlternateId — lets tests (E) aim a read at
// a specific chain node.
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

bool write_alternate_entry(Cache &cache, const std::string &key_str,
                           AlternateId id, std::span<const std::byte> content) {
  CacheKey key(key_str);
  auto wh = cache.write_alternate_sync(key, id, content.size());
  if (!wh.has_value()) {
    return false;
  }
  if (!wh->write_sync(content).has_value()) {
    return false;
  }
  return wh->close_sync().has_value();
}

CacheConfig aba_config() {
  CacheConfig config;
  config.ram_cache_size = 0;  // every read is a disk (mmap) hit
  config.enable_checksum = true;
  config.verify_checksum_on_read = true;
  // Disable read leases: a wrap must never be deferred in (A)/(B) --
  // the phase machinery itself is under test.
  config.read_lease_duration = std::chrono::milliseconds(0);
  config.lease_wrap_ceiling = std::chrono::milliseconds(0);
  return config;
}

// Geometry of a two-wrap trailing-gap survivor (see TEST (C) for the full
// derivation).  Header/end-slack constants mirror TEST (C).
struct GapSurvivor {
  uint64_t data_area = 0;
  uint64_t b = 0;    // filler doc size
  uint64_t gap = 0;  // trailing gap = data_area % b
  uint64_t n = 0;    // fillers per pass
  size_t idx = 0;    // flood-key counter, continued by the caller
};

// Drive `writer` (which OWNS the single stripe) into the two-wrap trailing-gap
// survivor state: J ("gap-survivor-J") sits in the trailing gap with intact
// bytes, its cold bucket untouched by any flood insert, and after two wraps its
// entry reads current-phase again while J's offset X = n*b sits far AHEAD of
// the write cursor.  Filler = make_content(3, b - kHeaderSize),
// j_content = make_content(11, gap - kEndSlack - kHeaderSize).  On return
// wrap_count == 2 and J is un-reused.
GapSurvivor build_two_wrap_gap_survivor(Cache &writer) {
  GapSurvivor g;
  auto s0 = writer.stats();
  REQUIRE(s0.stripe_count == 1);
  REQUIRE(s0.stripe_bytes > s0.current_bytes);
  g.data_area = s0.stripe_bytes - s0.current_bytes;

  constexpr uint64_t kHeaderSize = 132;  // Document::kHeaderSize
  constexpr uint64_t kEndSlack = 4096;   // >= max approx_size overshoot
  g.b = uint64_t{64} * 1024;
  while (g.b > uint64_t{16} * 1024 &&
         g.data_area % g.b < kEndSlack + kHeaderSize + 4096) {
    g.b -= 4096;
  }
  g.gap = g.data_area % g.b;
  REQUIRE(g.gap >= kEndSlack + kHeaderSize + 4096);
  g.n = g.data_area / g.b;
  REQUIRE(g.n >= 4);

  const uint32_t j_bucket = bucket_of(CacheKey("gap-survivor-J"));
  const auto filler = make_content(3, g.b - kHeaderSize);
  const auto j_content = make_content(11, g.gap - kEndSlack - kHeaderSize);

  // Pass 0: n fillers tile [0, X), then J fills the gap.
  for (uint64_t i = 0; i < g.n; ++i) {
    REQUIRE(
        write_entry(writer, next_disjoint_key("c-", g.idx, j_bucket), filler));
  }
  REQUIRE(writer.stats().write_buffer_wraps == 0);
  REQUIRE(write_entry(writer, "gap-survivor-J", j_content));
  REQUIRE(writer.stats().write_buffer_wraps == 0);

  // Pass 1: wrap #1 (phase -> P1); n fillers tile [0, X), gap untouched.
  for (uint64_t i = 0; i < g.n; ++i) {
    REQUIRE(
        write_entry(writer, next_disjoint_key("c-", g.idx, j_bucket), filler));
  }
  REQUIRE(writer.stats().write_buffer_wraps == 1);

  // Pass 2: wrap #2 (phase -> P0); one filler at offset 0.
  REQUIRE(
      write_entry(writer, next_disjoint_key("c-", g.idx, j_bucket), filler));
  REQUIRE(writer.stats().write_buffer_wraps == 2);
  return g;
}

}  // namespace

// --- (A): 2-wrap phase ABA, offset reused ----------------------------------
TEST_CASE(
    "Two-wrap phase ABA re-exposes a surviving entry but the full-key "
    "gauntlet rejects it",
    "[wrap][aba][eviction]") {
  // Three documents forced to the same data-area offset O across three
  // phases (every doc is kDocSize, so each pass tiles the area identically):
  //   - V (victim)   written first          -> offset O, phase P0
  //   - B (trigger)  wrap #1 lands it at O  -> offset O, phase P1
  //   - C (trigger)  wrap #2 lands it at O  -> offset O, phase P0
  // All flood keys are bucket-disjoint from V, so V's DirEntry survives
  // both wraps verbatim (no insert ever visits its bucket).  After wrap #2
  // the phase is back to P0: V's SURVIVING entry reads current-phase again
  // — a genuine ABA, not ordinary stale-slot reclaim — while O holds C's
  // bytes.  Reading V must be a clean miss (full-key mismatch), never a
  // stale/foreign/torn serve.  Single-threaded scope: the concurrent
  // forward-fill window is demonstrated separately in (C).
  std::string path = create_temp_file("two_wrap", kVolMB);
  auto cache_result = Cache::create(aba_config());
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = path;
  vol_config.size = kVolMB * 1024 * 1024;
  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());
  REQUIRE(cache->stats().stripe_count == 1);

  CacheKey victim_key("victim-aba");
  const uint32_t victim_bucket = bucket_of(victim_key);
  auto victim_content = make_content(7, kDocSize);
  REQUIRE(write_entry(*cache, "victim-aba", victim_content));

  // Baseline: correct read-back before any wrap.
  {
    auto rh = cache->read_sync(victim_key);
    REQUIRE(rh.has_value());
    REQUIRE(content_equals(rh->content(), victim_content));
  }

  size_t idx = 0;
  auto flood_content = make_content(99, kDocSize);
  std::string b_key =
      flood_until_wrap(*cache, idx, 1, flood_content, victim_bucket);
  REQUIRE_FALSE(b_key.empty());

  // ABA precondition probe after wrap #1: V's entry is one phase behind, so
  // the probe skips it — a phase-mismatch miss.  The read path never
  // mutates the directory on a miss (probe_each is const; only insert
  // reclaims stale slots, and no insert visits this bucket), so the entry
  // is still in place for wrap #2.  Leases are off, so this read cannot
  // defer the next wrap.
  {
    auto rh = cache->read_sync(victim_key);
    REQUIRE_FALSE(rh.has_value());
    REQUIRE(rh.error() == CacheError::NotFound);
  }

  std::string c_key =
      flood_until_wrap(*cache, idx, 2, flood_content, victim_bucket);
  REQUIRE_FALSE(c_key.empty());
  REQUIRE(cache->stats().write_buffer_wraps >= 2);
  // Structural check: no insert ever evicted a tag collider out of a full
  // bucket, backing the bucket-disjointness construction (V's bucket holds
  // only V's entry).  next_disjoint_key keeps every flood key out of V's
  // bucket, so V's bucket never reaches four entries and neither full-bucket
  // path — collider eviction nor nearest-to-clobber — can touch V's entry.
  // Only the collider counter is asserted here: bucket_full_evictions is
  // per-volume, not per-bucket, and the flood legitimately fills OTHER
  // buckets, so it is expected to be nonzero.
  REQUIRE(cache->stats().tag_collision_evictions == 0);

  // The ABA moment: V's entry reads current-phase again and points at O,
  // which now holds C's bytes.
  auto victim_read = cache->read_sync(victim_key);
  if (victim_read.has_value()) {
    FAIL("victim key must not resolve to a served document after 2 wraps");
  } else {
    REQUIRE(victim_read.error() == CacheError::NotFound);
  }

  // Sanity: the document actually occupying O reads back correctly, proving
  // the miss above is key-verification at a live offset, not an empty region.
  auto c_read = cache->read_sync(CacheKey(c_key));
  REQUIRE(c_read.has_value());
  REQUIRE(content_equals(c_read->content(), flood_content));

  cache->stop();
  remove_cache_files(path);
}

// --- (B): stale checksum-cache verdict after same-offset reuse -------------
TEST_CASE(
    "Stale checksum-cache verdict after wrap reuse never serves wrong "
    "content",
    "[wrap][checksum]") {
  // Drive the CRC-skip condition deterministically:
  //   1. Write A at offset O and READ it -> caches pack(O, top16(crc(A))).
  //   2. Flood with documents whose payload CRC-32C shares A's top 16 bits
  //      (constructed below) until a wrap lands one of them at O.
  //   3. Read that document: is_checksum_validated(O, crc(new)) returns
  //      true off the STALE verdict, so the new bytes' CRC is skipped.
  // HONESTY NOTE: there is no observable (counter or error) distinguishing
  // "skipped" from "re-verified", so the skip is exercised, not observed;
  // if the cache were invalidated on wrap this test would still pass (it
  // would then simply re-verify).  What IS load-bearing: the served bytes
  // are the new document's real bytes (any stale-verdict shortcut past a
  // wrong/partial document would surface here as a content mismatch), and
  // the replaced key misses cleanly.
  auto content_a = make_content(0, kDocSize);
  uint32_t crc_a = oracle_crc32c(content_a);
  REQUIRE(crc_a != 0);  // read path consults the cache only when checksum != 0

  // Brute-force a DIFFERENT payload whose CRC-32C shares A's top 16 bits
  // (~65536 candidates expected).  The stored checksum covers the payload
  // (empty header + content), so the content CRC is exactly what the read
  // path compares.  Cheap: the prefix CRC state is computed once and only
  // the 4-byte tail is re-hashed per candidate.
  std::vector<std::byte> content_b = make_content(1, kDocSize);
  const uint32_t prefix_state = oracle_crc32c_update(
      0xFFFFFFFFu,
      std::span<const std::byte>(content_b).first(content_b.size() - 4));
  bool collided = false;
  for (uint32_t probe = 0; probe < 40'000'000u && !collided; ++probe) {
    std::array<std::byte, 4> tail = {
        static_cast<std::byte>(probe & 0xFF),
        static_cast<std::byte>((probe >> 8) & 0xFF),
        static_cast<std::byte>((probe >> 16) & 0xFF),
        static_cast<std::byte>((probe >> 24) & 0xFF)};
    uint32_t crc_b = oracle_crc32c_update(prefix_state, tail) ^ 0xFFFFFFFFu;
    if ((crc_b >> 16) == (crc_a >> 16) && crc_b != 0) {
      std::copy(tail.begin(), tail.end(), content_b.end() - 4);
      collided = true;
    }
  }
  REQUIRE(collided);
  REQUIRE((oracle_crc32c(content_b) >> 16) == (crc_a >> 16));
  REQUIRE_FALSE(content_equals(content_b, content_a));

  std::string path = create_temp_file("checksum_reuse", kVolMB);
  auto cache_result = Cache::create(aba_config());
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = path;
  vol_config.size = kVolMB * 1024 * 1024;
  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  // A is the first document -> offset O.  Reading it populates the checksum
  // cache slot for O with A's verdict.
  CacheKey key_a("checksum-A");
  REQUIRE(write_entry(*cache, "checksum-A", content_a));
  {
    auto rh = cache->read_sync(key_a);
    REQUIRE(rh.has_value());
    REQUIRE(content_equals(rh->content(), content_a));
  }

  // Flood with the colliding payload (distinct keys, identical content, so
  // every candidate for O carries crc(B)) until a wrap reuses O.
  size_t idx = 0;
  std::string reuse_key =
      flood_until_wrap(*cache, idx, 1, content_b, bucket_of(key_a));
  REQUIRE_FALSE(reuse_key.empty());
  REQUIRE(cache->stats().write_buffer_wraps >= 1);

  // Read the document now occupying O: correct bytes despite the stale
  // verdict matching the new checksum's top 16 bits.
  auto reuse_read = cache->read_sync(CacheKey(reuse_key));
  REQUIRE(reuse_read.has_value());
  REQUIRE(content_equals(reuse_read->content(), content_b));

  // The replaced key misses cleanly, never a stale serve off the verdict.
  auto a_after = cache->read_sync(key_a);
  REQUIRE_FALSE(a_after.has_value());
  REQUIRE(a_after.error() == CacheError::NotFound);

  cache->stop();
  remove_cache_files(path);
}

// --- (C): DEMONSTRATOR — trailing-gap survivor vs ungated forward fill -----
TEST_CASE(
    "Phase-ABA positional guard rejects a two-wrap trailing-gap survivor "
    "before the ungated forward fill can tear a borrow",
    "[wrap][forwardfill][lease]") {
  // Deterministic construction (all sizes derived from the measured data
  // area D; fillers use one size b, the gap doc J uses g = D mod b, so
  // every pass tiles identically and the last g bytes are the trailing gap
  // no size-b pass ever fills):
  //
  //   pass 0 (P0): n fillers of size b, then J at X = n*b sized to leave
  //                only 4KB of slack before D (the read path maps
  //                approx_size bytes — block-rounded UP by as much as
  //                4KB-1 — and a mapping past the end of the file fails,
  //                which would turn J's read into a miss), so the NEXT
  //                size-b write still wraps.
  //   pass 1 (P1): wrap #1; n fillers tile [0, X); [X, D) untouched; the
  //                next size-b write wraps again.
  //   pass 2 (P0): wrap #2; ONE filler lands at 0.  Phase is back to P0:
  //                J's cold-bucket entry (all other keys bucket-disjoint)
  //                reads current-phase, its bytes at X are INTACT, and X
  //                sits far AHEAD of write_pos.
  //
  // Every read/CRC gauntlet leg passes legitimately for J after the two wraps
  // (real bytes, matching key, valid CRC) and the lease revalidation would
  // pass too (no wrap since probe start) — so ONLY the positional guard can
  // reject J.  With the guard in place, the post-two-wrap read is a clean miss
  // BY POSITION (J's offset X is at/ahead of the write cursor), no borrow is
  // handed out, and the subsequent ungated forward fill across X (plain in-pass
  // reservations + pwrites, no wrap, no lease/epoch machinery) has nothing to
  // tear.  The LONG lease (600s) is kept only to prove the rejection does not
  // depend on the lease: it is the position, not the lease, that rejects J.
  std::string path = create_temp_file("forward_fill", kVolMB);

  CacheConfig config;
  config.ram_cache_size = 0;
  config.enable_checksum = true;
  config.verify_checksum_on_read = true;
  // LONG lease: the point is that the forward fill ignores it entirely.
  config.read_lease_duration = std::chrono::milliseconds(600000);
  config.lease_wrap_ceiling = std::chrono::milliseconds(600000);
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = path;
  vol_config.size = kVolMB * 1024 * 1024;
  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  // Measure the data area: before any write, current_bytes is the
  // directory area (write_pos == data_offset), so D = stripe_bytes -
  // current_bytes.  Single stripe by auto-sizing (4MB < 32MB granularity).
  auto s0 = cache->stats();
  REQUIRE(s0.stripe_count == 1);
  REQUIRE(s0.stripe_bytes > s0.current_bytes);
  const uint64_t data_area = s0.stripe_bytes - s0.current_bytes;

  // Filler doc size b: pick so the trailing gap g = D mod b comfortably
  // holds a document (header 132 bytes + content) plus the end slack.
  constexpr uint64_t kHeaderSize = 132;  // Document::kHeaderSize
  constexpr uint64_t kEndSlack = 4096;   // >= max approx_size overshoot
  uint64_t b = uint64_t{64} * 1024;
  while (b > uint64_t{16} * 1024 &&
         data_area % b < kEndSlack + kHeaderSize + 4096) {
    b -= 4096;
  }
  const uint64_t gap = data_area % b;
  REQUIRE(gap >= kEndSlack + kHeaderSize + 4096);
  const uint64_t n = data_area / b;
  REQUIRE(n >= 4);

  const CacheKey j_key("gap-survivor-J");
  const uint32_t j_bucket = bucket_of(j_key);
  const auto filler = make_content(3, b - kHeaderSize);
  // J and the overwriter both occupy [X, D - 4KB): small enough that the
  // remaining 4KB cannot host another size-b filler (the next one wraps),
  // large enough that the tear is unmistakable.
  const auto j_content = make_content(11, gap - kEndSlack - kHeaderSize);
  const auto overwriter_content =
      make_content(17, gap - kEndSlack - kHeaderSize);

  size_t idx = 0;
  // Pass 0: n fillers tile [0, X), then J fills [X, D - 512).
  for (uint64_t i = 0; i < n; ++i) {
    REQUIRE(
        write_entry(*cache, next_disjoint_key("c-", idx, j_bucket), filler));
  }
  REQUIRE(cache->stats().write_buffer_wraps == 0);
  REQUIRE(write_entry(*cache, "gap-survivor-J", j_content));
  REQUIRE(cache->stats().write_buffer_wraps == 0);  // J fit inside the gap

  // Baseline: J reads back correctly pre-wrap.  The handle is closed before
  // the floods, so the stamped lease alone cannot defer wraps (the gate
  // requires an OUTSTANDING borrow as well).
  {
    auto rh0 = cache->read_sync(j_key);
    CAPTURE(data_area, b, gap, n);
    if (!rh0.has_value()) {
      UNSCOPED_INFO("baseline error=" << static_cast<int>(rh0.error()));
    }
    REQUIRE(rh0.has_value());
    REQUIRE(content_equals(rh0->content(), j_content));
  }

  // Pass 1: wrap #1 (phase -> P1), n fillers tile [0, X); gap untouched.
  for (uint64_t i = 0; i < n; ++i) {
    REQUIRE(
        write_entry(*cache, next_disjoint_key("c-", idx, j_bucket), filler));
  }
  REQUIRE(cache->stats().write_buffer_wraps == 1);

  // Pass 2: wrap #2 (phase -> P0), one filler at offset 0.
  REQUIRE(write_entry(*cache, next_disjoint_key("c-", idx, j_bucket), filler));
  REQUIRE(cache->stats().write_buffer_wraps == 2);
  REQUIRE(cache->stats().writes_dropped_by_lease == 0);

  // THE INVARIANT (restored): J's surviving entry is current-phase again and
  // its bytes at X are intact, but X sits AHEAD of the write cursor.  The
  // positional guard rejects the entry BY POSITION before any borrow — the
  // full-key + CRC gauntlet cannot, since the bytes really are J's.  A clean
  // miss, and crucially NO borrow is outstanding: the ungated forward fill has
  // nothing to tear.  Rejection is by design, not by an approx_size map
  // overrun (J was constructed to map cleanly — the pre-wrap baseline above
  // proved it reads back), so this fails closed regardless of end-slack luck.
  CAPTURE(data_area, b, gap, n);
  {
    auto rh = cache->read_sync(j_key);
    if (rh.has_value()) {
      UNSCOPED_INFO(
          "post-wrap-2 unexpectedly served J (positional guard "
          "did not reject the ahead-of-cursor survivor)");
    }
    REQUIRE_FALSE(rh.has_value());
    REQUIRE(rh.error() == CacheError::NotFound);
  }
  REQUIRE(cache->stats().borrows_outstanding == 0);  // no borrow was handed out

  // Advance write_pos to EXACTLY X: n-1 fillers land the cursor on J's own
  // offset.  This is the >= boundary — offset == cursor — where the bytes are
  // still intact and the overwriter is the very next reservation.  The guard
  // must reject here too (>=, not >), or the imminent write at X would tear.
  for (uint64_t i = 0; i < n - 1; ++i) {
    REQUIRE(
        write_entry(*cache, next_disjoint_key("c-", idx, j_bucket), filler));
  }
  {
    auto rh_boundary = cache->read_sync(j_key);
    REQUIRE_FALSE(rh_boundary.has_value());  // offset == cursor -> rejected
    REQUIRE(rh_boundary.error() == CacheError::NotFound);
  }
  REQUIRE(cache->stats().borrows_outstanding == 0);

  // The overwriter (same size as J) now lands AT X.  No wrap; the lease gate
  // is never consulted — and needn't be, because the survivor was already
  // rejected upstream.
  const std::string ow_key = next_disjoint_key("ow-", idx, j_bucket);
  REQUIRE(write_entry(*cache, ow_key, overwriter_content));
  REQUIRE(cache->stats().write_buffer_wraps == 2);       // no wrap event
  REQUIRE(cache->stats().writes_dropped_by_lease == 0);  // no gate ran
  REQUIRE(cache->stats().borrows_outstanding == 0);      // still no borrow

  // J still misses after the fill (now the full-key gauntlet would also reject:
  // X holds the overwriter's bytes) and the overwriter serves its OWN bytes,
  // proving X is a live, correctly readable region — only J is rejected.
  {
    auto j_after = cache->read_sync(j_key);
    REQUIRE_FALSE(j_after.has_value());
    REQUIRE(j_after.error() == CacheError::NotFound);
  }
  {
    auto ow_read = cache->read_sync(CacheKey(ow_key));
    REQUIRE(ow_read.has_value());
    REQUIRE(content_equals(ow_read->content(), overwriter_content));
  }

  cache->stop();
  remove_cache_files(path);
}

// --- (D): MP variant — positional guard reads the SHARED write cursor -------
TEST_CASE(
    "Cross-view phase-ABA positional guard rejects an ahead-of-cursor "
    "survivor via the shared write cursor, not a stale local one",
    "[wrap][forwardfill][multiprocess]") {
  // Two live Cache views on one file mirror the cross-process topology (as in
  // the lease multiprocess tests): the writer view OWNS the single stripe;
  // the reader view owns nothing, so its process-local write_pos is frozen at
  // data_offset and only the shared cursor is authoritative.  This is exactly
  // the case approach (A) must read SHARED, not local:
  //   * LEG 1 (would break under a local read): a legit doc that lands BEHIND
  //     the shared cursor must still be SERVED on the reader view.  A stale-low
  //     local cursor (== data_offset) would reject every non-first document,
  //     so this read succeeding proves the guard consults the shared cursor.
  //   * LEG 2: the two-wrap trailing-gap survivor J, current-phase again and
  //     AHEAD of the shared cursor, must MISS on the reader view.
  std::string path = create_temp_file("forward_fill_mp", kVolMB);

  auto make_view = [&](uint32_t process_index) {
    CacheConfig config;
    config.ram_cache_size = 0;
    config.set_enable_checksum(true);  // required in multi-process mode
    config.set_multi_process(process_index, 2);
    // Leases off: wraps must not defer while the phase machinery is exercised.
    config.read_lease_duration = std::chrono::milliseconds(0);
    config.lease_wrap_ceiling = std::chrono::milliseconds(0);
    auto cache_result = Cache::create(config);
    REQUIRE(cache_result.has_value());
    VolumeConfig vol_config;
    vol_config.path = path;
    vol_config.size = kVolMB * 1024 * 1024;
    REQUIRE((*cache_result)->add_volume(vol_config).has_value());
    REQUIRE((*cache_result)->start().has_value());
    return std::move(*cache_result);
  };

  auto writer_view = make_view(0);  // owns the single stripe
  auto reader_view = make_view(1);  // owns nothing: lock-free reads
  REQUIRE(writer_view->stats().stripe_count == 1);

  const CacheKey j_key("gap-survivor-J");
  const uint32_t j_bucket = bucket_of(j_key);

  // Writer drives the two-wrap trailing-gap survivor.  After this the shared
  // cursor sits near data-area start (pass 2) while J's entry is current-phase
  // at offset X = n*b far ahead of it.
  GapSurvivor g = build_two_wrap_gap_survivor(*writer_view);

  // LEG 1: a legit doc BEHIND the shared cursor, read back on the reader view.
  const auto legit_content = make_content(41, 4096);
  const std::string legit_key = next_disjoint_key("legit-", g.idx, j_bucket);
  REQUIRE(write_entry(*writer_view, legit_key, legit_content));
  {
    auto rl = reader_view->read_sync(CacheKey(legit_key));
    REQUIRE(rl.has_value());  // fails if the guard read the stale local cursor
    REQUIRE_FALSE(rl->is_ram_cache_hit());
    REQUIRE(content_equals(rl->content(), legit_content));
  }

  // LEG 2: the ahead-of-cursor survivor misses on the reader view.
  {
    auto rj = reader_view->read_sync(j_key);
    CAPTURE(g.data_area, g.b, g.gap, g.n);
    REQUIRE_FALSE(rj.has_value());
    REQUIRE(rj.error() == CacheError::NotFound);
  }
  REQUIRE(reader_view->stats().borrows_outstanding == 0);

  reader_view->stop();
  writer_view->stop();
  remove_cache_files(path);
}

// --- (E): ONE-wrap dark chain tail vs the chain-hop guard leg ---------------
TEST_CASE(
    "Chain-hop positional guard rejects a one-wrap dark tail before it is "
    "enumerated, selected, or borrowed",
    "[wrap][forwardfill][alternate]") {
  // The chain-hop bypass: hops consume next_alternate_offset values from
  // DOCUMENT HEADERS, not the directory, so the probe-leg guard never sees
  // them — and a SAME-KEY dark node passes both bounds validation and the
  // walks' cross-key guard.  Reachable with ONE wrap (no phase ABA needed):
  //
  //   1. n-1 fillers of size b tile [0, X); K's first alternate H (a small
  //      doc) lands at X.  One filler-slot plus the tail gap remain after H
  //      — enough that the chain walk's 64KB+header initial map from X stays
  //      inside the file (REQUIREd below), but NOT enough for the oversized
  //      second alternate.
  //   2. K's SECOND alternate (128KB) does not fit: commit_alternate_write
  //      probes old head H (behind the pre-wrap cursor — passes), stamps
  //      next = X into the new document, then allocate_write_slot WRAPS and
  //      lands new head L at the data-area start.  The directory repoints
  //      K -> L.
  //
  // Now L (behind cursor).next -> H: an intact, ahead-of-cursor, same-key
  // node no phase toggle ever touches.  Without the hop guard, every chain
  // walk enumerates H, a selector can pick it, and a borrow lands on bytes
  // the ordinary forward fill overwrites with no wrap event — the one-wrap
  // variant of the tear.  With the guard, the hop L -> H is positionally
  // rejected: reads of K serve L's own bytes or miss, NEVER H's, and
  // list_alternates never describes H.
  std::string path = create_temp_file("dark_tail", kVolMB);
  auto cache_result = Cache::create(aba_config());
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = path;
  vol_config.size = kVolMB * 1024 * 1024;
  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  // Geometry: D, filler size b, trailing gap g = D mod b.
  auto s0 = cache->stats();
  REQUIRE(s0.stripe_count == 1);
  const uint64_t data_area = s0.stripe_bytes - s0.current_bytes;
  constexpr uint64_t kHeaderSize = 132;  // Document::kHeaderSize
  const uint64_t b = uint64_t{64} * 1024;
  const uint64_t gap = data_area % b;
  const uint64_t n = data_area / b;
  REQUIRE(n >= 6);

  const CacheKey k_key("dark-tail-K");
  const uint32_t k_bucket = bucket_of(k_key);
  const auto filler = make_content(3, b - kHeaderSize);
  const auto h_content = make_content(21, 4096);  // H doc = 4228 bytes
  const uint64_t h_doc = kHeaderSize + h_content.size();
  const auto l_content = make_content(33, size_t{128} * 1024);
  const uint64_t l_doc = kHeaderSize + l_content.size();

  // Geometry preconditions, REQUIREd so upstream sizing changes fail loudly:
  //  - the walk's initial map (64KB + header) from X = (n-1)*b must fit in
  //    the remaining b + gap;
  //  - L must NOT fit after H (so its commit wraps).
  REQUIRE(b + gap >= uint64_t{64} * 1024 + kHeaderSize + h_doc);
  REQUIRE(l_doc > b + gap - h_doc);

  size_t idx = 0;
  for (uint64_t i = 0; i + 1 < n; ++i) {
    REQUIRE(
        write_entry(*cache, next_disjoint_key("c-", idx, k_bucket), filler));
  }
  REQUIRE(cache->stats().write_buffer_wraps == 0);

  // H: K's first alternate, head of the chain, at X = (n-1)*b.
  REQUIRE(write_alternate_entry(*cache, "dark-tail-K", AlternateId::Brotli,
                                h_content));
  REQUIRE(cache->stats().write_buffer_wraps == 0);

  // Baseline: H serves while behind the cursor.
  {
    IdSelector want_h(AlternateId::Brotli);
    AlternateSelectionContext ctx;
    auto rh = cache->read_alternate_sync(k_key, want_h, ctx);
    CAPTURE(data_area, b, gap, n);
    REQUIRE(rh.has_value());
    REQUIRE(content_equals(rh->content(), h_content));
  }

  // L: K's second alternate.  Doesn't fit in the 4KB slack -> the commit
  // probes H (passes: still behind the pre-wrap cursor), stamps next = X,
  // WRAPS, and lands L at the data-area start.  H is now a dark tail.
  REQUIRE(write_alternate_entry(*cache, "dark-tail-K", AlternateId::Gzip,
                                l_content));
  REQUIRE(cache->stats().write_buffer_wraps == 1);

  // list_alternates must never describe the dark node: exactly L.
  {
    auto alts = cache->list_alternates_sync(k_key);
    REQUIRE(alts.has_value());
    REQUIRE(alts->size() == 1);
    REQUIRE((*alts)[0].id == AlternateId::Gzip);
    REQUIRE((*alts)[0].content_length == l_content.size());
  }

  // A read aimed at H must MISS (hop rejected -> selector never sees it) —
  // never serve H's bytes through a tearable borrow.
  {
    IdSelector want_h(AlternateId::Brotli);
    AlternateSelectionContext ctx;
    auto rh = cache->read_alternate_sync(k_key, want_h, ctx);
    REQUIRE_FALSE(rh.has_value());
    REQUIRE(rh.error() == CacheError::AlternateNotFound);
  }
  REQUIRE(cache->stats().borrows_outstanding == 0);

  // The head L itself serves normally (the guard rejects the hop, not the
  // live head).
  {
    DefaultStorageSelector first;
    AlternateSelectionContext ctx;
    auto rh = cache->read_alternate_sync(k_key, first, ctx);
    REQUIRE(rh.has_value());
    REQUIRE(content_equals(rh->content(), l_content));
    REQUIRE_FALSE(content_equals(rh->content(), h_content));
  }

  // Forward fill across X: after the wrap the cursor sits at l_doc (~2b), so
  // n-3 fillers advance it to X + kHeaderSize — the last one overwrites the
  // dark node's header IN PLACE with no wrap event, exactly the write the
  // rejection above made safe.  K's reads are unchanged: L serves, H gone.
  for (uint64_t i = 0; i + 3 < n; ++i) {
    REQUIRE(
        write_entry(*cache, next_disjoint_key("c-", idx, k_bucket), filler));
  }
  REQUIRE(cache->stats().write_buffer_wraps == 1);  // no wrap event
  {
    auto alts = cache->list_alternates_sync(k_key);
    REQUIRE(alts.has_value());
    REQUIRE(alts->size() == 1);
    REQUIRE((*alts)[0].id == AlternateId::Gzip);
    DefaultStorageSelector first;
    AlternateSelectionContext ctx;
    auto rh = cache->read_alternate_sync(k_key, first, ctx);
    REQUIRE(rh.has_value());
    REQUIRE(content_equals(rh->content(), l_content));
  }

  cache->stop();
  remove_cache_files(path);
}

// --- (G): a wrapping write REFUSES to link its new head to the pre-wrap chain
TEST_CASE(
    "An alternate write whose allocation wraps starts a fresh chain instead of "
    "linking across the wrap frontier",
    "[wrap][forwardfill][alternate]") {
  // (E) above shows the hazard from the READ side: a head committed across a
  // wrap keeps next_alternate_offset pointing at a pre-wrap node, and the
  // positional hop guard is the only thing keeping walks out of it — a guard
  // that REOPENS once the cursor passes that offset again.  This case closes
  // the hazard at its source: the link is never created.
  //
  // The write samples the wrap epoch BEFORE it resolves the old head, and
  // re-checks it after allocating.  A moved epoch means a wrap happened — the
  // only way behind-cursor bytes get reused — so every offset resolved during
  // the walk may now name bytes the ring is refilling.  The new document's
  // next field is zeroed in the buffer before the fill: the write starts a
  // fresh chain and the pre-wrap nodes are orphaned, which is the honest
  // outcome given the ring has already begun overwriting them.
  //
  // This also pins the interaction with the superseded-alternate unlink: that
  // mechanism would have pointed the new head at an OLDER node than today's
  // behaviour does (the first thing a wrap recycles), so it strictly widens
  // this window.  The refusal is what closes it, and the two ship together —
  // note it is NOT gated by the unlink kill switch.
  std::string path = create_temp_file("wrap_refuse_link", kVolMB);
  auto cache_result = Cache::create(aba_config());
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = path;
  vol_config.size = kVolMB * 1024 * 1024;
  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  auto s0 = cache->stats();
  REQUIRE(s0.stripe_count == 1);
  const uint64_t data_area = s0.stripe_bytes - s0.current_bytes;
  constexpr uint64_t kHeaderSize = 132;  // Document::kHeaderSize
  const uint64_t b = uint64_t{64} * 1024;

  const CacheKey k_key("wrap-refuse-K");
  const uint32_t k_bucket = bucket_of(k_key);
  const auto filler = make_content(3, b - kHeaderSize);
  const auto keeper = make_content(21, 4096);    // Brotli: a DISTINCT id
  const auto old_head = make_content(22, 4096);  // Original v1
  const auto new_head = make_content(23, size_t{128} * 1024);  // Original v2
  const uint64_t small_doc = kHeaderSize + 4096;
  const uint64_t big_doc = kHeaderSize + new_head.size();

  // The two small alternates go in FIRST, at the data-area start, so the wrap
  // that lands the big document recycles the very bytes the old chain
  // occupies — the exact hazard, not an approximation of it.
  REQUIRE(write_alternate_entry(*cache, "wrap-refuse-K", AlternateId::Brotli,
                                keeper));
  REQUIRE(write_alternate_entry(*cache, "wrap-refuse-K", AlternateId::Original,
                                old_head));
  REQUIRE(cache->stats().write_buffer_wraps == 0);
  {
    auto alts = cache->list_alternates_sync(k_key);
    REQUIRE(alts.has_value());
    REQUIRE(alts->size() == 2);  // [Original v1, Brotli]
  }

  // Fill the rest of the stripe with bucket-disjoint fillers, leaving less
  // than one filler slot -- so the 128KB document below cannot fit and its
  // allocation must wrap.  (b + trailing slack < big_doc always holds here:
  // the slack is under one 64KB filler and big_doc is 128KB + a header.)
  const uint64_t remaining_after_smalls = data_area - 2 * small_doc;
  const uint64_t fillers = remaining_after_smalls / b;
  REQUIRE(fillers >= 4);
  REQUIRE(big_doc > remaining_after_smalls - fillers * b);
  size_t idx = 0;
  for (uint64_t i = 0; i < fillers; ++i) {
    REQUIRE(
        write_entry(*cache, next_disjoint_key("w-", idx, k_bucket), filler));
  }
  REQUIRE(cache->stats().write_buffer_wraps == 0);

  // Re-record Original.  Its allocation wraps.
  REQUIRE(write_alternate_entry(*cache, "wrap-refuse-K", AlternateId::Original,
                                new_head));
  REQUIRE(cache->stats().write_buffer_wraps == 1);

  // The new head is alone in its chain: the pre-wrap Brotli is orphaned, not
  // linked-then-guarded.
  auto alts = cache->list_alternates_sync(k_key);
  REQUIRE(alts.has_value());
  REQUIRE(alts->size() == 1);
  REQUIRE((*alts)[0].id == AlternateId::Original);
  {
    DefaultStorageSelector first;
    AlternateSelectionContext ctx;
    auto rh = cache->read_alternate_sync(k_key, first, ctx);
    REQUIRE(rh.has_value());
    REQUIRE(content_equals(rh->content(), new_head));
  }
  {
    IdSelector want_keeper(AlternateId::Brotli);
    AlternateSelectionContext ctx;
    auto rk = cache->read_alternate_sync(k_key, want_keeper, ctx);
    REQUIRE_FALSE(rk.has_value());
  }

  // The splice was abandoned too: no in-place store may run against offsets
  // resolved before a wrap.  Neither shadow counter moves — the refusal
  // detached the whole old chain, so nothing was left linked and nothing was
  // spliced.  The refusal itself is counted: without it this case
  // would be indistinguishable from an ordinary wrap in stats.
  auto st = cache->stats();
  REQUIRE(st.alternate_shadows_unlinked == 0);
  REQUIRE(st.alternate_splice_deferred == 0);
  REQUIRE(st.alternate_wrap_refusals == 1);

#ifndef _WIN32
  // Direct evidence, and the only thing that distinguishes the refusal from
  // the read-side hop guard doing the work: the link is ZERO on disk.  Without
  // the refusal this field would hold the pre-wrap Brotli's offset.
  {
    const auto files = cache->volume_files();
    REQUIRE(files.size() >= 1);
    const int fd = ::open(files[0].file_path.c_str(), O_RDONLY);
    REQUIRE(fd >= 0);
    uint64_t link = 0xdeadbeef;
    const ssize_t n = ::pread(
        fd, &link, sizeof(link),
        static_cast<off_t>((*alts)[0].disk_offset + 112));  // next_alt_offset
    ::close(fd);
    REQUIRE(n == static_cast<ssize_t>(sizeof(link)));
    REQUIRE(link == 0);
  }
#endif

  cache->stop();
  remove_cache_files(path);
}

// --- (F): reopen persistence of the shared write cursor ---------------------
TEST_CASE(
    "Shared write cursor persists across reopen: behind-cursor entries still "
    "serve after a wrapped volume is closed and reopened",
    "[wrap][multiprocess]") {
  // The positional guard compares entry offsets against shared_write_pos on
  // mmap-directory stripes.  Volume::init_stripes RECOVERS that cursor from
  // the shared header on reopen; if it were wiped (or misread as relative),
  // every persisted entry would sit at/ahead of a data_offset-reset cursor
  // and the guard would reject the whole volume.  Pin the behavior: wrap an
  // MP-directory volume, write a probe doc (behind cursor), close, reopen,
  // and require the probe doc still serves with intact bytes.
  std::string path = create_temp_file("reopen_cursor", kVolMB);

  auto make_cache = [&]() {
    CacheConfig config;
    config.ram_cache_size = 0;
    config.set_enable_checksum(true);  // required in multi-process mode
    config.set_multi_process(0, 1);    // mmap (shared) directories
    config.read_lease_duration = std::chrono::milliseconds(0);
    config.lease_wrap_ceiling = std::chrono::milliseconds(0);
    auto cache_result = Cache::create(config);
    REQUIRE(cache_result.has_value());
    VolumeConfig vol_config;
    vol_config.path = path;
    vol_config.size = kVolMB * 1024 * 1024;
    REQUIRE((*cache_result)->add_volume(vol_config).has_value());
    REQUIRE((*cache_result)->start().has_value());
    return std::move(*cache_result);
  };

  const CacheKey probe_key("reopen-probe");
  const uint32_t probe_bucket = bucket_of(probe_key);
  const auto probe_content = make_content(29, kDocSize);
  const auto flood_content = make_content(99, kDocSize);

  {
    auto cache = make_cache();
    REQUIRE(cache->stats().stripe_count == 1);
    size_t idx = 0;
    std::string trigger =
        flood_until_wrap(*cache, idx, 1, flood_content, probe_bucket);
    REQUIRE_FALSE(trigger.empty());
    REQUIRE(cache->stats().write_buffer_wraps >= 1);
    // Probe doc lands just after the wrap: behind the cursor, current phase.
    REQUIRE(write_entry(*cache, "reopen-probe", probe_content));
    auto rh = cache->read_sync(probe_key);
    REQUIRE(rh.has_value());
    REQUIRE(content_equals(rh->content(), probe_content));
    cache->stop();
  }

  // Reopen: the recovered shared cursor must still classify the persisted
  // entry as behind-cursor (live), so it serves.
  {
    auto cache = make_cache();
    auto rh = cache->read_sync(probe_key);
    if (!rh.has_value()) {
      UNSCOPED_INFO("reopen error=" << static_cast<int>(rh.error()));
    }
    REQUIRE(rh.has_value());
    REQUIRE(content_equals(rh->content(), probe_content));
    cache->stop();
  }

  remove_cache_files(path);
}

// ===========================================================================
// F6: close the reservation-to-pwrite tear window (hold the write lock across
// the pwrite; advance the guard-visible cursor only after the fill lands).
//
// These cases reuse the two-wrap trailing-gap survivor machinery above and add
// a TEST-ONLY gate seam (Volume::s_write_tear_gate_for_test) that freezes a
// writer at the reservation->pwrite boundary — the exact µs window F6 closes.
// The pre-fix behaviour is reproduced on demand via
// Volume::s_advance_cursor_at_reservation_for_test (advance the cursor + drop
// the lock AT reservation), mirroring the presume-dead seam the write-lock
// lifetime tests use.  Each fixed-vs-pre-fix pair asserts BOTH: the fix yields
// a clean miss, the pre-fix seam yields a served-then-torn zero-copy borrow.
// ===========================================================================
namespace {

// Cross-thread rendezvous used by the tear-gate hook: the writer thread calls
// enter() inside the tear window (bytes reserved, cursor NOT advanced, and in
// MP mode the write lock STILL HELD) and blocks until the test releases it.
struct TearGate {
  std::mutex m;
  std::condition_variable cv;
  bool at_gate = false;
  bool released = false;
  uint64_t seen_write_offset = 0;
  uint64_t seen_new_write_pos = 0;

  void enter(uint64_t wo, uint64_t np) {
    std::unique_lock<std::mutex> lk(m);
    seen_write_offset = wo;
    seen_new_write_pos = np;
    at_gate = true;
    cv.notify_all();
    cv.wait(lk, [&] { return released; });
  }
  void wait_until_at_gate() {
    std::unique_lock<std::mutex> lk(m);
    cv.wait(lk, [&] { return at_gate; });
  }
  void release() {
    std::unique_lock<std::mutex> lk(m);
    released = true;
    cv.notify_all();
  }
};

// Advance the single-stripe write cursor to EXACTLY J's offset X with (n-1)
// bucket-disjoint size-b fillers (mirrors TEST (C)), so the NEXT forward fill
// reserves a span that starts at X and therefore CONTAINS J's bytes.
void advance_cursor_to_survivor(Cache &cache, GapSurvivor &g,
                                std::span<const std::byte> filler,
                                uint32_t j_bucket) {
  for (uint64_t i = 0; i < g.n - 1; ++i) {
    REQUIRE(
        write_entry(cache, next_disjoint_key("c-", g.idx, j_bucket), filler));
  }
  REQUIRE(cache.stats().write_buffer_wraps == 2);
}

}  // namespace

// --- (F6-A): single-process reservation->pwrite window ----------------------
TEST_CASE(
    "F6-A: a phase-ABA survivor inside a reserved-but-unfilled span stays a "
    "clean miss (single-process); the pre-fix seam serves it then tears it",
    "[wrap][forwardfill][f6][writelock]") {
  constexpr uint64_t kHeaderSize = 132;
  constexpr uint64_t kEndSlack = 4096;

  auto scenario = [&](bool prefix_mode) {
    Volume::s_advance_cursor_at_reservation_for_test.store(prefix_mode);
    std::string path =
        create_temp_file(prefix_mode ? "f6a_prefix" : "f6a_fixed", kVolMB);

    CacheConfig config;
    config.ram_cache_size = 0;
    config.enable_checksum = true;
    config.verify_checksum_on_read = true;
    // LONG lease: the forward fill ignores it entirely — only the positional
    // guard, fed by the held-lock cursor discipline, can reject J.
    config.read_lease_duration = std::chrono::milliseconds(600000);
    config.lease_wrap_ceiling = std::chrono::milliseconds(600000);
    auto cache_result = Cache::create(config);
    REQUIRE(cache_result.has_value());
    auto &cache = *cache_result;
    VolumeConfig vc;
    vc.path = path;
    vc.size = kVolMB * 1024 * 1024;
    REQUIRE(cache->add_volume(vc).has_value());
    REQUIRE(cache->start().has_value());

    GapSurvivor g = build_two_wrap_gap_survivor(*cache);
    const CacheKey j_key("gap-survivor-J");
    const uint32_t j_bucket = bucket_of(j_key);
    const auto filler = make_content(3, g.b - kHeaderSize);
    const auto j_content = make_content(11, g.gap - kEndSlack - kHeaderSize);
    const auto overwriter_content =
        make_content(17, g.gap - kEndSlack - kHeaderSize);

    advance_cursor_to_survivor(*cache, g, filler, j_bucket);

    // Steady state (no writer in flight): J is a survivor the positional guard
    // already rejects in BOTH builds — the tear window is what re-admits it
    // pre-fix.
    REQUIRE_FALSE(cache->read_sync(j_key).has_value());
    REQUIRE(cache->stats().borrows_outstanding == 0);

    TearGate gate;
    Volume::s_write_tear_gate_for_test = [&gate](uint64_t wo, uint64_t np) {
      gate.enter(wo, np);
    };

    const std::string ow_key = next_disjoint_key("ow-", g.idx, j_bucket);
    std::atomic<bool> writer_ok{false};
    std::thread writer([&] {
      writer_ok.store(write_entry(*cache, ow_key, overwriter_content));
    });

    gate.wait_until_at_gate();
    // The writer is frozen INSIDE the tear window: [write_offset,
    // new_write_pos) is reserved (starting AT J's offset X) and its pwrite has
    // not run.
    REQUIRE(gate.seen_new_write_pos > gate.seen_write_offset);

    std::optional<ReadHandle> held;
    if (!prefix_mode) {
      // FIXED: the guard-visible cursor did NOT advance, so J is still at/ahead
      // of it — a clean miss, and crucially NO borrow to tear.
      auto rj = cache->read_sync(j_key);
      REQUIRE_FALSE(rj.has_value());
      REQUIRE(rj.error() == CacheError::NotFound);
      REQUIRE(cache->stats().borrows_outstanding == 0);
    } else if (CacheConfig{}.wrap_retention) {
      // With wrap retention the PASS STAMP is a second, independent layer: J
      // carries pass 0 while the snapshot is in pass 2, so even the pre-fix
      // seam's advanced cursor cannot get it admitted (the position leg
      // passes, the stamp leg rejects).  Nothing to tear.
      auto rj = cache->read_sync(j_key);
      REQUIRE_FALSE(rj.has_value());
      REQUIRE(cache->stats().stamp_rejections > 0);
      REQUIRE(cache->stats().borrows_outstanding == 0);
    } else {
      // PRE-FIX DEMONSTRATOR: the cursor advanced past X at reservation, so the
      // survivor is re-admitted mid-window and a zero-copy borrow is handed out
      // over bytes the imminent pwrite will overwrite.
      auto rj = cache->read_sync(j_key);
      REQUIRE(rj.has_value());  // the bug: served inside the window
      REQUIRE(content_equals(rj->content(), j_content));  // still intact...
      held.emplace(std::move(*rj));
      REQUIRE(cache->stats().borrows_outstanding >= 1);
    }

    gate.release();
    writer.join();
    Volume::s_write_tear_gate_for_test = {};
    REQUIRE(writer_ok.load());

    if (held.has_value()) {
      // ...the pwrite has now landed the overwriter INTO the borrowed bytes:
      // the still-open zero-copy borrow reads the overwriter's bytes — TORN.
      REQUIRE_FALSE(content_equals(held->content(), j_content));
      REQUIRE(content_equals(held->content(), overwriter_content));
      held.reset();
    } else {
      // FIXED: J still misses; the overwriter serves ITS OWN bytes at X,
      // proving the region is live and correctly readable — only J is rejected.
      REQUIRE_FALSE(cache->read_sync(j_key).has_value());
      {
        auto ow = cache->read_sync(CacheKey(ow_key));
        REQUIRE(ow.has_value());
        REQUIRE(content_equals(ow->content(), overwriter_content));
      }  // close the overwriter borrow before the outstanding-count check
      REQUIRE(cache->stats().borrows_outstanding == 0);
    }

    cache->stop();
    remove_cache_files(path);
    Volume::s_advance_cursor_at_reservation_for_test.store(false);
  };

  SECTION("fixed protocol: clean miss inside the tear window") {
    scenario(false);
  }
  SECTION("pre-fix seam: served-then-torn borrow (the closed bug)") {
    scenario(true);
  }
}

// --- (F6-B): multi-process (cross-view) reservation->pwrite window -----------
TEST_CASE(
    "F6-B: a non-owning reader view misses an ahead-of-cursor survivor while "
    "the writer view is frozen mid-pwrite holding the write lock; the pre-fix "
    "seam serves it then tears it across views",
    "[wrap][forwardfill][multiprocess][f6][writelock]") {
  constexpr uint64_t kHeaderSize = 132;
  constexpr uint64_t kEndSlack = 4096;

  auto scenario = [&](bool prefix_mode) {
    Volume::s_advance_cursor_at_reservation_for_test.store(prefix_mode);
    std::string path =
        create_temp_file(prefix_mode ? "f6b_prefix" : "f6b_fixed", kVolMB);

    auto make_view = [&](uint32_t process_index) {
      CacheConfig config;
      config.ram_cache_size = 0;
      config.set_enable_checksum(true);  // required in multi-process mode
      config.set_multi_process(process_index, 2);
      config.read_lease_duration = std::chrono::milliseconds(600000);
      config.lease_wrap_ceiling = std::chrono::milliseconds(600000);
      auto cache_result = Cache::create(config);
      REQUIRE(cache_result.has_value());
      VolumeConfig vc;
      vc.path = path;
      vc.size = kVolMB * 1024 * 1024;
      REQUIRE((*cache_result)->add_volume(vc).has_value());
      REQUIRE((*cache_result)->start().has_value());
      return std::move(*cache_result);
    };

    auto writer_view = make_view(0);  // owns the single stripe
    auto reader_view = make_view(1);  // owns nothing: lock-free reads
    REQUIRE(writer_view->stats().stripe_count == 1);

    GapSurvivor g = build_two_wrap_gap_survivor(*writer_view);
    const CacheKey j_key("gap-survivor-J");
    const uint32_t j_bucket = bucket_of(j_key);
    const auto filler = make_content(3, g.b - kHeaderSize);
    const auto j_content = make_content(11, g.gap - kEndSlack - kHeaderSize);
    const auto overwriter_content =
        make_content(17, g.gap - kEndSlack - kHeaderSize);

    advance_cursor_to_survivor(*writer_view, g, filler, j_bucket);
    // Steady state: the reader view already rejects the ahead-of-shared-cursor
    // survivor.
    REQUIRE_FALSE(reader_view->read_sync(j_key).has_value());

    TearGate gate;
    Volume::s_write_tear_gate_for_test = [&gate](uint64_t wo, uint64_t np) {
      gate.enter(wo, np);
    };

    const std::string ow_key = next_disjoint_key("ow-", g.idx, j_bucket);
    std::atomic<bool> writer_ok{false};
    std::thread writer([&] {
      writer_ok.store(write_entry(*writer_view, ow_key, overwriter_content));
    });

    gate.wait_until_at_gate();

    std::optional<ReadHandle> held;
    if (!prefix_mode) {
      // FIXED: shared_write_pos is un-advanced (the writer holds the write lock
      // across its pwrite), so the reader view sees J at/ahead of the cursor.
      auto rj = reader_view->read_sync(j_key);
      REQUIRE_FALSE(rj.has_value());
      REQUIRE(rj.error() == CacheError::NotFound);
      REQUIRE(reader_view->stats().borrows_outstanding == 0);
    } else if (CacheConfig{}.wrap_retention) {
      // Wrap retention: the pass stamp rejects J even past the pre-fix seam's
      // advanced shared cursor (see F6-A).
      auto rj = reader_view->read_sync(j_key);
      REQUIRE_FALSE(rj.has_value());
      REQUIRE(reader_view->stats().stamp_rejections > 0);
    } else {
      // PRE-FIX: shared_write_pos was published past X and the lock dropped at
      // reservation, so the reader view re-admits J and borrows torn-to-be
      // bytes across the process boundary.
      auto rj = reader_view->read_sync(j_key);
      REQUIRE(rj.has_value());
      REQUIRE(content_equals(rj->content(), j_content));
      held.emplace(std::move(*rj));
    }

    gate.release();
    writer.join();
    Volume::s_write_tear_gate_for_test = {};
    REQUIRE(writer_ok.load());

    if (held.has_value()) {
      REQUIRE_FALSE(content_equals(held->content(), j_content));
      REQUIRE(content_equals(held->content(), overwriter_content));
      held.reset();
    } else {
      REQUIRE_FALSE(reader_view->read_sync(j_key).has_value());
      auto ow = reader_view->read_sync(CacheKey(ow_key));
      REQUIRE(ow.has_value());
      REQUIRE(content_equals(ow->content(), overwriter_content));
    }

    reader_view->stop();
    writer_view->stop();
    remove_cache_files(path);
    Volume::s_advance_cursor_at_reservation_for_test.store(false);
  };

  SECTION("fixed protocol: cross-view clean miss") { scenario(false); }
  SECTION("pre-fix seam: cross-view served-then-torn") { scenario(true); }
}

// --- (F6-E): wrap composition — a gated WRAPPING write ----------------------
TEST_CASE(
    "F6-E: freezing a WRAPPING write in the tear window still rejects a "
    "two-wrap survivor (guard + phase-epoch), and the wrap completes without "
    "deadlock once released",
    "[wrap][forwardfill][f6][writelock]") {
  constexpr uint64_t kHeaderSize = 132;
  std::string path = create_temp_file("f6e", kVolMB);

  CacheConfig config;
  config.ram_cache_size = 0;
  config.enable_checksum = true;
  config.verify_checksum_on_read = true;
  // Leases OFF: the gated write must WRAP (a live lease could defer it).
  config.read_lease_duration = std::chrono::milliseconds(0);
  config.lease_wrap_ceiling = std::chrono::milliseconds(0);
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;
  VolumeConfig vc;
  vc.path = path;
  vc.size = kVolMB * 1024 * 1024;
  REQUIRE(cache->add_volume(vc).has_value());
  REQUIRE(cache->start().has_value());

  GapSurvivor g = build_two_wrap_gap_survivor(*cache);
  const CacheKey j_key("gap-survivor-J");
  const uint32_t j_bucket = bucket_of(j_key);
  const auto filler = make_content(3, g.b - kHeaderSize);

  // Advance the cursor to EXACTLY X: the NEXT size-b filler cannot fit in the
  // trailing gap (gap < b) and therefore WRAPS.  That wrapping write is the one
  // we freeze.
  advance_cursor_to_survivor(*cache, g, filler, j_bucket);
  REQUIRE_FALSE(cache->read_sync(j_key).has_value());

  TearGate gate;
  Volume::s_write_tear_gate_for_test = [&gate](uint64_t wo, uint64_t np) {
    gate.enter(wo, np);
  };

  const std::string wrap_key = next_disjoint_key("wrap-", g.idx, j_bucket);
  std::atomic<bool> writer_ok{false};
  std::thread writer(
      [&] { writer_ok.store(write_entry(*cache, wrap_key, filler)); });

  gate.wait_until_at_gate();
  // Inside the window the wrap has already toggled the phase and bumped the
  // epoch (evict_if_needed + record_wrap run at reservation), and the cursor is
  // reset to the data-area start — J is rejected by BOTH the phase check and
  // the positional guard, and the new doc's bytes are not written yet.
  {
    auto rj = cache->read_sync(j_key);
    REQUIRE_FALSE(rj.has_value());
    REQUIRE(rj.error() == CacheError::NotFound);
  }
  REQUIRE(cache->stats().borrows_outstanding == 0);

  gate.release();
  writer.join();
  Volume::s_write_tear_gate_for_test = {};
  REQUIRE(writer_ok.load());  // the wrap completed — no deadlock under the lock
  REQUIRE(cache->stats().write_buffer_wraps == 3);

  // J still misses; the wrapping doc serves its own bytes at the data-area
  // start.
  REQUIRE_FALSE(cache->read_sync(j_key).has_value());
  {
    auto rw = cache->read_sync(CacheKey(wrap_key));
    REQUIRE(rw.has_value());
    REQUIRE(content_equals(rw->content(), filler));
  }

  cache->stop();
  remove_cache_files(path);
}

// --- (F6-E-MP): wrap window vs a stale SHARED cursor -------------------------
// The multi-process counterpart of F6-E, where the guard cursor is the SHARED
// one.  A wrap resets the process-local stripe->write_pos to the data-area
// start S, but a multi-process reader's positional guard reads
// shared_write_pos (Stripe::current_write_cursor).  If the wrap left the shared
// cursor at its pre-wrap HIGH value until commit_write_slot, then for the whole
// wrap's reservation->pwrite window the guard would admit offsets in
// [S, old cursor), including the span the wrapping write is about to fill.
// Pair that with a two-wrap directory survivor whose bytes at S carry the same
// key, and every leg of the gauntlet passes:
//
//   pass 0: K written at S (entry e1, phase p0), then n-1 size-b fillers.
//   pass 1: K written again -> wrap #1 lands it at S.  The in-place election
//           skips e1 (stale phase), so the new entry e2 takes an EMPTY slot
//           and e1 survives.  Then n-1 fillers.
//   wrap #2 (the gated write): the phase toggles back to p0, so e1 is current
//           again and points at S, which still holds K's intact pass-1 bytes
//           (key + CRC verify).  The epoch was bumped and intent cleared
//           BEFORE the tear gate, so a reader that starts now captures the
//           post-wrap epoch and its borrow revalidates cleanly.  Only the
//           positional guard can reject e1, and only if the shared cursor was
//           lowered to S inside the wrap.
//
// Freeze the wrapping writer at the tear gate (bytes reserved at S, pwrite not
// run) and read K from the non-owning view: it must be a clean miss, with no
// borrow handed out over the bytes the pwrite is about to overwrite.
TEST_CASE(
    "F6-E in mmap mode: wrap window with stale shared cursor must not admit a "
    "two-wrap survivor at the wrap target",
    "[wrap][forwardfill][multiprocess][f6][writelock]") {
  constexpr uint64_t kHeaderSize = 132;  // Document::kHeaderSize
  std::string path = create_temp_file("f6e_mp", kVolMB);

  auto make_view = [&](uint32_t process_index) {
    CacheConfig config;
    config.ram_cache_size = 0;
    config.set_enable_checksum(true);  // required in multi-process mode
    config.set_multi_process(process_index, 2);
    // Long lease so borrows_outstanding counts real borrows.  No borrow is
    // held across any wrap below, so the gate never defers.
    config.read_lease_duration = std::chrono::milliseconds(600000);
    config.lease_wrap_ceiling = std::chrono::milliseconds(600000);
    auto cache_result = Cache::create(config);
    REQUIRE(cache_result.has_value());
    VolumeConfig vc;
    vc.path = path;
    vc.size = kVolMB * 1024 * 1024;
    REQUIRE((*cache_result)->add_volume(vc).has_value());
    REQUIRE((*cache_result)->start().has_value());
    return std::move(*cache_result);
  };

  auto writer_view = make_view(0);  // owns the single stripe
  auto reader_view = make_view(1);  // owns nothing: lock-free reads
  auto s0 = writer_view->stats();
  REQUIRE(s0.stripe_count == 1);
  const uint64_t data_area = s0.stripe_bytes - s0.current_bytes;
  const uint64_t b = uint64_t{64} * 1024;
  const uint64_t n = data_area / b;
  REQUIRE(n >= 4);
  // n docs of size b fill a pass; the (n+1)-th cannot fit the trailing gap
  // (data_area % b < b) and therefore wraps to S.

  const CacheKey k_key("wrap-target-K");
  const uint32_t k_bucket = bucket_of(k_key);
  const auto k0 = make_content(21, b - kHeaderSize);
  const auto k1 = make_content(22, b - kHeaderSize);
  const auto filler = make_content(3, b - kHeaderSize);
  const auto wrapper = make_content(5, b - kHeaderSize);
  size_t idx = 0;

  // Pass 0: K at S, then n-1 fillers.
  REQUIRE(write_entry(*writer_view, "wrap-target-K", k0));
  for (uint64_t i = 0; i + 1 < n; ++i) {
    REQUIRE(write_entry(*writer_view, next_disjoint_key("c-", idx, k_bucket),
                        filler));
  }
  REQUIRE(writer_view->stats().write_buffer_wraps == 0);

  // Pass 1: K again -> wrap #1 lands it at S (e1 survives), then n-1 fillers.
  REQUIRE(write_entry(*writer_view, "wrap-target-K", k1));
  REQUIRE(writer_view->stats().write_buffer_wraps == 1);
  for (uint64_t i = 0; i + 1 < n; ++i) {
    REQUIRE(write_entry(*writer_view, next_disjoint_key("c-", idx, k_bucket),
                        filler));
  }
  REQUIRE(writer_view->stats().write_buffer_wraps == 1);
  {
    auto rk = reader_view->read_sync(k_key);
    REQUIRE(rk.has_value());
    REQUIRE(content_equals(rk->content(), k1));
  }
  REQUIRE(reader_view->stats().borrows_outstanding == 0);

  TearGate gate;
  Volume::s_write_tear_gate_for_test = [&gate](uint64_t wo, uint64_t np) {
    gate.enter(wo, np);
  };
  const std::string w_key = next_disjoint_key("wrap-", idx, k_bucket);
  std::atomic<bool> writer_ok{false};
  std::thread writer(
      [&] { writer_ok.store(write_entry(*writer_view, w_key, wrapper)); });

  gate.wait_until_at_gate();
  // Wrap #2 is committed (phase toggled back, epoch bumped, intent cleared);
  // the gated write reserved [S, S + b) and has not filled it.
  REQUIRE(reader_view->stats().write_buffer_wraps == 2);
  const uint64_t wrap_target = gate.seen_write_offset;

  std::optional<ReadHandle> held;
  {
    auto rk = reader_view->read_sync(k_key);
    if (rk.has_value()) {
      held.emplace(std::move(*rk));
    } else {
      CHECK(rk.error() == CacheError::NotFound);
    }
  }
  CAPTURE(wrap_target, data_area, b, n);
  // The invariant: e1 sits at the wrap target, which is AT the (lowered)
  // shared cursor, so the positional guard rejects it: no serve, no borrow.
  CHECK_FALSE(held.has_value());
  CHECK(reader_view->stats().borrows_outstanding == 0);

  gate.release();
  writer.join();
  Volume::s_write_tear_gate_for_test = {};
  REQUIRE(writer_ok.load());
  REQUIRE(writer_view->stats().write_buffer_wraps == 2);

  if (held.has_value()) {
    // Diagnostic for the bug: the borrow handed out inside the window now
    // aliases the wrapping write's bytes — torn under a live handle.
    CHECK(content_equals(held->content(), k1));
    held.reset();
  }

  // After the fill: K misses (e1 now points at the wrapper's bytes -> key
  // mismatch), and the wrapper serves its own bytes at S.
  REQUIRE_FALSE(reader_view->read_sync(k_key).has_value());
  {
    auto rw = reader_view->read_sync(CacheKey(w_key));
    REQUIRE(rw.has_value());
    REQUIRE(content_equals(rw->content(), wrapper));
  }
  REQUIRE(reader_view->stats().borrows_outstanding == 0);

  reader_view->stop();
  writer_view->stop();
  remove_cache_files(path);
}

#ifndef _WIN32
// --- (F6-E-MP'): a wrap whose first fill never commits
// ------------------------ Companion to the case above.  If the wrap's first
// write fails before commit_write_slot (here: pwrite fails with EFBIG under an
// RLIMIT_FSIZE below the wrap target, standing in for ENOSPC/EIO on a sparse
// cache file), nothing advances the cursor.  Were the shared cursor still at
// its pre-wrap HIGH value, the next writer would adopt it (allocate_write_slot
// re-syncs from the shared cursor), find no room, and wrap AGAIN: a second
// phase toggle with no pass in between, which re-validates every pass-0 entry
// while their intact bytes sit behind the high cursor, so the positional guard
// admits them.  With the cursor lowered to S inside the wrap, the next writer
// adopts S and reserves there without wrapping: exactly ONE wrap, and pass-0
// entries stay stale.
TEST_CASE(
    "F6-E in mmap mode: a wrap whose first fill fails advances "
    "shared_wrap_count by exactly one",
    "[wrap][forwardfill][multiprocess][f6][writelock]") {
  constexpr uint64_t kHeaderSize = 132;  // Document::kHeaderSize
  std::string path = create_temp_file("f6e_mp_fail", kVolMB);

  auto make_view = [&](uint32_t process_index) {
    CacheConfig config;
    config.ram_cache_size = 0;
    config.set_enable_checksum(true);  // required in multi-process mode
    config.set_multi_process(process_index, 2);
    config.read_lease_duration = std::chrono::milliseconds(600000);
    config.lease_wrap_ceiling = std::chrono::milliseconds(600000);
    auto cache_result = Cache::create(config);
    REQUIRE(cache_result.has_value());
    VolumeConfig vc;
    vc.path = path;
    vc.size = kVolMB * 1024 * 1024;
    REQUIRE((*cache_result)->add_volume(vc).has_value());
    REQUIRE((*cache_result)->start().has_value());
    return std::move(*cache_result);
  };

  auto writer_view = make_view(0);
  auto reader_view = make_view(1);
  auto s0 = writer_view->stats();
  REQUIRE(s0.stripe_count == 1);
  const uint64_t data_area = s0.stripe_bytes - s0.current_bytes;
  const uint64_t b = uint64_t{64} * 1024;
  const uint64_t n = data_area / b;
  REQUIRE(n >= 4);

  const auto filler = make_content(3, b - kHeaderSize);
  const auto second = make_content(9, b - kHeaderSize);
  size_t idx = 0;
  // Pass 0: n fillers.  second_key is the doc at S + b.
  std::string second_key;
  for (uint64_t i = 0; i < n; ++i) {
    const std::string key = "p0-" + std::to_string(i);
    REQUIRE(write_entry(*writer_view, key, i == 1 ? second : filler));
    if (i == 1) {
      second_key = key;
    }
  }
  REQUIRE(writer_view->stats().write_buffer_wraps == 0);
  {
    auto r = reader_view->read_sync(CacheKey(second_key));
    REQUIRE(r.has_value());
  }

  // Wrapping write #1: make its pwrite fail.  The hook runs on the writer
  // thread after the wrap committed and before the pwrite; the limit is below
  // the wrap target, so the pwrite returns EFBIG (SIGXFSZ ignored).
  struct rlimit saved{};
  REQUIRE(::getrlimit(RLIMIT_FSIZE, &saved) == 0);
  auto *old_xfsz = ::signal(SIGXFSZ, SIG_IGN);
  Volume::s_write_tear_gate_for_test = [&saved](uint64_t wo, uint64_t) {
    struct rlimit lim = saved;
    lim.rlim_cur = static_cast<rlim_t>(wo);
    (void)::setrlimit(RLIMIT_FSIZE, &lim);
  };
  const bool first_ok =
      write_entry(*writer_view, next_disjoint_key("w1-", idx, 0), filler);
  Volume::s_write_tear_gate_for_test = {};
  REQUIRE(::setrlimit(RLIMIT_FSIZE, &saved) == 0);
  ::signal(SIGXFSZ, old_xfsz);
  REQUIRE_FALSE(first_ok);  // the fill failed: nothing published
  REQUIRE(writer_view->stats().write_buffer_wraps == 1);

  // Next write: freeze it at the tear gate and look at the state it made.
  TearGate gate;
  Volume::s_write_tear_gate_for_test = [&gate](uint64_t wo, uint64_t np) {
    gate.enter(wo, np);
  };
  const std::string w2_key = next_disjoint_key("w2-", idx, 0);
  std::atomic<bool> writer_ok{false};
  std::thread writer(
      [&] { writer_ok.store(write_entry(*writer_view, w2_key, filler)); });
  gate.wait_until_at_gate();

  // Exactly one wrap: the failed fill left the cursor at S, not high.
  CHECK(reader_view->stats().write_buffer_wraps == 1);
  // The pass-0 doc at S + b must not be admitted inside the window: a second
  // wrap would have toggled its entry current again behind a high cursor.
  std::optional<ReadHandle> held;
  {
    auto r = reader_view->read_sync(CacheKey(second_key));
    if (r.has_value()) {
      held.emplace(std::move(*r));
    }
  }
  CHECK_FALSE(held.has_value());
  CHECK(reader_view->stats().borrows_outstanding == 0);
  held.reset();

  gate.release();
  writer.join();
  Volume::s_write_tear_gate_for_test = {};
  REQUIRE(writer_ok.load());
  CHECK(writer_view->stats().write_buffer_wraps == 1);
  {
    auto rw = reader_view->read_sync(CacheKey(w2_key));
    REQUIRE(rw.has_value());
    REQUIRE(content_equals(rw->content(), filler));
  }

  reader_view->stop();
  writer_view->stop();
  remove_cache_files(path);
}
#endif  // _WIN32

#ifndef _WIN32
namespace {

// A fresh shared-anonymous MmapDirectory region for the fork-based F6 cases
// (parent and child operate the SAME lock words + shared cursor).
std::pair<void *, size_t> f6_map_shared_dir(size_t num_buckets) {
  const size_t region_size = MmapDirectory::required_size(num_buckets);
  void *base = ::mmap(nullptr, region_size, PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  return {base, region_size};
}

constexpr size_t kF6cRecSize = size_t{64} * 1024;
constexpr uint32_t kF6cRecsPerWriter = 120;

// Fill one self-describing record: [writer][seq][pattern...][crc32c(prefix)].
void f6c_fill_record(std::span<std::byte> buf, uint32_t writer_id,
                     uint32_t seq) {
  auto *hdr = reinterpret_cast<uint32_t *>(buf.data());
  hdr[0] = writer_id;
  hdr[1] = seq;
  for (size_t k = 8; k < buf.size() - 4; ++k) {
    buf[k] = static_cast<std::byte>((writer_id * 131u + seq * 17u + k) & 0xFFu);
  }
  uint32_t crc =
      oracle_crc32c(std::span<const std::byte>(buf.data(), buf.size() - 4));
  std::memcpy(buf.data() + buf.size() - 4, &crc, 4);
}

// True iff the record at `buf` is internally consistent (not torn / not
// overlapped): its trailing CRC matches and its identity is in range.
bool f6c_record_valid(std::span<const std::byte> buf) {
  uint32_t stored = 0;
  std::memcpy(&stored, buf.data() + buf.size() - 4, 4);
  uint32_t crc =
      oracle_crc32c(std::span<const std::byte>(buf.data(), buf.size() - 4));
  if (crc != stored) return false;
  uint32_t writer = 0;
  uint32_t seq = 0;
  std::memcpy(&writer, buf.data(), 4);
  std::memcpy(&seq, buf.data() + 4, 4);
  return (writer == 1 || writer == 2) && seq < kF6cRecsPerWriter;
}

// One writer child: reserve+fill kF6cRecsPerWriter records.  `buf` is a
// pre-allocated, pre-faulted scratch buffer inherited across fork() — the child
// does NO heap allocation and no first-time static init (fork() copies only the
// calling thread, so a malloc lock held by another thread at fork time would
// deadlock the child).  hold_across_pwrite selects the F6 fixed protocol
// (pwrite UNDER the lock) vs the pre-F6 protocol (publish + release, THEN
// pwrite).
void f6c_writer(MmapDirectory &dir, int fd, uint32_t writer_id,
                uint64_t file_size, bool hold_across_pwrite,
                std::span<std::byte> buf) {
  for (uint32_t seq = 0; seq < kF6cRecsPerWriter; ++seq) {
    auto tok = dir.acquire_write_lock();
    uint64_t base = dir.get_shared_write_pos();
    if (base + kF6cRecSize > file_size) {
      dir.release_write_lock(tok);
      break;
    }
    f6c_fill_record(buf, writer_id, seq);
    if (hold_across_pwrite) {
      (void)!::pwrite(fd, buf.data(), kF6cRecSize, static_cast<off_t>(base));
      dir.set_shared_write_pos(base + kF6cRecSize);
      dir.release_write_lock(tok);
    } else {
      dir.set_shared_write_pos(base + kF6cRecSize);
      dir.release_write_lock(tok);
      (void)!::pwrite(fd, buf.data(), kF6cRecSize, static_cast<off_t>(base));
    }
  }
}

}  // namespace

// --- (F6-C): two writer PROCESSES, same stripe — integrity + throughput -----
TEST_CASE(
    "F6-C: two writer processes reserving through one shared write lock never "
    "overlap or tear under a hammering reader; the lock-held-across-pwrite "
    "cost "
    "is measured against the release-before-pwrite baseline",
    "[multiprocess][writelock][f6][perf]") {
  constexpr size_t kBuckets = 64;
  auto [region, region_size] = f6_map_shared_dir(kBuckets);
  REQUIRE(region != MAP_FAILED);
  auto dir_opt = MmapDirectory::init(
      std::span<std::byte>(static_cast<std::byte *>(region), region_size),
      kBuckets);
  REQUIRE(dir_opt.has_value());
  MmapDirectory &dir = *dir_opt;
  MmapDirectory::s_write_lock_presume_dead_for_test.store(false);

  // File big enough for both writers' records with headroom.
  const uint64_t needed =
      static_cast<uint64_t>(kF6cRecSize) * kF6cRecsPerWriter * 2 + kF6cRecSize;
  const size_t file_mb =
      static_cast<size_t>(needed / (uint64_t{1024} * 1024)) + 2;
  std::string path = create_temp_file("f6c_data", file_mb);
  int fd = ::open(path.c_str(), O_RDWR);
  REQUIRE(fd >= 0);
  const uint64_t file_size = static_cast<uint64_t>(file_mb) * 1024 * 1024;

  // Pre-allocate + pre-fault the writers' scratch buffers and pre-init the
  // shared CRC table BEFORE any fork(), so the forked children (which run in a
  // process that may still host background threads from earlier test cases)
  // touch no allocator or lazy-static lock.
  std::vector<std::byte> wbuf1(kF6cRecSize);
  std::vector<std::byte> wbuf2(kF6cRecSize);
  f6c_fill_record(wbuf1, 1, 0);  // faults the pages + inits the CRC table
  f6c_fill_record(wbuf2, 2, 0);

  auto run = [&](bool hold_across_pwrite, uint64_t &torn_seen) -> double {
    dir.set_shared_write_pos(0);
    auto t0 = std::chrono::steady_clock::now();
    pid_t p1 = fork();
    REQUIRE(p1 >= 0);
    if (p1 == 0) {
      f6c_writer(dir, fd, 1, file_size, hold_across_pwrite, wbuf1);
      _exit(0);
    }
    pid_t p2 = fork();
    REQUIRE(p2 >= 0);
    if (p2 == 0) {
      f6c_writer(dir, fd, 2, file_size, hold_across_pwrite, wbuf2);
      _exit(0);
    }

    // Parent hammers reads of already-PUBLISHED records while the writers run.
    std::vector<std::byte> rbuf(kF6cRecSize);
    uint64_t reads = 0;
    int st1 = 0;
    int st2 = 0;
    bool r1 = false;
    bool r2 = false;
    while (!r1 || !r2) {
      uint64_t pub = dir.get_shared_write_pos();
      uint64_t nrec = pub / kF6cRecSize;
      if (nrec > 0) {
        uint64_t idx = reads % nrec;  // deterministic sweep, cheap
        ssize_t got = ::pread(fd, rbuf.data(), kF6cRecSize,
                              static_cast<off_t>(idx * kF6cRecSize));
        if (got == static_cast<ssize_t>(kF6cRecSize) &&
            !f6c_record_valid(rbuf)) {
          ++torn_seen;
        }
        ++reads;
      }
      if (!r1 && ::waitpid(p1, &st1, WNOHANG) == p1) r1 = true;
      if (!r2 && ::waitpid(p2, &st2, WNOHANG) == p2) r2 = true;
    }
    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
  };

  // Fixed protocol: hold the lock across the pwrite.  A reader can only ever
  // observe a PUBLISHED record whose bytes are already fully written.
  // NOTE on the reported delta: this measures the NO-FSYNC case (writers
  // pwrite only).  Under sync_on_write the fsync also joins the held-lock
  // window, so the fixed-vs-prefix throughput gap widens accordingly.
  uint64_t torn_fixed = 0;
  double t_fixed = run(true, torn_fixed);
  const uint64_t published_fixed = dir.get_shared_write_pos();

  // Every published record must be intact (no overlap, no tear).
  std::vector<std::byte> vbuf(kF6cRecSize);
  uint64_t checked = 0;
  for (uint64_t off = 0; off + kF6cRecSize <= published_fixed;
       off += kF6cRecSize) {
    REQUIRE(::pread(fd, vbuf.data(), kF6cRecSize, static_cast<off_t>(off)) ==
            static_cast<ssize_t>(kF6cRecSize));
    REQUIRE(f6c_record_valid(vbuf));
    ++checked;
  }
  REQUIRE(checked ==
          uint64_t{2} * kF6cRecsPerWriter);  // both writers, no lost/overlap
  REQUIRE(torn_fixed == 0);                  // hammering reader saw no tear

  // Baseline for the throughput delta: release the lock BEFORE the pwrite.
  uint64_t torn_prefix = 0;
  double t_prefix = run(false, torn_prefix);

  UNSCOPED_INFO("F6-C throughput (2 procs x "
                << kF6cRecsPerWriter << " x " << (kF6cRecSize / 1024)
                << "KB): fixed(lock-held-across-pwrite)=" << t_fixed
                << "ms  prefix(release-before-pwrite)=" << t_prefix
                << "ms  ratio=" << (t_prefix > 0 ? t_fixed / t_prefix : 0.0));

  // Report a single pwrite+fsync latency so the escalation-deadline margin is
  // visible: the last-resort live-holder escalation is ~4096 backoff cycles
  // (>= ~4 s), orders of magnitude above one durable write.
  {
    std::vector<std::byte> one(kF6cRecSize);
    f6c_fill_record(one, 1, 0);
    auto s = std::chrono::steady_clock::now();
    (void)!::pwrite(fd, one.data(), kF6cRecSize, 0);
    ::fsync(fd);
    auto e = std::chrono::steady_clock::now();
    double us = std::chrono::duration<double, std::micro>(e - s).count();
    UNSCOPED_INFO("F6-C one pwrite+fsync of "
                  << (kF6cRecSize / 1024) << "KB = " << us
                  << "us (escalation deadline ~4e6us => margin ~"
                  << (us > 0 ? 4.0e6 / us : 0.0) << "x)");
  }

  ::close(fd);
  ::munmap(region, region_size);
  remove_cache_files(path);
}

// --- (F6-D): crash UNDER the write lock before publish ----------------------
TEST_CASE(
    "F6-D: a writer that dies holding the write lock BEFORE publishing leaves "
    "the shared cursor un-advanced; a new writer force-releases (holder proven "
    "dead), reconciles the cursor, and reuses the abandoned reservation — no "
    "wedge, no overlap",
    "[multiprocess][writelock][f6]") {
  constexpr size_t kBuckets = 64;
  auto [region, region_size] = f6_map_shared_dir(kBuckets);
  REQUIRE(region != MAP_FAILED);
  auto dir_opt = MmapDirectory::init(
      std::span<std::byte>(static_cast<std::byte *>(region), region_size),
      kBuckets);
  REQUIRE(dir_opt.has_value());
  MmapDirectory &dir = *dir_opt;
  const uint64_t kReservationBase = 0x8000;
  dir.set_shared_write_pos(kReservationBase);
  MmapDirectory::s_write_lock_presume_dead_for_test.store(false);

  int pfd[2];
  REQUIRE(::pipe(pfd) == 0);
  pid_t pid = fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    // CHILD: acquire the lock, read the reservation base, and CRASH while
    // still holding it — modelling a death mid-pwrite-under-lock, BEFORE the
    // F6 fixed protocol would publish the advanced cursor.
    ::close(pfd[0]);
    auto tok = dir.acquire_write_lock();
    uint64_t base = dir.get_shared_write_pos();
    (void)!::write(pfd[1], &base, sizeof(base));
    _exit(137);
  }

  // PARENT
  ::close(pfd[1]);
  uint64_t child_base = 0;
  REQUIRE(::read(pfd[0], &child_base, sizeof(child_base)) ==
          static_cast<ssize_t>(sizeof(child_base)));
  REQUIRE(child_base == kReservationBase);
  int status = 0;
  ::waitpid(pid, &status, 0);  // reap -> kill(pid,0) reports ESRCH

  // F6 INVARIANT: the crash left the shared cursor un-advanced (the child never
  // reached the post-pwrite publish), so it still names the durable frontier.
  REQUIRE(dir.get_shared_write_pos() == kReservationBase);

  // A new writer recovers the lock from the PROVEN-dead holder and reconciles.
  auto tok = dir.acquire_write_lock();
  CHECK(tok.acquired);
  CHECK(tok.forced_release);  // routine crash recovery, not escalation
  CHECK_FALSE(tok.escalated_takeover);
  const uint64_t base = dir.get_shared_write_pos();
  CHECK(base == kReservationBase);  // reserves at the SAME base: no gap...
  dir.set_shared_write_pos(base + 0x1000);  // ...and no overlap on advance
  dir.release_write_lock(tok);

  ::close(pfd[0]);
  ::munmap(region, region_size);
}

// --- (F6-F): last-resort escalation usurps a LIVE stalled holder ------------
TEST_CASE(
    "F6-F: an escalation-usurped live holder's late pwrite may tear the "
    "usurper's published span, but only DETECTABLY (invalid record, never a "
    "valid foreign one); the usurped holder's commit is refused and the "
    "published cursor survives",
    "[multiprocess][writelock][f6]") {
  // Pins the bounded W2-class residual documented on WriteSlot /
  // commit_write_slot: holder A stalls inside its fill longer than the
  // escalation deadline while provably ALIVE; waiter B's last-resort
  // escalation usurps, reserves the SAME base off the un-advanced shared
  // cursor, fills and publishes; A's stalled fill then lands its tail over
  // B's published record.  Required properties, asserted below:
  //   1. the takeover is the ALERTABLE escalated_takeover (holder alive),
  //      not routine forced_release;
  //   2. A observes the usurpation (revalidate fails) and refuses to
  //      publish -- the shared cursor stays exactly B's publish;
  //   3. the composite bytes at the base are INVALID (CRC fails) -- a torn
  //      record is detectable and can only resolve to Corrupted/miss, never
  //      a valid foreign record served as B's.  Only the escalation deadline
  //      bounds how late A's tail can land; nothing recalls it.
  constexpr size_t kBuckets = 64;
  auto [region, region_size] = f6_map_shared_dir(kBuckets);
  REQUIRE(region != MAP_FAILED);
  auto dir_opt = MmapDirectory::init(
      std::span<std::byte>(static_cast<std::byte *>(region), region_size),
      kBuckets);
  REQUIRE(dir_opt.has_value());
  MmapDirectory &dir = *dir_opt;
  MmapDirectory::s_write_lock_presume_dead_for_test.store(false);
  MmapDirectory::s_write_lock_max_live_waits_for_test.store(0);

  const uint64_t kBase = 0x8000;
  dir.set_shared_write_pos(kBase);

  std::string path = create_temp_file("f6f_data", 2);
  int fd = ::open(path.c_str(), O_RDWR);
  REQUIRE(fd >= 0);

  // Pre-fill BOTH records before fork (the child must not malloc or run
  // first-time static init -- see the F6-C fork-safety note).
  constexpr size_t kRec = kF6cRecSize;
  std::vector<std::byte> a_rec(kRec);  // the stalled holder's document
  std::vector<std::byte> b_rec(kRec);  // the usurper's document
  f6c_fill_record(a_rec, 2, 0);
  f6c_fill_record(b_rec, 1, 0);

  int pfd[2];
  REQUIRE(::pipe(pfd) == 0);
  pid_t pid = fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    // CHILD = holder A.  Acquire, land the FIRST half of the fill, then
    // stall (SIGSTOP: alive, mid-pwrite).  Once resumed -- after B has
    // usurped, filled and published -- land the SECOND half (the in-flight
    // tail no gate can recall), then observe the usurpation and refuse to
    // publish.
    ::close(pfd[0]);
    auto tok = dir.acquire_write_lock();
    uint64_t base = dir.get_shared_write_pos();
    (void)!::pwrite(fd, a_rec.data(), kRec / 2, static_cast<off_t>(base));
    (void)!::write(pfd[1], &base, sizeof(base));
    ::raise(SIGSTOP);  // stalled but ALIVE, still holding the lock
    (void)!::pwrite(fd, a_rec.data() + kRec / 2, kRec / 2,
                    static_cast<off_t>(base + kRec / 2));
    const bool usurp_observed = !dir.revalidate_write_lock(tok);
    dir.release_write_lock(tok);  // ownership-checked: must be a no-op
    // Do NOT publish.  Exit 0 iff the usurpation was correctly observed.
    _exit(usurp_observed ? 0 : 1);
  }

  // PARENT = waiter B.
  ::close(pfd[1]);
  uint64_t child_base = 0;
  REQUIRE(::read(pfd[0], &child_base, sizeof(child_base)) ==
          static_cast<ssize_t>(sizeof(child_base)));
  REQUIRE(child_base == kBase);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  REQUIRE(::kill(pid, 0) == 0);  // holder alive (stopped)

  // Shrink the escalation budget so the last-resort takeover fires in test
  // time instead of after the multi-second production deadline.
  MmapDirectory::s_write_lock_max_live_waits_for_test.store(4);
  auto tok2 = dir.acquire_write_lock();
  MmapDirectory::s_write_lock_max_live_waits_for_test.store(0);
  CHECK(tok2.acquired);
  CHECK(tok2.escalated_takeover);    // the ALERTABLE event...
  CHECK_FALSE(tok2.forced_release);  // ...not routine crash recovery
  CHECK(::kill(pid, 0) == 0);        // holder STILL alive at usurpation

  // B reserves off the un-advanced cursor: the SAME base A is mid-filling --
  // the overlap is by construction, not by accident.
  const uint64_t b_base = dir.get_shared_write_pos();
  CHECK(b_base == kBase);
  REQUIRE(::pwrite(fd, b_rec.data(), kRec, static_cast<off_t>(b_base)) ==
          static_cast<ssize_t>(kRec));
  dir.set_shared_write_pos(b_base + kRec);
  dir.release_write_lock(tok2);

  // Resume A: its stalled tail lands OVER B's published record.
  REQUIRE(::kill(pid, SIGCONT) == 0);
  int status = 0;
  REQUIRE(::waitpid(pid, &status, 0) == pid);
  REQUIRE(WIFEXITED(status));
  REQUIRE(WEXITSTATUS(status) == 0);  // A observed the usurp, refused commit

  // Property 2: the published cursor is exactly B's publish -- A neither
  // published nor released B's lock epoch.
  CHECK(dir.get_shared_write_pos() == b_base + kRec);

  // Property 3: the composite record (B's first half + A's late tail) is
  // INVALID -- torn but detectable, never a valid foreign record.  This is
  // the property that makes the residual acceptable: at the Volume level the
  // CRC + full-key gauntlet resolves exactly this shape to Corrupted/miss.
  std::vector<std::byte> got(kRec);
  REQUIRE(::pread(fd, got.data(), kRec, static_cast<off_t>(kBase)) ==
          static_cast<ssize_t>(kRec));
  CHECK_FALSE(f6c_record_valid(got));
  // And the tear really is the documented composite, not a lucky miss: B's
  // half then A's half.
  CHECK(std::memcmp(got.data(), b_rec.data(), kRec / 2) == 0);
  CHECK(std::memcmp(got.data() + kRec / 2, a_rec.data() + kRec / 2, kRec / 2) ==
        0);

  ::close(pfd[0]);
  ::close(fd);
  ::munmap(region, region_size);
  remove_cache_files(path);
}

#endif  // _WIN32
