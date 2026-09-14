// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// RAM-cache invalidation on alternate re-record.
//
// Writes are write-around: neither commit path populates the RAM cache, and
// reads populate it on miss.  That is fine for a key that is only ever
// written once, but an alternate that is RE-RECORDED supersedes a version a
// reader may already have pulled into RAM.  Without an invalidation at the
// commit, that RAM entry is never evicted: read_alternate_sync consults RAM
// *after* the selector picks, keyed by (key, selected id), so every later read
// of that id is a RAM hit returning the superseded bytes.  Pinned, silent, and
// with no error to observe — the disk path can be perfectly correct and the
// consumer still never sees the new content.
//
// This file is the RAM-ON counterpart of the served-version oracle in
// test_alternate_chain_bound.cpp, which deliberately disables the RAM cache to
// keep its assertions pointed at the disk path.  Everything here runs with the
// RAM cache ENABLED and at an object size that qualifies for it, because that
// is the whole point.
//
// Why each case is here:
//
//   * SINGLE ID — the dominant shape: one id refreshed on a TTL.  The
//     superseded node is the chain head, so the commit's splice is a
//     build-time link change and the invalidation is the only thing standing
//     between the consumer and stale content.
//   * MULTI ID — re-recording an id that sits mid-chain, which additionally
//     runs the post-publish in-place repoint.  Also checks the sibling id is
//     unaffected: the invalidation must be surgical, not a per-key purge.
//   * CONCURRENCY — readers racing a re-record loop.  A reader that misses RAM
//     can be copying the OLD bytes while the writer invalidates; the ordering
//     between the publish, the invalidation and the reader's resurrection
//     guard is what makes the post-quiescence state honest.
//
// Both stock selectors are covered, and both RAM cache implementations: CLFUS
// (the production default) admits an entry only on the SECOND put of a
// (key, id) it has not seen, so "populate RAM" is not a given and every case
// proves it rather than assuming it.

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "cyclone/alternate.hpp"
#include "cyclone/cache.hpp"
#include "cyclone/config.hpp"
#include "cyclone/key.hpp"
#include "support/temp_cache.hpp"

using namespace cyclone;

namespace {

constexpr size_t kVolumeBytes = static_cast<size_t>(64) * 1024 * 1024;

// Content size that qualifies for the RAM cache on every volume layout: the
// read path caches an alternate when the RAM cache exists AND either the
// volume has no persistent mmap base or the content is at most 32 KB.  Staying
// under the threshold makes the case independent of that branch.
constexpr size_t kRamEligibleSize = 4096;

CacheConfig ram_config(RamCacheType type) {
  CacheConfig c;
  c.ram_cache_size = 64_MB;
  c.ram_cache_type = type;
  return c;
}

std::unique_ptr<Cache> make_cache(const TempCacheDir& dir,
                                  const CacheConfig& cfg,
                                  size_t size = kVolumeBytes,
                                  size_t stripe_size = 0) {
  auto created = Cache::create(cfg);
  REQUIRE(created.has_value());
  auto cache = std::move(*created);
  VolumeConfig vc;
  vc.path = dir.path();
  vc.size = size;
  vc.stripe_size = stripe_size;
  REQUIRE(cache->add_volume(vc).has_value());
  REQUIRE(cache->start().has_value());
  return cache;
}

// Content that carries its own (id, version) stamp, so a served buffer can be
// checked for BOTH identity and self-consistency: the tail bytes are derived
// from the stamp, so a torn or spliced-together read fails the check.
std::vector<std::byte> stamped(AlternateId id, uint32_t version,
                               size_t size = kRamEligibleSize) {
  std::vector<std::byte> v(size);
  const auto id8 = static_cast<uint8_t>(id);
  v[0] = static_cast<std::byte>(id8);
  v[1] = static_cast<std::byte>(version & 0xFF);
  v[2] = static_cast<std::byte>((version >> 8) & 0xFF);
  v[3] = static_cast<std::byte>((version >> 16) & 0xFF);
  for (size_t i = 4; i < size; ++i) {
    v[i] = static_cast<std::byte>((id8 * 31 + version * 7 + i) & 0xFF);
  }
  return v;
}

// Inverse of stamped(): returns the version iff the buffer is a well-formed,
// self-consistent stamp for `id`, else 0.  Versions start at 1, so 0 is an
// unambiguous "no valid stamp" — and unlike an optional it prints, so a failed
// assertion names the version that WAS served instead of "{?}".
constexpr uint32_t kNoStamp = 0;

uint32_t stamp_of(std::span<const std::byte> got, AlternateId id) {
  if (got.size() < 4) {
    return kNoStamp;
  }
  const auto id8 = static_cast<uint8_t>(id);
  if (static_cast<uint8_t>(got[0]) != id8) {
    return kNoStamp;
  }
  const uint32_t version = static_cast<uint32_t>(got[1]) |
                           (static_cast<uint32_t>(got[2]) << 8) |
                           (static_cast<uint32_t>(got[3]) << 16);
  for (size_t i = 4; i < got.size(); ++i) {
    if (static_cast<uint8_t>(got[i]) != ((id8 * 31 + version * 7 + i) & 0xFF)) {
      return kNoStamp;
    }
  }
  return version;
}

bool put_alt(Cache& cache, const CacheKey& key, AlternateId id,
             std::span<const std::byte> content) {
  auto wh = cache.write_alternate_sync(key, id, content.size());
  if (!wh.has_value()) {
    return false;
  }
  if (!wh->write_sync(content).has_value()) {
    return false;
  }
  return wh->close_sync().has_value();
}

// Asserting variant: fails the test on any write error.  Never call from a
// worker thread (Catch2 assertions are not thread-safe).
void put_alt_ok(Cache& cache, const CacheKey& key, AlternateId id,
                std::span<const std::byte> content) {
  REQUIRE(put_alt(cache, key, id, content));
}

// Selector that picks exactly one AlternateId, so a read can be aimed at a
// specific chain node.
class IdSelector : public StorageAlternateSelector {
 public:
  explicit IdSelector(AlternateId want) : _want(want) {}
  [[nodiscard]] std::optional<size_t> select(
      std::span<const AlternateInfo> alternates,
      const AlternateSelectionContext& /*ctx*/) const override {
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

// Read one alternate through a specific selector and return its stamp, or
// kNoStamp on any miss/mismatch.
uint32_t read_stamp_with(Cache& cache, const CacheKey& key, AlternateId id,
                         const StorageAlternateSelector& sel,
                         bool prefer_compressed = false) {
  AlternateSelectionContext ctx;
  ctx.prefer_compressed = prefer_compressed;
  auto rh = cache.read_alternate_sync(key, sel, ctx);
  if (!rh.has_value()) {
    return kNoStamp;
  }
  return stamp_of(rh->content(), id);
}

uint32_t read_stamp(Cache& cache, const CacheKey& key, AlternateId id) {
  IdSelector sel(id);
  return read_stamp_with(cache, key, id, sel);
}

// Populate the RAM cache for (key, id) AND prove it happened.
//
// Neither half is optional.  CLFUS runs a scan-resistance admission filter: a
// (key, id) it has not seen is only marked, not admitted, so one read never
// populates.  The filter slot is indexed by the KEY alone (the id only
// perturbs the stored tag), so reads of two ids on one key keep resetting each
// other's slot — the reads below are deliberately consecutive on a single id.
// And a case that quietly failed to populate RAM would still pass every
// assertion in this file while testing nothing, so the population is verified
// against the RAM hit counter instead of assumed.
void warm_ram(Cache& cache, const CacheKey& key, AlternateId id,
              uint32_t expect_version) {
  for (int attempt = 0; attempt < 8; ++attempt) {
    const uint64_t before = cache.stats().ram_cache_hits;
    REQUIRE(read_stamp(cache, key, id) == expect_version);
    if (cache.stats().ram_cache_hits > before) {
      return;  // that read was served from RAM: the entry is live
    }
  }
  FAIL(
      "RAM cache never served this (key, alternate) — the staleness oracle "
      "would be vacuous");
}

// ---------------------------------------------------------------------------
// Single id, refreshed repeatedly
// ---------------------------------------------------------------------------
void run_single_id_refresh(RamCacheType ram_type) {
  TempCacheDir tmp("ram_alt_single");
  auto cache = make_cache(tmp, ram_config(ram_type));
  const CacheKey key("ram-alt-single");
  constexpr auto id = AlternateId::Original;

  put_alt_ok(*cache, key, id, stamped(id, 1));

  for (uint32_t v = 2; v <= 6; ++v) {
    // Pull the CURRENT version into RAM, then supersede it.  Without the
    // commit-side invalidation the reads below keep answering from this copy.
    warm_ram(*cache, key, id, v - 1);

    put_alt_ok(*cache, key, id, stamped(id, v));

    // The property a consumer depends on, through the public API.
    DefaultStorageSelector def;
    REQUIRE(read_stamp_with(*cache, key, id, def) == v);
    REQUIRE(read_stamp(*cache, key, id) == v);
  }

  // The chain is still a single node: the invalidation is independent of the
  // splice, and must not have disturbed it.
  auto alts = cache->list_alternates_sync(key);
  REQUIRE(alts.has_value());
  REQUIRE(alts->size() == 1);

  cache->stop();
}

// ---------------------------------------------------------------------------
// Multi-id chain: the re-recorded id may sit mid-chain, and its siblings must
// be left alone
// ---------------------------------------------------------------------------
void run_multi_id_refresh(RamCacheType ram_type) {
  TempCacheDir tmp("ram_alt_multi");
  auto cache = make_cache(tmp, ram_config(ram_type));
  const CacheKey key("ram-alt-multi");

  put_alt_ok(*cache, key, AlternateId::Original,
             stamped(AlternateId::Original, 1));
  put_alt_ok(*cache, key, AlternateId::Brotli, stamped(AlternateId::Brotli, 1));

  // Alternating the two ids makes every iteration re-record an id that is NOT
  // the chain head, which is the case that also exercises the post-publish
  // in-place repoint.
  uint32_t orig_v = 1;
  uint32_t brotli_v = 1;

  for (uint32_t round = 2; round <= 5; ++round) {
    // --- refresh Brotli, checked through the compression-aware selector ---
    warm_ram(*cache, key, AlternateId::Brotli, brotli_v);
    brotli_v = round;
    put_alt_ok(*cache, key, AlternateId::Brotli,
               stamped(AlternateId::Brotli, brotli_v));

    CompressionAwareSelector compression_aware;
    REQUIRE(read_stamp_with(*cache, key, AlternateId::Brotli, compression_aware,
                            /*prefer_compressed=*/true) == brotli_v);
    // Surgical: the sibling id keeps its own (older) version.
    REQUIRE(read_stamp(*cache, key, AlternateId::Original) == orig_v);

    // --- refresh Original, checked through the default selector ---
    warm_ram(*cache, key, AlternateId::Original, orig_v);
    orig_v = round;
    put_alt_ok(*cache, key, AlternateId::Original,
               stamped(AlternateId::Original, orig_v));

    REQUIRE(read_stamp(*cache, key, AlternateId::Original) == orig_v);
    REQUIRE(read_stamp(*cache, key, AlternateId::Brotli) == brotli_v);
  }

  // Both ids survived as exactly one node each.
  auto alts = cache->list_alternates_sync(key);
  REQUIRE(alts.has_value());
  REQUIRE(alts->size() == 2);

  cache->stop();
}

// ---------------------------------------------------------------------------
// The plain (non-alternate) commit path supersedes the same RAM entry
// ---------------------------------------------------------------------------
void run_plain_write_supersedes(RamCacheType ram_type) {
  // There are exactly two commit paths, and BOTH can supersede the version an
  // alternate read left in RAM.  A plain write publishes a fresh head carrying
  // the Original id, which is the same (key, id) the RAM cache is keyed by —
  // so the entry an earlier alternate read populated now describes a superseded
  // document, and both read paths will happily serve it.  This is the same
  // defect reached through the other door: the C API's write goes here.
  //
  // The plain read path only ever READS the RAM cache (it has no put of its
  // own), so the entry has to be established through an alternate read.  That
  // is not a contrivance — it is exactly what a consumer that reads via the
  // alternate API and writes via the plain one produces.
  TempCacheDir tmp("ram_plain_supersede");
  auto cache = make_cache(tmp, ram_config(ram_type));
  const CacheKey key("ram-plain-supersede");
  constexpr auto id = AlternateId::Original;

  put_alt_ok(*cache, key, id, stamped(id, 1));
  warm_ram(*cache, key, id, 1);

  // Supersede it through the PLAIN path.
  {
    const auto content = stamped(id, 2);
    auto wh = cache->write_sync(key, content.size());
    REQUIRE(wh.has_value());
    REQUIRE(wh->write_sync(std::span<const std::byte>(content)).has_value());
    REQUIRE(wh->close_sync().has_value());
  }

  // Neither read path may still answer with version 1.
  REQUIRE(read_stamp(*cache, key, id) == 2);
  {
    auto rh = cache->read_sync(key);
    REQUIRE(rh.has_value());
    REQUIRE(stamp_of(rh->content(), id) == 2);
  }

  cache->stop();
}

}  // namespace

TEST_CASE("A re-recorded alternate serves its NEW bytes with the RAM cache on",
          "[alternate][ram][regression]") {
  SECTION("CLFUS (production default)") {
    run_single_id_refresh(RamCacheType::CLFUS);
  }
  SECTION("LRU") { run_single_id_refresh(RamCacheType::LRU); }
}

TEST_CASE("Re-recording one alternate of a chain refreshes only that id in RAM",
          "[alternate][ram][regression]") {
  SECTION("CLFUS (production default)") {
    run_multi_id_refresh(RamCacheType::CLFUS);
  }
  SECTION("LRU") { run_multi_id_refresh(RamCacheType::LRU); }
}

TEST_CASE("A plain write evicts the RAM copy it supersedes",
          "[alternate][ram][regression]") {
  SECTION("CLFUS (production default)") {
    run_plain_write_supersedes(RamCacheType::CLFUS);
  }
  SECTION("LRU") { run_plain_write_supersedes(RamCacheType::LRU); }
}

// ---------------------------------------------------------------------------
// Readers racing a re-record loop
// ---------------------------------------------------------------------------
namespace {

struct RaceOutcome {
  uint32_t final_version = 0;
  int write_error = -1;
  bool saw_error = false;
  bool saw_torn = false;
  bool saw_future = false;
  bool saw_regression = false;
  uint32_t regress_from = 0;
  uint32_t regress_to = 0;
  uint64_t ram_hits = 0;
  uint64_t wraps = 0;
  uint32_t quiescent_stamp = 0;
};

// Drive readers against a re-record loop and report what they observed.
// `ram` selects the configuration: 0 disables the RAM cache entirely, which
// makes this same body a CONTROL for the layer under test — anything the
// control also reports is a property of the lock-free disk read path, not of
// the invalidation.
RaceOutcome run_rerecord_race(size_t ram_bytes) {
  // Sized so the run cannot WRAP the key's stripe (one 256MB stripe, far more
  // than the bytes written).  A wrap would legitimately orphan chain nodes and
  // let a read miss, which has nothing to do with the invalidation; the wrap
  // count is reported so the caller can keep that true.
  constexpr size_t k256MB = static_cast<size_t>(256) * 1024 * 1024;
  TempCacheDir tmp("ram_alt_races");
  CacheConfig cfg = ram_config(RamCacheType::CLFUS);
  cfg.ram_cache_size = ram_bytes;
  auto cache = make_cache(tmp, cfg, k256MB, k256MB);
  const CacheKey key("ram-alt-races");
  constexpr auto id = AlternateId::Original;

  put_alt_ok(*cache, key, id, stamped(id, 1));
  if (ram_bytes > 0) {
    warm_ram(*cache, key, id, 1);  // RAM is live before the race starts
  }

  std::atomic<bool> stop{false};
  std::atomic<uint32_t> published{1};  // highest version the writer COMMITTED
  // Highest version the writer has STARTED.  Distinct from `published`
  // because the commit becomes visible to readers inside close_sync(), i.e.
  // BEFORE the writer can record it — so `published` is not a valid ceiling
  // for what a concurrent read may legitimately return, and `attempted` is.
  std::atomic<uint32_t> attempted{1};
  // Catch2 macros are not thread-safe: workers only record into atomics.
  std::atomic<bool> saw_error{false};       // a hard read failure
  std::atomic<bool> saw_torn{false};        // content failed its own stamp
  std::atomic<bool> saw_regression{false};  // a served version went backwards
  std::atomic<bool> saw_future{false};      // a version nobody has written yet
  std::atomic<uint32_t> regress_from{0};    // first regression observed ...
  std::atomic<uint32_t> regress_to{0};      // ... and what replaced it
  std::atomic<int> write_error{-1};

  std::thread writer([&] {
    uint32_t v = 2;
    while (!stop.load(std::memory_order_relaxed)) {
      attempted.store(v, std::memory_order_release);
      auto wh = cache->write_alternate_sync(key, id, kRamEligibleSize);
      if (!wh.has_value()) {
        write_error.store(static_cast<int>(wh.error()));
        return;
      }
      const auto content = stamped(id, v);
      (void)wh->write_sync(std::span<const std::byte>(content));
      auto closed = wh->close_sync();
      if (!closed.has_value()) {
        write_error.store(static_cast<int>(closed.error()));
        return;
      }
      published.store(v, std::memory_order_release);
      ++v;
    }
  });

  auto reader_body = [&] {
    uint32_t last = 0;
    while (!stop.load(std::memory_order_relaxed)) {
      IdSelector sel(id);
      AlternateSelectionContext ctx;
      auto rh = cache->read_alternate_sync(key, sel, ctx);
      // Sampled AFTER the read: a version can only be served once the writer
      // has begun writing it, so the highest attempt observed now is a sound
      // upper bound on anything the read may legitimately have returned.
      const uint32_t ceiling = attempted.load(std::memory_order_acquire);
      if (!rh.has_value()) {
        if (rh.error() == CacheError::Corrupted ||
            rh.error() == CacheError::ChainCorrupted) {
          saw_error.store(true);
        }
        continue;  // a transient miss is legal; wrong bytes are not
      }
      const uint32_t stamp = stamp_of(rh->content(), id);
      if (stamp == kNoStamp) {
        saw_torn.store(true);  // wrong id, or bytes not self-consistent
        continue;
      }
      if (stamp > ceiling) {
        saw_future.store(true);
      }
      if (stamp < last) {
        if (!saw_regression.exchange(true)) {
          regress_from.store(last);
          regress_to.store(stamp);
        }
      }
      last = stamp;
    }
  };

  std::thread r1(reader_body);
  std::thread r2(reader_body);

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < deadline &&
         published.load(std::memory_order_acquire) < 5000) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  stop.store(true);
  writer.join();
  r1.join();
  r2.join();

  RaceOutcome out;
  out.final_version = published.load(std::memory_order_acquire);
  out.write_error = write_error.load();
  out.saw_error = saw_error.load();
  out.saw_torn = saw_torn.load();
  out.saw_future = saw_future.load();
  out.saw_regression = saw_regression.load();
  out.regress_from = regress_from.load();
  out.regress_to = regress_to.load();

  // THE oracle: quiescent, a read must serve the last version written.  A RAM
  // copy stranded by the race answers here with an older stamp forever.
  out.quiescent_stamp = read_stamp(*cache, key, id);

  const auto st = cache->stats();
  out.ram_hits = st.ram_cache_hits;
  out.wraps = st.write_buffer_wraps;

  cache->stop();
  return out;
}

}  // namespace

TEST_CASE("No stale alternate survives concurrent re-records with RAM on",
          "[alternate][ram][concurrency]") {
  // A reader that misses RAM copies the selected document into RAM with no
  // stripe lock held, so it can be copying the OLD bytes across the writer's
  // publish and invalidation.  The writer invalidates AFTER publishing and
  // bumps the stripe's remove generation as it does; the reader's
  // conditional RAM put re-checks that generation under the RAM
  // cache's write lock, so a put that lands after the invalidation's
  // eviction is dropped at insert time instead of inserted-then-withdrawn,
  // and no superseded copy outlives the invalidation.  The oracle that
  // matters is the state once the writer stops: no copy of a superseded
  // version may still be served.
  const auto out = run_rerecord_race(64_MB);

  CAPTURE(out.write_error, out.final_version, out.regress_from, out.regress_to,
          out.ram_hits, out.wraps);
  REQUIRE(out.write_error == -1);
  REQUIRE(out.final_version > 500);  // the race window was actually exercised
  REQUIRE(out.wraps == 0);           // keeps the miss/orphan reasoning real
  REQUIRE(out.ram_hits > 0);         // the RAM path was genuinely in play
  REQUIRE_FALSE(out.saw_error);
  REQUIRE_FALSE(out.saw_torn);
  REQUIRE_FALSE(out.saw_future);

  // The property this change is about: once the writer stops, the cache serves
  // the last version written — no copy of a superseded one survives anywhere.
  REQUIRE(out.quiescent_stamp == out.final_version);

  // Strict per-reader version monotonicity — asserted since the conditional-put
  // guard.
  //
  // Before the conditional put this did NOT hold with a RAM cache in front:
  // the read path's resurrection guard was put-THEN-undo — a reader that
  // raced a publish wrote its older copy into RAM and only then re-checked
  // the remove generation and took it back out, so for those few
  // instructions the older bytes were visible to another reader and a served
  // version could go backwards (measured here: order one dip per several
  // thousand re-records, shallow, always healing).  The conditional put
  // evaluates the generation re-check under the same RAM-cache write lock
  // the writer's eviction takes, making the insert atomic with respect to
  // the invalidation's bump+evict: a superseded copy can no longer outlive
  // the writer's completed invalidation, which is exactly the survival the
  // dips measured here required.  (A put landing between publish and evict
  // is still briefly served until that evict — in-flight invalidation, not
  // survival.)
  // The RAM-OFF control below pins the other side: on the disk path alone
  // monotonicity held all along, which is what localised the dips to the
  // RAM layer's old put-then-undo shape.
  REQUIRE_FALSE(out.saw_regression);
}

TEST_CASE("Concurrent re-records: RAM on matches the RAM-off control",
          "[alternate][ram][concurrency]") {
  // Control for the case above.  Per-read version monotonicity is NOT a
  // guarantee the lock-free read path makes: a reader can sample the directory
  // just before a publish and return the previous version after another read
  // already returned the newer one.  That is visible with the RAM cache
  // switched off too, so it is a property of the disk read path and not
  // something the invalidation introduces or is allowed to be blamed for.
  // Asserting it here — against the control rather than in the abstract — is
  // what keeps the RAM-on monotonicity assertion above pointed at the RAM
  // layer instead of at a property the disk path never had.
  const auto off = run_rerecord_race(0);
  CAPTURE(off.final_version, off.regress_from, off.regress_to);
  REQUIRE(off.write_error == -1);
  REQUIRE(off.final_version > 500);
  REQUIRE(off.ram_hits == 0);  // the control really had no RAM cache
  REQUIRE_FALSE(off.saw_error);
  REQUIRE_FALSE(off.saw_torn);
  REQUIRE_FALSE(off.saw_future);
  // The disk read path IS monotone: with no RAM cache to hold a withdrawn
  // copy, no reader ever sees a served version go backwards.  This is what
  // localised the earlier dips to the RAM layer's put-then-undo window,
  // rather than leaving them as an unexplained allowance.
  REQUIRE_FALSE(off.saw_regression);
  // Quiescence holds on the disk path alone, as it always did.
  REQUIRE(off.quiescent_stamp == off.final_version);
}

// ---------------------------------------------------------------------------
// Removes must not leave a resurrectable RAM copy behind
// ---------------------------------------------------------------------------
//
// remove_sync and remove_alternate_sync used to bump the stripe's remove
// generation at the TOP of the operation but evict the RAM entry only after
// the directory mutation completed — and remove_alternate_sync's
// middle/tail repoint path did not evict at all.  Both shapes leave a RAM
// copy of the removed content alive with nothing ever withdrawing it:
//
//   * MIDDLE/TAIL (deterministic): no invalidation ran, so a (key, id) copy
//     warmed before the removal simply survives it.  For most ids that copy
//     is unreachable — the selector never picks a removed id — but the plain
//     read path RAM-gets (key, Original) WITHOUT consulting the directory,
//     so a removed Original keeps being served indefinitely.
//   * TOP-BUMP (race): a lock-free reader that samples the generation
//     between the bump and the mutation still probes the LIVE entry, copies
//     it into RAM, and its re-check sees no second bump — while the
//     remover's own eviction has already run.  Same permanent resurrection.
namespace {

// Deterministic mid-chain case: warms (key, Original) in RAM, moves Original
// off the head (re-recording Brotli splices Brotli to the front), then
// removes Original — the repoint path.  On the unfixed code nothing evicts
// the warmed copy, and the plain read path keeps serving it.
void run_midchain_remove(RamCacheType ram_type) {
  TempCacheDir tmp("ram_midchain_remove");
  auto cache = make_cache(tmp, ram_config(ram_type));
  const CacheKey key("ram-midchain-remove");
  constexpr auto id = AlternateId::Original;

  put_alt_ok(*cache, key, id, stamped(id, 1));
  put_alt_ok(*cache, key, AlternateId::Brotli, stamped(AlternateId::Brotli, 1));
  warm_ram(*cache, key, id, 1);  // (key, Original) is RAM-resident

  // Re-record Brotli: it becomes the head, Original drops to the tail.
  put_alt_ok(*cache, key, AlternateId::Brotli, stamped(AlternateId::Brotli, 2));

  // Removing Original now takes the middle/tail repoint path.
  auto removed = cache->remove_alternate_sync(key, id);
  REQUIRE(removed.has_value());

  // The plain read path must not serve the removed Original.  It may serve
  // the Brotli head (which stamp_of rejects as a different id) or miss.
  auto rh = cache->read_sync(key);
  if (rh.has_value()) {
    REQUIRE(stamp_of(rh->content(), id) == kNoStamp);
  }
  REQUIRE(read_stamp(*cache, key, id) == kNoStamp);
  // Surgical: the sibling id is undisturbed and now heads the chain.
  REQUIRE(read_stamp(*cache, key, AlternateId::Brotli) == 2);

  cache->stop();
}

// The race shape that exercises the top-bump hole: a freshly-written key is
// not in RAM yet (writes are write-around), so the reads that first discover
// it are RAM MISSES — exactly the reads that probe the directory and PUT.
// The per-trial delay cycles through a spread: immediately after the write
// the storm's first wave is still mid-flight, and a remove landing while
// RAM-cold readers probe the live entry is what resurrects on the unfixed
// code.  (Measured on the pre-fix build: a few percent of trials at delays
// of 16us and up, zero at sub-8us — the delay is what places a reader's
// generation sample inside the remove's mutation window.)
constexpr std::array<uint32_t, 6> kRemoveRaceDelayUs = {12, 16, 20, 24, 32, 48};
constexpr uint32_t kRemoveRaceTrials = 600;

struct RemoveRaceOutcome {
  int trials = 0;
  int resurrected_version = 0;  // version served after its remove, or 0
  uint64_t served_while_live = 0;
  bool saw_error = false;
  bool saw_torn = false;
  uint64_t ram_hits = 0;
  uint64_t wraps = 0;
  uint32_t sibling_stamp = 0;
};

// Asserting plain-write helper.  Never call from a worker thread.
void put_plain_ok(Cache& cache, const CacheKey& key,
                  std::span<const std::byte> content) {
  auto wh = cache.write_sync(key, content.size());
  REQUIRE(wh.has_value());
  REQUIRE(wh->write_sync(content).has_value());
  REQUIRE(wh->close_sync().has_value());
}

// The strict post-quiescence oracle, Cache-level (the write-around lesson:
// lower- level oracles miss RAM-tier staleness).  Returns true iff the PLAIN
// read path still serves content stamped with `id` after the remove returned.
// The resurrection the remove paths must prevent persists forever, while a
// raced conditional put is dropped at insert time or withdrawn
// within instructions, so the first check lands 1ms out (ample healing
// time) and a positive must persist across two more checks 10ms apart — no
// production hook needed.
bool served_after_remove(Cache& cache, const CacheKey& key, AlternateId id) {
  for (int check = 0; check < 3; ++check) {
    std::this_thread::sleep_for(check == 0 ? std::chrono::milliseconds(1)
                                           : std::chrono::milliseconds(10));
    auto rh = cache.read_sync(key);
    if (!rh.has_value()) {
      return false;  // NotFound: the remove is fully visible
    }
    if (stamp_of(rh->content(), id) == kNoStamp) {
      return false;  // serves a different, still-live alternate: fine
    }
  }
  return true;
}

// Drives the write/remove loop for both remove paths.  Readers always aim at
// (key, Original) through the alternate read path — the only read path that
// PUTS into RAM — while the oracle above observes through the plain read
// path, which RAM-gets (key, Original) unconditionally.  `remove` performs
// one trial's removal (remove_sync, or remove_alternate_sync of the Original
// head) and must succeed.
RemoveRaceOutcome run_remove_race(
    RamCacheType ram_type, const char* tag,
    const std::function<void(Cache&, const CacheKey&, uint32_t)>& write_v,
    const std::function<void(Cache&, const CacheKey&)>& remove,
    std::optional<AlternateId> sibling = std::nullopt) {
  // Single 256MB stripe: the run cannot wrap (a wrap would legitimately drop
  // content, which has nothing to do with the invalidation).
  constexpr size_t k256MB = static_cast<size_t>(256) * 1024 * 1024;
  TempCacheDir tmp(tag);
  auto cache = make_cache(tmp, ram_config(ram_type), k256MB, k256MB);
  const CacheKey key("ram-remove-race");
  constexpr auto id = AlternateId::Original;

  std::atomic<bool> stop{false};
  std::atomic<bool> saw_error{false};
  std::atomic<bool> saw_torn{false};
  std::atomic<uint64_t> served_while_live{0};

  // Catch2 macros are not thread-safe: workers only record into atomics.
  auto reader_body = [&] {
    while (!stop.load(std::memory_order_relaxed)) {
      IdSelector sel(id);
      AlternateSelectionContext ctx;
      auto rh = cache->read_alternate_sync(key, sel, ctx);
      if (!rh.has_value()) {
        if (rh.error() == CacheError::Corrupted ||
            rh.error() == CacheError::ChainCorrupted) {
          saw_error.store(true);
        }
        continue;  // NotFound / AlternateNotFound: the removed windows
      }
      if (stamp_of(rh->content(), id) == kNoStamp) {
        saw_torn.store(true);  // bytes failed their own stamp
        continue;
      }
      served_while_live.fetch_add(1, std::memory_order_relaxed);
    }
  };
  std::vector<std::thread> readers;
  readers.reserve(4);
  for (int i = 0; i < 4; ++i) {
    readers.emplace_back(reader_body);
  }

  // Prove the RAM path is live for this key and configuration before the
  // race — a race that never populates RAM would pass every assertion while
  // testing nothing.
  write_v(*cache, key, 1);
  warm_ram(*cache, key, id, 1);
  remove(*cache, key);

  RemoveRaceOutcome out;
  for (uint32_t v = 2; v <= kRemoveRaceTrials + 1; ++v) {
    // Write-around leaves RAM empty, so the readers' discovery reads are
    // misses that probe and put; a remove landing mid-storm is what races
    // them.  Cycle the write->remove delay through the spread above so the
    // trial set does not depend on one machine-specific phase.
    write_v(*cache, key, v);
    const auto t0 = std::chrono::steady_clock::now();
    const uint32_t delay_us = kRemoveRaceDelayUs[v % kRemoveRaceDelayUs.size()];
    while (std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now() - t0)
               .count() < delay_us) {
    }
    remove(*cache, key);
    out.trials = static_cast<int>(v);
    if (served_after_remove(*cache, key, id)) {
      out.resurrected_version = static_cast<int>(v);
      break;
    }
  }
  if (sibling.has_value()) {
    out.sibling_stamp = read_stamp(*cache, key, *sibling);
  }

  stop.store(true);
  for (auto& r : readers) {
    r.join();
  }

  out.served_while_live = served_while_live.load();
  out.saw_error = saw_error.load();
  out.saw_torn = saw_torn.load();
  const auto st = cache->stats();
  out.ram_hits = st.ram_cache_hits;
  out.wraps = st.write_buffer_wraps;
  cache->stop();
  return out;
}

void check_remove_race_outcome(const RemoveRaceOutcome& out) {
  CAPTURE(out.trials, out.resurrected_version, out.served_while_live,
          out.ram_hits, out.wraps);
  REQUIRE(out.wraps == 0);  // keeps the miss reasoning real
  REQUIRE(out.ram_hits > 0);
  // Non-vacuousness: readers genuinely served the key during the race.
  REQUIRE(out.served_while_live > 0);
  REQUIRE_FALSE(out.saw_error);
  REQUIRE_FALSE(out.saw_torn);
  // THE property: no removed version is ever served after its remove
  // returned and the readers drained.
  REQUIRE(out.resurrected_version == 0);
}

}  // namespace

TEST_CASE("A mid-chain removed Original is never served from RAM",
          "[remove][ram][regression]") {
  SECTION("CLFUS (production default)") {
    run_midchain_remove(RamCacheType::CLFUS);
  }
  SECTION("LRU") { run_midchain_remove(RamCacheType::LRU); }
}

TEST_CASE("A key removed under concurrent readers is never served again",
          "[remove][ram][concurrency][regression]") {
  const auto run = [](RamCacheType type) {
    return run_remove_race(
        type, "ram_remove_race",
        [](Cache& c, const CacheKey& k, uint32_t v) {
          put_plain_ok(c, k, stamped(AlternateId::Original, v));
        },
        [](Cache& c, const CacheKey& k) {
          auto removed = c.remove_sync(k);
          REQUIRE(removed.has_value());
        });
  };
  SECTION("CLFUS (production default)") {
    check_remove_race_outcome(run(RamCacheType::CLFUS));
  }
  SECTION("LRU") { check_remove_race_outcome(run(RamCacheType::LRU)); }
}

TEST_CASE(
    "An Original head removed under concurrent readers is never served again",
    "[remove][ram][concurrency][regression]") {
  // remove_alternate_sync of the chain HEAD: the directory is repointed to
  // the Brotli successor, but a RAM copy of the removed Original resurrected
  // via the top-bump hole is still served by the plain read path, which
  // never consults the directory.
  const auto run = [](RamCacheType type) {
    return run_remove_race(
        type, "ram_alt_remove_race",
        [](Cache& c, const CacheKey& k, uint32_t v) {
          if (v == 1) {
            put_alt_ok(c, k, AlternateId::Brotli,
                       stamped(AlternateId::Brotli, 1));
          }
          // Re-recording Original splices it in as the new head, so every
          // removal below is the head-removal-with-successor shape.
          put_alt_ok(c, k, AlternateId::Original,
                     stamped(AlternateId::Original, v));
        },
        [](Cache& c, const CacheKey& k) {
          auto removed = c.remove_alternate_sync(k, AlternateId::Original);
          REQUIRE(removed.has_value());
        },
        AlternateId::Brotli);
  };
  SECTION("CLFUS (production default)") {
    const auto out = run(RamCacheType::CLFUS);
    check_remove_race_outcome(out);
    // The removal was surgical: the Brotli successor still serves.
    REQUIRE(out.sibling_stamp == 1);
  }
  SECTION("LRU") {
    const auto out = run(RamCacheType::LRU);
    check_remove_race_outcome(out);
    REQUIRE(out.sibling_stamp == 1);
  }
}

// ---------------------------------------------------------------------------
// RAM-coherence toggle: no behaviour change for existing (single-process)
// consumers
// ---------------------------------------------------------------------------
//
// Everything above runs with CacheConfig::cross_process_ram_coherence at its
// default (OFF), which is the whole existing consumer base.  This case runs
// the same single-id refresh oracle with the knob ON in a SINGLE-process cache
// and requires an identical outcome, including an identical RAM hit count:
// without the shared directory there is no version to validate against, and a
// toggle that quietly rejected every hit would disable the RAM tier while
// still passing every staleness assertion in this file.
namespace {

struct ToggleParityOutcome {
  uint32_t final_stamp = kNoStamp;
  uint64_t ram_hits = 0;
  uint64_t ram_misses = 0;
  uint64_t rejections = 0;
  uint64_t put_rejections = 0;
  size_t chain_len = 0;
};

ToggleParityOutcome run_toggle_parity(RamCacheType ram_type, bool coherence) {
  TempCacheDir tmp("ram_alt_toggle");
  CacheConfig cfg = ram_config(ram_type);
  cfg.cross_process_ram_coherence = coherence;
  auto cache = make_cache(tmp, cfg);
  const CacheKey key("ram-alt-toggle");
  constexpr auto id = AlternateId::Original;

  put_alt_ok(*cache, key, id, stamped(id, 1));
  for (uint32_t v = 2; v <= 6; ++v) {
    warm_ram(*cache, key, id, v - 1);
    put_alt_ok(*cache, key, id, stamped(id, v));
    REQUIRE(read_stamp(*cache, key, id) == v);
  }

  ToggleParityOutcome out;
  out.final_stamp = read_stamp(*cache, key, id);
  const auto st = cache->stats();
  out.ram_hits = st.ram_cache_hits;
  out.ram_misses = st.ram_cache_misses;
  out.rejections = st.ram_coherence_rejections;
  out.put_rejections = st.ram_coherence_put_rejections;
  auto alts = cache->list_alternates_sync(key);
  REQUIRE(alts.has_value());
  out.chain_len = alts->size();
  cache->stop();
  return out;
}

}  // namespace

TEST_CASE("The RAM-coherence toggle changes nothing in a single-process cache",
          "[alternate][ram][coherence][regression]") {
  const auto run = [](RamCacheType type) {
    const auto off = run_toggle_parity(type, false);
    const auto on = run_toggle_parity(type, true);
    CAPTURE(off.ram_hits, on.ram_hits, off.ram_misses, on.ram_misses,
            on.rejections, on.put_rejections);
    REQUIRE(off.ram_hits > 0);  // non-vacuous: the RAM tier really served
    REQUIRE(on.final_stamp == off.final_stamp);
    REQUIRE(on.ram_hits == off.ram_hits);
    REQUIRE(on.ram_misses == off.ram_misses);
    REQUIRE(on.chain_len == off.chain_len);
    REQUIRE(on.rejections == 0);
    REQUIRE(on.put_rejections == 0);
    REQUIRE(off.rejections == 0);
    REQUIRE(off.put_rejections == 0);
  };
  SECTION("CLFUS (production default)") { run(RamCacheType::CLFUS); }
  SECTION("LRU") { run(RamCacheType::LRU); }
}
