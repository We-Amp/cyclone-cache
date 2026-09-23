// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Lease-based region pinning: borrowed mmap
// views returned by read_sync / read_alternate_sync disk hits must not be
// overwritten in place by a circular write-buffer wrap while the stripe's
// read lease holds.  Covers the reader stamp-then-revalidate protocol, the
// writer-side lease gate at the shared allocation helper, the
// anti-starvation ceiling, ReadHandle::renew_lease(), cross-view lease
// visibility through the shared mmap-directory header (offset 56), and the
// reboot/staleness clamp.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "../../src/core/mmap_directory.hpp"
#include "../../src/core/volume.hpp"  // VolumeHeader / kMinStripeSize / kAutoStripeTarget
#include "../../src/io/mapped_file.hpp"
#include "cyclone/cache.hpp"
#include "cyclone/config.hpp"
#include "cyclone/key.hpp"

#ifdef _WIN32
#include <process.h>
#define LEASE_GETPID _getpid
#else
#include <unistd.h>
#define LEASE_GETPID getpid
#endif

using namespace cyclone;

namespace {

// Remove the raw path AND the structural-fingerprint sibling(s)
// ("<stem>-<fmt>-<hash><ext>") Cache::add_volume() actually opens, so the
// fingerprinted files the Volume uses do not accumulate on a persistent /tmp.
// Test-only directory iteration.
void remove_cache_files(const std::string &path) {
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

std::string create_temp_file(const std::string &name, size_t size_mb) {
  std::string path = (std::filesystem::temp_directory_path() /
                      ("cyclone_lease_" + name + "_" +
                       std::to_string(LEASE_GETPID()) + ".dat"))
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

// Deterministic per-key content: byte j of key index i is (i * 131 + j).
std::vector<std::byte> make_content(size_t index, size_t size) {
  std::vector<std::byte> content(size);
  for (size_t j = 0; j < size; ++j) {
    content[j] = static_cast<std::byte>((index * 131 + j) & 0xFF);
  }
  return content;
}

bool content_matches(std::span<const std::byte> got, size_t index) {
  for (size_t j = 0; j < got.size(); ++j) {
    if (got[j] != static_cast<std::byte>((index * 131 + j) & 0xFF)) {
      return false;
    }
  }
  return !got.empty();
}

// Write a keyed entry; returns true when the fill was committed (a dropped
// fill surfaces as a close_sync() error).
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

uint64_t steady_now_ns_test() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

// Resolve the structural-fingerprint filename Cache::add_volume() opens, so a
// raw second MappedFile view targets the file the cache actually created (see
// fingerprint_cache_path).  These are single multi-process configs, so
// fingerprinting the path up front is exact; add_volume() re-applies it
// idempotently.
std::string fp(const std::string &path, size_t size) {
  return fingerprint_cache_path(path, size, /*stripe_size=*/0,
                                /*mmap_directory=*/true);
}

constexpr size_t kCacheSizeMB = 4;
constexpr size_t kCacheSize = kCacheSizeMB * 1024 * 1024;
constexpr size_t kChunkSize = static_cast<const size_t>(64 * 1024);

// The step a live borrow defers, in each eviction mode.  Flush mode: the
// gated WRAP itself, so no wrap happens while the borrow is held.  Wrap
// retention: the wrap is ungated and happens exactly once; the mandatory
// frontier ADVANCE over the borrowed chunk is what waits (the borrowed
// documents here all sit in the stripe's first chunk).  Both drop the
// fills.
void require_borrow_deferred_step(const CacheStats &stats, bool retention) {
  CAPTURE(retention);
  if (retention) {
    REQUIRE(stats.write_buffer_wraps == 1);
    REQUIRE(stats.advances_deferred_by_lease > 0);
    REQUIRE(stats.wraps_deferred_by_lease == 0);
  } else {
    REQUIRE(stats.write_buffer_wraps == 0);
    REQUIRE(stats.wraps_deferred_by_lease > 0);
    REQUIRE(stats.advances_deferred_by_lease == 0);
  }
  REQUIRE(stats.writes_dropped_by_lease > 0);
}

// Shared scenario for the overwrite-under-borrow regression: hold a
// borrowed ReadHandle on the first document in the stripe, then flood the
// cache far past capacity so the write position wants to wrap over it.
// Returns via out-params so the caller can assert the two regimes:
// lease on (borrow stays intact, fills drop) vs lease off / T=0 (the
// pre-lease corruption: the borrowed bytes change under the handle).
struct OverwriteOutcome {
  bool borrowed_bytes_intact = false;
  CacheStats stats;
};

OverwriteOutcome run_overwrite_under_borrow(
    std::chrono::milliseconds lease_duration, bool retention) {
  std::string cache_path = create_temp_file("overwrite", kCacheSizeMB);

  CacheConfig config;
  config.wrap_retention = retention;
  config.ram_cache_size = 0;       // Every read is a disk (mmap-borrow) hit
  config.enable_checksum = false;  // Overwritten regions are the point here
  config.read_lease_duration = lease_duration;
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = kCacheSize;

  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  // The victim is the FIRST document, so it sits at the start of the data
  // area — the exact region a wrap overwrites first.
  auto victim_content = make_content(0, kChunkSize);
  REQUIRE(write_entry(*cache, "victim", victim_content));

  CacheKey victim_key("victim");
  auto rh = cache->read_sync(victim_key);
  REQUIRE(rh.has_value());
  REQUIRE_FALSE(rh->is_ram_cache_hit());
  REQUIRE(rh->mapped_view().has_value());  // A real borrowed mmap view
  REQUIRE(content_matches(rh->content(), 0));

  // Flood: ~3x capacity of fills.  With the lease held these fills hit the
  // wrap gate and are dropped; with T=0 they wrap and pwrite over the
  // borrowed region in place.
  size_t num_entries = (kCacheSize / kChunkSize) * 3;
  for (size_t i = 0; i < num_entries; ++i) {
    auto content = make_content(i + 1, kChunkSize);
    (void)write_entry(*cache, "flood-" + std::to_string(i), content);
  }

  OverwriteOutcome outcome;
  outcome.borrowed_bytes_intact = content_matches(rh->content(), 0);
  outcome.stats = cache->stats();

  rh->close();
  cache->stop();
  remove_cache_files(cache_path);
  return outcome;
}

// --- Lease-retention regression: stripe-granular pinning ------------------
// The lease gate defers wraps at STRIPE granularity.  On a
// single-stripe volume the wrap target is always the one borrowed stripe, so
// any live read borrow blocks EVERY write — fills are dropped and the cache
// stops retaining new entries under mixed read/write.  Auto stripe sizing
// (VolumeConfig::stripe_size == 0) spreads the volume across kAutoStripeTarget
// stripes so a hot borrow pins only its own stripe while writes to the others
// land.  Bisected to the lease-pinning change; on the cache microbenchmark this
// restored the
// mixed-workload hit rate from ~49% (single 1GB stripe) to ~80% (8 stripes).
struct StripeRetentionOutcome {
  bool victim_intact = false;
  uint64_t write_buffer_wraps = 0;
  uint64_t writes_dropped_by_lease = 0;
};

StripeRetentionOutcome run_stripe_retention(size_t vol_size_mb,
                                            size_t stripe_size,
                                            size_t chunk_size, bool retention) {
  std::string cache_path = create_temp_file("stripe_retention", vol_size_mb);
  CacheConfig config;
  config.wrap_retention = retention;
  config.ram_cache_size = 0;  // Force disk (mmap-borrow) hits.
  config.enable_checksum = false;
  // Pin the lease and the anti-starvation ceiling well past the flood's
  // wall-clock so the victim's single stamp never lapses mid-test (sanitizer
  // builds run the flood far slower) — this isolates the STRIPE-granularity
  // behaviour from lease timing.
  config.read_lease_duration = std::chrono::milliseconds(600000);
  config.lease_wrap_ceiling = std::chrono::milliseconds(600000);
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = vol_size_mb * 1024 * 1024;
  vol_config.stripe_size =
      stripe_size;  // 0 = auto (multi-stripe); size = single
  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  auto victim_content = make_content(0, chunk_size);
  REQUIRE(write_entry(*cache, "victim", victim_content));
  CacheKey victim_key("victim");
  auto rh = cache->read_sync(victim_key);
  REQUIRE(rh.has_value());
  REQUIRE_FALSE(rh->is_ram_cache_hit());
  REQUIRE(rh->mapped_view().has_value());
  REQUIRE(content_matches(rh->content(), 0));

  // Flood ~2x the volume so every stripe wants to wrap.  The victim's stripe
  // defers (borrow held under a lease that outlives the flood); other stripes
  // wrap freely under multi-stripe sizing.
  size_t num_entries = (vol_config.size / chunk_size) * 2;
  for (size_t i = 0; i < num_entries; ++i) {
    (void)write_entry(*cache, "flood-" + std::to_string(i),
                      make_content(i + 1, chunk_size));
  }

  StripeRetentionOutcome outcome;
  outcome.victim_intact = content_matches(rh->content(), 0);
  CacheStats stats = cache->stats();
  outcome.write_buffer_wraps = stats.write_buffer_wraps;
  outcome.writes_dropped_by_lease = stats.writes_dropped_by_lease;

  rh->close();
  cache->stop();
  remove_cache_files(cache_path);
  return outcome;
}

}  // namespace

TEST_CASE("Lease pinning keeps borrowed bytes intact under wrap pressure",
          "[lease][eviction]") {
  // Default T (5s): the borrow's lease defers every wrap for the duration
  // of this test, so the flood drops its fills and the borrowed bytes
  // survive untouched.  THE PROTECTION IS WHAT MAKES THIS GREEN — see the
  // companion test below, which demonstrates the corruption with T=0.
  const bool retention = GENERATE(false, true);
  auto outcome =
      run_overwrite_under_borrow(std::chrono::milliseconds(5000), retention);

  REQUIRE(outcome.borrowed_bytes_intact);
  require_borrow_deferred_step(outcome.stats, retention);
  REQUIRE(outcome.stats.wraps_forced_past_lease == 0);
}

TEST_CASE("Without the lease a wrap overwrites bytes under a live borrow",
          "[lease][eviction]") {
  // T=0 disables the protocol: this is the pre-lease behavior and the
  // overwrite-under-borrow corruption class — the same scenario as above now
  // mutates the borrowed span in place under the live ReadHandle.
  const bool retention = GENERATE(false, true);
  auto outcome =
      run_overwrite_under_borrow(std::chrono::milliseconds(0), retention);

  REQUIRE_FALSE(outcome.borrowed_bytes_intact);
  REQUIRE(outcome.stats.write_buffer_wraps >= 1);
  REQUIRE(outcome.stats.wraps_deferred_by_lease == 0);
  REQUIRE(outcome.stats.writes_dropped_by_lease == 0);
}

TEST_CASE("Single-stripe volume pins the whole cache under a live borrow",
          "[lease][stripe]") {
  // One stripe == the whole cache: the borrow blocks every wrap.  Safety holds
  // (victim intact) but NO write makes progress — the regression shape.  A
  // small volume keeps this fast; the single-stripe pin is size-independent.
  // (With wrap retention the ungated wrap does happen, once; the advance
  // over the borrowed first chunk is what blocks every write after it.)
  const bool retention = GENERATE(false, true);
  auto single = run_stripe_retention(/*vol_size_mb=*/4,
                                     /*stripe_size=*/4ULL * 1024 * 1024,
                                     /*chunk_size=*/kChunkSize, retention);
  REQUIRE(single.victim_intact);
  REQUIRE(single.write_buffer_wraps == (retention ? 1 : 0));
  REQUIRE(single.writes_dropped_by_lease > 0);
}

TEST_CASE(
    "Auto stripe sizing keeps writes flowing under a live borrow "
    "(lease retention)",
    "[lease][stripe]") {
  // Auto sizing spreads a 512MB volume across multiple stripes, so the borrow
  // pins only its own while floods to the others wrap and land — write progress
  // restored AND the borrowed bytes stay intact.  Regression guard for the 1GB
  // single-stripe default that halved the mixed-workload hit rate.
  const bool retention = GENERATE(false, true);
  auto various = run_stripe_retention(/*vol_size_mb=*/512, /*stripe_size=*/0,
                                      /*chunk_size=*/512ULL * 1024, retention);
  REQUIRE(various.victim_intact);
  REQUIRE(various.write_buffer_wraps >= 1);
}

// --- Even-tiling geometry guard -------------------------------------------
// The auto stripe count is clamp(round(usable/kMinStripeSize), 1,
// kAutoStripeTarget) and the whole usable region is even-tiled across it, so
// the stripe sizes always SUM to the usable size (no wasted tail).  This is
// the regression guard for the two bugs the count-first rewrite fixed:
//   * floor(usable/stripe) truncated the last stripe (up to ~kMinStripeSize
//     wasted) and left a 1GB volume at 7 stripes, not 8;
//   * a sub-2*floor volume stayed single-stripe (the lease-retention
//     collapse) instead of splitting.
// Open a single auto volume and read the geometry back through stats().
struct AutoGeometry {
  uint64_t count;
  uint64_t bytes;
};
static AutoGeometry auto_stripe_geometry(size_t vol_size_mb) {
  std::string cache_path = create_temp_file("geometry", vol_size_mb);
  CacheConfig config;
  config.ram_cache_size = 0;
  config.enable_checksum = false;
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;
  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = vol_size_mb * 1024ULL * 1024ULL;
  vol_config.stripe_size = 0;  // auto
  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());
  CacheStats stats = cache->stats();
  cache->stop();
  remove_cache_files(cache_path);
  return {stats.stripe_count, stats.stripe_bytes};
}

TEST_CASE("Auto stripe geometry even-tiles the whole volume",
          "[stripe][geometry]") {
  const uint64_t kGran = kAutoStripeGranularity;  // 32MB granularity target
  auto expected_count = [&](size_t vol_size_mb) -> uint64_t {
    uint64_t usable = vol_size_mb * 1024ULL * 1024ULL - VolumeHeader::kSize;
    uint64_t n = (usable + kGran / 2) / kGran;  // round to nearest
    if (n < 1) n = 1;
    if (n > kAutoStripeTarget) n = kAutoStripeTarget;
    return n;
  };
  auto check = [&](size_t vol_size_mb, uint64_t want_count) {
    CAPTURE(vol_size_mb);
    uint64_t usable = vol_size_mb * 1024ULL * 1024ULL - VolumeHeader::kSize;
    auto g = auto_stripe_geometry(vol_size_mb);
    REQUIRE(g.count == expected_count(vol_size_mb));
    REQUIRE(g.count == want_count);
    // No wasted tail: the stripes tile the ENTIRE usable region.
    REQUIRE(g.bytes == usable);
  };

  check(/*vol_size_mb=*/4, /*want_count=*/1);    // below granularity -> single
  check(/*vol_size_mb=*/96, /*want_count=*/3);   // 96/32 = 3, below cap
  check(/*vol_size_mb=*/224, /*want_count=*/7);  // 224/32 = 7
  check(/*vol_size_mb=*/1024,
        /*want_count=*/kAutoStripeTarget);  // 32->capped 16
  check(/*vol_size_mb=*/4096, /*want_count=*/kAutoStripeTarget);  // capped
}

// Review #5: the derived stripe count is persisted in the volume header and
// is authoritative on reopen.  A multi-process volume created under one
// geometry and reopened by a build that would derive a DIFFERENT count must
// reset rather than read the on-disk per-stripe directories at stale
// offsets.  We simulate a geometry change by reopening the same file with an
// explicit stripe_size that yields a different count, and assert the header's
// stripe_count matches the derived geometry on a fresh create.
TEST_CASE("Persisted stripe_count matches derived geometry",
          "[stripe][geometry][header]") {
  const size_t kSizeMb = 512;  // 512MB -> 16 stripes @ gran32
  auto path = create_temp_file("stripe_count_persist", kSizeMb);
  const size_t kSize = kSizeMb * 1024ULL * 1024ULL;
  {
    CacheConfig config;
    config.ram_cache_size = 0;
    auto cache = Cache::create(config).value();
    VolumeConfig vc;
    vc.path = path;
    vc.size = kSize;
    REQUIRE(cache->add_volume(vc).has_value());
    REQUIRE(cache->start().has_value());
    auto stats = cache->stats();
    // Header stripe_count is stamped from the same derivation the stripes use.
    REQUIRE(stats.stripe_count == 16);
  }
  // Reopen with the SAME geometry: no reset, count stable, data path intact.
  {
    CacheConfig config;
    config.ram_cache_size = 0;
    auto cache = Cache::create(config).value();
    VolumeConfig vc;
    vc.path = path;
    vc.size = kSize;
    REQUIRE(cache->add_volume(vc).has_value());
    REQUIRE(cache->start().has_value());
    REQUIRE(cache->stats().stripe_count == 16);
  }
  std::filesystem::remove(path);
}

TEST_CASE("Sharded borrow accounting: cross-thread borrows all gate the wrap",
          "[lease][eviction]") {
  // The non-mmap outstanding-borrow slot is sharded per thread.
  // The wrap gate must still see the TOTAL: borrows registered by many
  // different reader threads (distinct shards) each defer the wrap, and
  // releases landing on a different thread than the acquire (the handles
  // are closed on the main thread) must drain the exact shard that was
  // counted.
  std::string cache_path = create_temp_file("shards", kCacheSizeMB);
  const bool retention = GENERATE(false, true);

  CacheConfig config;
  config.wrap_retention = retention;
  config.ram_cache_size = 0;  // Every read is a disk (mmap-borrow) hit
  config.enable_checksum = false;
  config.read_lease_duration = std::chrono::milliseconds(5000);
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = kCacheSize;
  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  constexpr size_t kReaders = 8;
  for (size_t i = 0; i < kReaders; ++i) {
    REQUIRE(write_entry(*cache, "victim-" + std::to_string(i),
                        make_content(i, kChunkSize)));
  }

  // One borrow per reader THREAD: each new thread gets its own borrow
  // shard, so the counts land on kReaders distinct slots.
  std::vector<std::optional<ReadHandle>> handles(kReaders);
  {
    std::vector<std::thread> readers;
    readers.reserve(kReaders);
    for (size_t i = 0; i < kReaders; ++i) {
      readers.emplace_back([&, i]() {
        auto rh = cache->read_sync(CacheKey("victim-" + std::to_string(i)));
        if (rh.has_value() && !rh->is_ram_cache_hit()) {
          handles[i] = std::move(*rh);
        }
      });
    }
    for (auto &t : readers) {
      t.join();
    }
  }
  for (auto &h : handles) {
    REQUIRE(h.has_value());
  }
  REQUIRE(cache->stats().borrows_outstanding == kReaders);

  // Flood past capacity: every wrap defers while ANY shard holds a count.
  size_t num_entries = (kCacheSize / kChunkSize) * 3;
  for (size_t i = 0; i < num_entries; ++i) {
    (void)write_entry(*cache, "flood-" + std::to_string(i),
                      make_content(i + 100, kChunkSize));
  }
  {
    auto stats = cache->stats();
    require_borrow_deferred_step(stats, retention);
    REQUIRE(stats.wraps_forced_past_lease == 0);
  }
  for (size_t i = 0; i < kReaders; ++i) {
    REQUIRE(content_matches(handles[i]->content(), i));
  }

  // Cross-thread release: close half on THIS thread (acquired on reader
  // threads) — the sum must drop by exactly that many.
  for (size_t i = 0; i < kReaders / 2; ++i) {
    handles[i]->close();
  }
  REQUIRE(cache->stats().borrows_outstanding == kReaders - kReaders / 2);

  for (size_t i = kReaders / 2; i < kReaders; ++i) {
    handles[i]->close();
  }
  REQUIRE(cache->stats().borrows_outstanding == 0);

  // With every shard drained the writer wraps normally (no ceiling force).
  REQUIRE(write_entry(*cache, "after-drain", make_content(7, kChunkSize)));
  {
    auto stats = cache->stats();
    REQUIRE(stats.write_buffer_wraps >= 1);
    REQUIRE(stats.wraps_forced_past_lease == 0);
  }

  cache->stop();
  remove_cache_files(cache_path);
}

TEST_CASE("renew_lease extends protection past T; lapsing frees the writer",
          "[lease][eviction]") {
  std::string cache_path = create_temp_file("renew", kCacheSizeMB);
  const bool retention = GENERATE(false, true);

  CacheConfig config;
  config.wrap_retention = retention;
  config.ram_cache_size = 0;
  config.enable_checksum = false;
  config.read_lease_duration = std::chrono::milliseconds(1000);  // T = 1s
  // Ceiling stays at the 60s default: nothing is forced in this test.
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = kCacheSize;

  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  auto victim_content = make_content(0, kChunkSize);
  REQUIRE(write_entry(*cache, "victim", victim_content));

  auto rh = cache->read_sync(CacheKey("victim"));
  REQUIRE(rh.has_value());
  REQUIRE_FALSE(rh->is_ram_cache_hit());

  // Fill the remaining tail so every further fill needs a wrap.  Renew the
  // victim lease each iteration: this is setup, and under a heavily
  // instrumented build (TSan) filling the tail can take longer than T, which
  // would otherwise let the lease lapse mid-setup and turn the terminal
  // wrap-needing write into an accepted wrap instead of the intended drop
  // (an unbounded loop).  Keeping the lease live is the precondition the test
  // means to establish; the drop-vs-renew behaviour under load is exercised
  // by Phase 1 below.
  size_t warm = 0;
  auto filler = make_content(9999, kChunkSize);
  while (write_entry(*cache, "warm-" + std::to_string(warm), filler)) {
    REQUIRE(rh->renew_lease());
    ++warm;
    REQUIRE(warm < 2000);  // Safety bound (free tail is deterministic in size)
  }

  // Phase 1: hold the borrow for ~1.5s (> T) while renewing at a cadence
  // well under the 3T/4 protection floor.  Every wrap-needing fill must
  // keep dropping and the borrowed bytes must stay intact.
  for (int i = 0; i < 6; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    REQUIRE(rh->renew_lease());
    REQUIRE_FALSE(write_entry(*cache, "p1-" + std::to_string(i), filler));
  }
  REQUIRE(content_matches(rh->content(), 0));
  {
    auto stats = cache->stats();
    REQUIRE(stats.writes_dropped_by_lease >= 6);
    require_borrow_deferred_step(stats, retention);
    REQUIRE(stats.wraps_forced_past_lease == 0);
  }

  // Phase 2: stop renewing and let the lease lapse (sleep well past T).
  // The wrap then proceeds naturally — no ceiling forcing involved.
  std::this_thread::sleep_for(std::chrono::milliseconds(1600));
  REQUIRE(write_entry(*cache, "p2", filler));
  {
    auto stats = cache->stats();
    REQUIRE(stats.write_buffer_wraps >= 1);
    REQUIRE(stats.wraps_forced_past_lease == 0);
  }

  // A RAM-cache-less disk borrow renews; after stop() nothing renews.
  rh->close();
  cache->stop();
  remove_cache_files(cache_path);
}

TEST_CASE(
    "Starvation ceiling: reads keep dropping fills until the forced "
    "wrap restores write capacity",
    "[lease][eviction]") {
  std::string cache_path = create_temp_file("ceiling", kCacheSizeMB);

  CacheConfig config;
  config.ram_cache_size = 0;
  config.enable_checksum = false;
  // T long enough that the lease can never lapse mid-test: only the
  // ceiling can unblock the writer.
  config.read_lease_duration = std::chrono::milliseconds(30000);
  config.lease_wrap_ceiling = std::chrono::milliseconds(500);
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = kCacheSize;

  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  auto victim_content = make_content(0, kChunkSize);
  REQUIRE(write_entry(*cache, "victim", victim_content));
  CacheKey victim_key("victim");

  // HOLD the borrow (only an OPEN handle defers wraps — the
  // the original version of this test relied on the residual lease timestamp of
  // a closed handle, which no longer blocks anything) and stamp the (long)
  // lease: from here on every wrap must be deferred until the ceiling
  // forces it.
  auto rh = cache->read_sync(victim_key);
  REQUIRE(rh.has_value());
  REQUIRE_FALSE(rh->is_ram_cache_hit());
  REQUIRE(cache->stats().borrows_outstanding == 1);

  // Fill the tail so further fills need a wrap (the loop's terminating
  // failure is the first borrow-deferred drop).
  size_t warm = 0;
  auto filler = make_content(9999, kChunkSize);
  while (write_entry(*cache, "warm-" + std::to_string(warm), filler)) {
    ++warm;
    REQUIRE(warm < 1000);
  }

  // The held borrow keeps deferring wrap-needing fills
  // (writes_dropped_by_lease grows) until the deferral has lasted longer
  // than the ceiling, at which point the wrap is forced
  // (wraps_forced_past_lease increments) and the fill SUCCEEDS again.
  bool fill_succeeded_after_force = false;
  uint64_t dropped_before_force = 0;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  int attempt = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    bool ok = write_entry(*cache, "c-" + std::to_string(attempt++), filler);
    auto stats = cache->stats();
    if (stats.wraps_forced_past_lease >= 1) {
      fill_succeeded_after_force = ok;
      break;
    }
    dropped_before_force = stats.writes_dropped_by_lease;
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }

  auto stats = cache->stats();
  REQUIRE(stats.wraps_forced_past_lease >= 1);
  REQUIRE(fill_succeeded_after_force);     // The forcing fill lands
  REQUIRE(dropped_before_force > 0);       // ... after real starvation
  REQUIRE(stats.write_buffer_wraps >= 1);  // The wrap really happened
  // the forced wrap reset the outstanding-borrow slot — the
  // (invalidated) held handle no longer registers, and its late release
  // is generation-dropped.
  REQUIRE(cache->stats().borrows_outstanding == 0);

  rh->close();
  REQUIRE(cache->stats().borrows_outstanding == 0);  // release was a no-op

  cache->stop();
  remove_cache_files(cache_path);
}

TEST_CASE("Borrows racing wraps never observe corrupted content",
          "[lease][eviction][multiprocess]") {
  // Stamp-then-revalidate ordering under real wrap pressure.  Two Cache
  // views on one file in one process, mirroring the cross-process
  // topology: the writer view owns the stripe (multi_process 0/2); the
  // reader view (1/2) owns nothing, so its reads take NO stripe lock —
  // the exact lock-free probe → stamp → revalidate path the protocol
  // protects.  Readers pause periodically so leases lapse and the writer
  // actually wraps; any successful read must return exactly the bytes
  // written for that key, both immediately and while the (short, < 3T/4)
  // hold continues.
  //
  // With the wrap-intent flag (offset 33) this is a HARD guarantee — the
  // writer's intent-store-then-lease-load against the reader's
  // stamp-then-intent/epoch-load closes the Dekker window entirely (proof
  // at Volume::allocate_write_slot) — so this test asserts zero
  // corruption over an extended run, not a best-effort bound.
  constexpr size_t kStressChunk = static_cast<const size_t>(16 * 1024);
  std::string cache_path = create_temp_file("stress", kCacheSizeMB);

  auto make_view = [&](uint32_t process_index) {
    CacheConfig config;
    config.ram_cache_size = 0;
    config.set_enable_checksum(true);  // Required in multi-process mode
    config.set_multi_process(process_index, 2);
    config.read_lease_duration = std::chrono::milliseconds(100);
    auto cache_result = Cache::create(config);
    REQUIRE(cache_result.has_value());
    VolumeConfig vol_config;
    vol_config.path = cache_path;
    vol_config.size = kCacheSize;
    REQUIRE((*cache_result)->add_volume(vol_config).has_value());
    REQUIRE((*cache_result)->start().has_value());
    return std::move(*cache_result);
  };

  auto writer_view = make_view(0);  // Owns the single stripe
  auto reader_view = make_view(1);  // Owns nothing: lock-free reads

  std::atomic<bool> stop{false};
  std::atomic<int64_t> latest{-1};
  std::atomic<uint64_t> reads_ok{0};
  std::atomic<uint64_t> corruptions{0};

  auto writer_fn = [&]() {
    size_t i = 0;
    while (!stop.load(std::memory_order_relaxed)) {
      auto content = make_content(i, kStressChunk);
      if (write_entry(*writer_view, "s-" + std::to_string(i), content)) {
        latest.store(static_cast<int64_t>(i), std::memory_order_relaxed);
      }
      ++i;
    }
  };

  auto reader_fn = [&](unsigned seed) {
    uint64_t rng = seed * 2654435761ULL + 12345;
    int iters_since_pause = 0;
    while (!stop.load(std::memory_order_relaxed)) {
      int64_t max_idx = latest.load(std::memory_order_relaxed);
      if (max_idx < 0) {
        std::this_thread::yield();
        continue;
      }
      rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
      auto idx = static_cast<size_t>((rng >> 33) %
                                     (static_cast<uint64_t>(max_idx) + 1));
      auto rh = reader_view->read_sync(CacheKey("s-" + std::to_string(idx)));
      if (rh.has_value()) {
        // Verify twice across a short hold — both checks run well inside
        // the 3T/4 protection floor of the lease stamped by this read.
        bool ok_now = content_matches(rh->content(), idx);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        bool ok_held = content_matches(rh->content(), idx);
        if (ok_now && ok_held) {
          reads_ok.fetch_add(1, std::memory_order_relaxed);
        } else {
          corruptions.fetch_add(1, std::memory_order_relaxed);
        }
      }
      // NotFound/Corrupted are legitimate under churn (evicted entries,
      // CRC-detected torn reads) — only a SUCCESSFUL read with wrong
      // bytes counts as corruption.

      if (++iters_since_pause >= 20) {
        iters_since_pause = 0;
        // Let the lease lapse so the writer can actually wrap.
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
      }
    }
  };

  std::thread writer(writer_fn);
  std::thread r1(reader_fn, 1);
  std::thread r2(reader_fn, 2);

  // Longer run for the hard guarantee: at least kMinRuntime of sustained
  // read/wrap interleaving, and keep going until real wrap pressure and a
  // meaningful read count have accumulated (or a generous cap).
  const auto start = std::chrono::steady_clock::now();
  const auto min_end = start + std::chrono::seconds(8);
  const auto deadline = start + std::chrono::seconds(45);
  while (std::chrono::steady_clock::now() < deadline) {
    auto now = std::chrono::steady_clock::now();
    auto stats = writer_view->stats();
    if (now >= min_end && stats.write_buffer_wraps >= 5 &&
        reads_ok.load() >= 200) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  stop.store(true);
  writer.join();
  r1.join();
  r2.join();

  auto stats = writer_view->stats();
  INFO("wraps=" << stats.write_buffer_wraps << " reads_ok=" << reads_ok.load()
                << " dropped=" << stats.writes_dropped_by_lease);
  REQUIRE(corruptions.load() == 0);
  REQUIRE(reads_ok.load() > 0);
  REQUIRE(stats.write_buffer_wraps >= 1);  // Real wrap pressure occurred

  reader_view->stop();
  writer_view->stop();
  remove_cache_files(cache_path);
}

TEST_CASE(
    "A reader arriving during a held wrap intent misses cleanly and "
    "recovers after release",
    "[lease][eviction][multiprocess]") {
  // Simulates a writer paused mid-wrap-decision: a raw second view on the
  // shared mmap-directory header holds write_lock + wrap_intent — exactly
  // the window across which allocate_write_slot keeps the flag up.  A
  // concurrent read must NOT hand out a borrow: revalidation sees
  // intent == 1, burns its bounded retry budget, and reports a clean miss
  // (NotFound, never Corrupted, never success).  Once the flag clears,
  // the same read succeeds with intact content.
  std::string cache_path =
      fp(create_temp_file("intent", kCacheSizeMB), kCacheSize);

  CacheConfig config;
  config.ram_cache_size = 0;
  config.set_multi_process(0, 1);  // mmap (shared) directories
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = kCacheSize;
  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  auto content = make_content(0, kChunkSize);
  REQUIRE(write_entry(*cache, "victim", content));
  CacheKey victim_key("victim");
  {
    auto rh = cache->read_sync(victim_key);
    REQUIRE(rh.has_value());
    REQUIRE(content_matches(rh->content(), 0));
  }

  // Raw second view onto the stripe's shared directory header (the single
  // stripe starts right after the 64-byte VolumeHeader; the bucket count
  // matches the volume's per-stripe directory sizing, 16K buckets).
  auto mf = MappedFile::create();
  REQUIRE(mf != nullptr);
  REQUIRE(mf->open(cache_path, MappedFile::OpenMode::ReadWrite).has_value());
  constexpr size_t kBuckets = static_cast<const size_t>(16 * 1024);
  auto region = mf->map_region(64, MmapDirectory::required_size(kBuckets),
                               MappedFile::MapMode::ReadWrite);
  REQUIRE(region.has_value());
  auto dir = MmapDirectory::open(*region);
  REQUIRE(dir.has_value());
  REQUIRE_FALSE(dir->wrap_intent());

  // Hold write_lock + intent, as a mid-decision writer would.
  auto write_token = dir->acquire_write_lock();
  dir->set_wrap_intent(true);

  {
    auto rh = cache->read_sync(victim_key);
    REQUIRE_FALSE(rh.has_value());
    // Bounded retries exhausted against a held intent → clean miss.
    REQUIRE(rh.error() == CacheError::NotFound);
  }

  dir->set_wrap_intent(false);
  dir->release_write_lock(write_token);

  {
    auto rh = cache->read_sync(victim_key);
    REQUIRE(rh.has_value());
    REQUIRE(content_matches(rh->content(), 0));
  }

  mf->unmap_region(*region);
  cache->stop();
  remove_cache_files(cache_path);
}

TEST_CASE(
    "An open borrow in one cache view defers wraps in a second live view; "
    "closing it restores write capacity",
    "[lease][eviction][multiprocess]") {
  // Cross-view visibility through the shared mmap-directory header: the
  // outstanding-borrow slot at offset 34 plus the lease at offset 56.  Two
  // live Cache views on one file mirror the cross-process topology (the
  // writer view owns the single stripe; the reader view owns nothing).
  // While the reader view HOLDS a disk borrow, the writer view's
  // wrap-needing fills must drop; the moment the borrow is CLOSED — well
  // inside its 30s lease — write capacity must return.  (Before the borrow gate
  // this test asserted the inverse second half: the residual lease timestamp of
  // a long-CLOSED borrow kept a fresh view write-starved.  That is the
  // write-starvation starvation defect, removed on purpose.)
  std::string cache_path = create_temp_file("secondview", kCacheSizeMB);
  const bool retention = GENERATE(false, true);

  auto make_view = [&](uint32_t process_index) {
    CacheConfig config;
    config.wrap_retention = retention;
    config.ram_cache_size = 0;
    config.set_enable_checksum(true);  // Required in multi-process mode
    config.set_multi_process(process_index, 2);
    config.read_lease_duration = std::chrono::milliseconds(30000);
    auto cache_result = Cache::create(config);
    REQUIRE(cache_result.has_value());
    VolumeConfig vol_config;
    vol_config.path = cache_path;
    vol_config.size = kCacheSize;
    REQUIRE((*cache_result)->add_volume(vol_config).has_value());
    REQUIRE((*cache_result)->start().has_value());
    return std::move(*cache_result);
  };

  auto writer_view = make_view(0);  // Owns the single stripe
  auto reader_view = make_view(1);  // Owns nothing: lock-free reads

  auto victim_content = make_content(0, kChunkSize);
  REQUIRE(write_entry(*writer_view, "victim", victim_content));

  // Reader view takes and HOLDS the borrow: registered in the SHARED slot.
  auto rh = reader_view->read_sync(CacheKey("victim"));
  REQUIRE(rh.has_value());
  REQUIRE_FALSE(rh->is_ram_cache_hit());
  REQUIRE(content_matches(rh->content(), 0));
  // Both views see the shared gauge.
  REQUIRE(reader_view->stats().borrows_outstanding == 1);
  REQUIRE(writer_view->stats().borrows_outstanding == 1);

  // Writer view floods past capacity: every wrap attempt must defer on the
  // reader view's live borrow, and the borrowed bytes must stay intact.
  auto filler = make_content(1, kChunkSize);
  size_t num_entries = (kCacheSize / kChunkSize) * 2;
  for (size_t i = 0; i < num_entries; ++i) {
    (void)write_entry(*writer_view, "flood-" + std::to_string(i), filler);
  }
  {
    auto stats = writer_view->stats();
    require_borrow_deferred_step(stats, retention);
  }
  REQUIRE(content_matches(rh->content(), 0));

  // Close the borrow — 30s lease still live — and the very next
  // wrap-needing fill in the writer view must succeed.
  rh->close();
  REQUIRE(writer_view->stats().borrows_outstanding == 0);
  REQUIRE(write_entry(*writer_view, "after-close", filler));
  REQUIRE(writer_view->stats().write_buffer_wraps >= 1);

  reader_view->stop();
  writer_view->stop();
  remove_cache_files(cache_path);
}

TEST_CASE("Writer ignores a bogus far-future lease (reboot/staleness clamp)",
          "[lease][eviction][multiprocess]") {
  // CLOCK_MONOTONIC restarts at boot, so a persisted expiry from a prior
  // boot can read as unexpired for days.  The writer must treat any expiry
  // beyond now + T + ceiling as stale and ignore it.  Pre-write a bogus
  // far-future expiry directly into the header slot and assert wraps
  // proceed unhindered.
  std::string cache_path = create_temp_file("staleclamp", kCacheSizeMB);

  auto make_view = [&]() {
    CacheConfig config;
    config.ram_cache_size = 0;
    config.set_multi_process(0, 1);
    config.read_lease_duration = std::chrono::milliseconds(5000);
    auto cache_result = Cache::create(config);
    REQUIRE(cache_result.has_value());
    VolumeConfig vol_config;
    vol_config.path = cache_path;
    vol_config.size = kCacheSize;
    REQUIRE((*cache_result)->add_volume(vol_config).has_value());
    REQUIRE((*cache_result)->start().has_value());
    return std::move(*cache_result);
  };

  // --- View 1: initialize the volume, then close. ---
  {
    auto cache = make_view();
    auto content = make_content(0, kChunkSize);
    REQUIRE(write_entry(*cache, "seed", content));
    cache->stop();
  }

  // --- Patch a bogus far-future expiry into the on-disk lease slot. ---
  // The single stripe starts right after the 64-byte VolumeHeader, and the
  // lease slot sits at mmap-directory-header offset 56 (static_assert'd in
  // mmap_directory.hpp).
  {
    constexpr uint64_t kVolumeHeaderSize = 64;
    const uint64_t slot_offset =
        kVolumeHeaderSize +
        offsetof(MmapDirectory::Header, stripe_lease_expiry_ns);
    // Now + ~10 hours: far beyond the now + T + ceiling clamp (~65s).
    uint64_t bogus_expiry =
        steady_now_ns_test() + uint64_t{10} * 3600 * 1000000000ULL;
    FILE *f = std::fopen(cache_path.c_str(), "r+b");
    REQUIRE(f != nullptr);
    REQUIRE(std::fseek(f, static_cast<long>(slot_offset), SEEK_SET) == 0);
    REQUIRE(std::fwrite(&bogus_expiry, sizeof(bogus_expiry), 1, f) == 1);
    std::fclose(f);
  }

  // --- View 2: flood past capacity; the bogus lease must be ignored. ---
  {
    auto cache = make_view();
    auto filler = make_content(1, kChunkSize);
    size_t num_entries = (kCacheSize / kChunkSize) * 3;
    for (size_t i = 0; i < num_entries; ++i) {
      (void)write_entry(*cache, "flood-" + std::to_string(i), filler);
    }

    auto stats = cache->stats();
    REQUIRE(stats.write_buffer_wraps >= 1);       // Wraps proceeded
    REQUIRE(stats.wraps_deferred_by_lease == 0);  // Never deferred
    REQUIRE(stats.writes_dropped_by_lease == 0);  // Nothing dropped
    REQUIRE(stats.wraps_forced_past_lease == 0);  // Ignored, not forced
    cache->stop();
  }

  remove_cache_files(cache_path);
}

// checked-renew tests (lease amendment 2026-07-07): renew_read_lease is now
// epoch-only and check-before-stamp — it reports an overwrite (moved epoch)
// so the embedder copies/aborts, never false-aborts on a transient wrap
// intent, and is use-after-free-safe when a borrow outlives its Cache.

TEST_CASE(
    "renew_lease fails once a forced wrap moves the borrow epoch (copy "
    "trigger)",
    "[lease][renew][eviction]") {
  std::string cache_path = create_temp_file("renewfail", kCacheSizeMB);

  CacheConfig config;
  config.ram_cache_size = 0;
  config.enable_checksum = false;
  config.read_lease_duration =
      std::chrono::milliseconds(30000);                        // never lapses
  config.lease_wrap_ceiling = std::chrono::milliseconds(300);  // forces fast
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = kCacheSize;
  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  REQUIRE(write_entry(*cache, "victim", make_content(0, kChunkSize)));
  CacheKey victim_key("victim");
  auto rh = cache->read_sync(victim_key);
  REQUIRE(rh.has_value());
  REQUIRE_FALSE(rh->is_ram_cache_hit());
  REQUIRE(rh->mapped_view().has_value());
  REQUIRE(rh->renew_lease());  // fresh borrow, epoch current: renews

  // Fill the tail so every further fill needs a wrap.
  size_t warm = 0;
  auto filler = make_content(9999, kChunkSize);
  while (write_entry(*cache, "warm-" + std::to_string(warm), filler)) {
    ++warm;
    REQUIRE(warm < 1000);
  }

  // Sustain the lease (our held borrow + fresh reads) and push fills until
  // the anti-starvation ceiling forces a wrap over the borrowed region.
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  int attempt = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    (void)cache->read_sync(victim_key);  // keep the lease live
    (void)write_entry(*cache, "c-" + std::to_string(attempt++), filler);
    if (cache->stats().wraps_forced_past_lease >= 1) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  REQUIRE(cache->stats().wraps_forced_past_lease >= 1);

  // The forced wrap moved the stripe epoch: the checked renew now FAILS,
  // signalling the embedder to copy/abort rather than keep aliasing an
  // overwritten region.  (Pre-amendment renew_read_lease blindly re-stamped
  // and returned true here — the corruption this fix closes.)
  REQUIRE_FALSE(rh->renew_lease());

  rh->close();
  cache->stop();
  remove_cache_files(cache_path);
}

TEST_CASE(
    "renew_lease ignores a transient wrap_intent (no false-positive copy)",
    "[lease][renew][multiprocess]") {
  // A held borrow whose stripe has wrap_intent raised but NOT committed (a
  // deferred-wrap attempt in flight, zero side effects) must keep renewing
  // successfully: intent alone is inconclusive; only a committed epoch change
  // fails the renew.  Failing on transient intent would spuriously force the
  // embedder to copy/abort a valid in-flight serve on every hot-stripe writer
  // attempt.  (Contrast the INITIAL borrow, which conservatively misses on
  // intent — see the intent-miss test above.)
  std::string cache_path =
      fp(create_temp_file("renewintent", kCacheSizeMB), kCacheSize);

  CacheConfig config;
  config.ram_cache_size = 0;
  config.set_multi_process(0, 1);  // mmap (shared) directories
  config.read_lease_duration = std::chrono::milliseconds(30000);
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = kCacheSize;
  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  REQUIRE(write_entry(*cache, "victim", make_content(0, kChunkSize)));
  CacheKey victim_key("victim");
  auto rh = cache->read_sync(victim_key);
  REQUIRE(rh.has_value());
  REQUIRE_FALSE(rh->is_ram_cache_hit());
  REQUIRE(rh->renew_lease());  // baseline

  // Raw second view onto the stripe's shared directory header; raise intent
  // WITHOUT completing a wrap (epoch stays put).
  auto mf = MappedFile::create();
  REQUIRE(mf != nullptr);
  REQUIRE(mf->open(cache_path, MappedFile::OpenMode::ReadWrite).has_value());
  constexpr size_t kBuckets = static_cast<const size_t>(16 * 1024);
  auto region = mf->map_region(64, MmapDirectory::required_size(kBuckets),
                               MappedFile::MapMode::ReadWrite);
  REQUIRE(region.has_value());
  auto dir = MmapDirectory::open(*region);
  REQUIRE(dir.has_value());
  auto write_token = dir->acquire_write_lock();
  dir->set_wrap_intent(true);

  // Epoch unchanged, so the checked renew SUCCEEDS despite the raised intent.
  REQUIRE(rh->renew_lease());

  dir->set_wrap_intent(false);
  dir->release_write_lock(write_token);
  REQUIRE(rh->renew_lease());  // still fine after release

  mf->unmap_region(*region);
  rh->close();
  cache->stop();
  remove_cache_files(cache_path);
}

TEST_CASE("Anchored read handles keep their spans valid past stop() and ~Cache",
          "[lease][teardown][anchor]") {
  // disk-hit handles from a Cache-owned Volume pin the Volume
  // and the mapping through a per-thread-shard anchor.  The old weak_ptr
  // contract made a handle that outlived teardown safe to DESTROY but not
  // to USE (its spans dangled once the MappedFile died).  The anchor
  // upgrades this: the spans stay valid until the handle closes, while
  // stop()/~Cache remain non-blocking and the checked renews
  // still report the teardown.
  std::string cache_path = create_temp_file("anchorlife", kCacheSizeMB);

  CacheConfig config;
  config.ram_cache_size = 0;
  config.enable_checksum = false;
  config.read_lease_duration = std::chrono::milliseconds(30000);
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  std::unique_ptr<Cache> cache = std::move(*cache_result);

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = kCacheSize;
  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  REQUIRE(write_entry(*cache, "victim", make_content(0, kChunkSize)));
  auto rh = cache->read_sync(CacheKey("victim"));
  REQUIRE(rh.has_value());
  REQUIRE_FALSE(rh->is_ram_cache_hit());
  REQUIRE(rh->mapped_view().has_value());
  REQUIRE(rh->renew_lease());

  // Past stop(): spans valid, renews refuse (teardown is published).
  cache->stop();
  REQUIRE(content_matches(rh->content(), 0));
  REQUIRE_FALSE(rh->renew_lease());
  REQUIRE(rh->renew_lease_strict() == LeaseRenewal::kTorn);

  // Past ~Cache: the anchor alone keeps the Volume + mapping alive.
  cache.reset();
  REQUIRE(content_matches(rh->content(), 0));
  REQUIRE_FALSE(rh->renew_lease());

  rh->close();  // Last ref: Volume + MappedFile die here (ASan-verified).
  remove_cache_files(cache_path);
}

TEST_CASE("Read anchors are rebuilt across a stop()/start() cycle",
          "[lease][teardown][anchor]") {
  // Every start() may create a fresh MappedFile; a handle taken after the
  // restart must pin (and later unmap on) the NEW mapping, not a stale
  // anchor's old one.  A handle from before the stop keeps the OLD mapping
  // alive independently.  ASan turns a stale-anchor regression into a
  // use-after-free here.
  std::string cache_path = create_temp_file("anchorcycle", kCacheSizeMB);

  CacheConfig config;
  config.ram_cache_size = 0;
  config.enable_checksum = false;
  config.read_lease_duration = std::chrono::milliseconds(30000);
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  std::unique_ptr<Cache> cache = std::move(*cache_result);

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = kCacheSize;
  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  REQUIRE(write_entry(*cache, "gen1", make_content(1, kChunkSize)));
  auto rh1 = cache->read_sync(CacheKey("gen1"));
  REQUIRE(rh1.has_value());
  REQUIRE_FALSE(rh1->is_ram_cache_hit());

  cache->stop();
  // Between stop and restart the pinned old mapping still shows the bytes.
  REQUIRE(content_matches(rh1->content(), 1));
  REQUIRE(cache->start().has_value());  // reopen: fresh mapping, new anchors

  REQUIRE(write_entry(*cache, "gen2", make_content(2, kChunkSize)));
  auto rh2 = cache->read_sync(CacheKey("gen2"));
  REQUIRE(rh2.has_value());
  REQUIRE_FALSE(rh2->is_ram_cache_hit());

  REQUIRE(content_matches(rh2->content(), 2));  // new mapping
  // rh1 stays SAFE to access, but not stable: both MappedFiles view the
  // same file, and the restarted instance may lawfully recycle gen1's
  // region (leases do not span instances).  Touch it without asserting
  // the bytes — ASan is what fails this test if the pin regressed.
  volatile std::byte sink{};
  for (std::byte b : rh1->content()) {
    sink = b;
  }
  (void)sink;

  // The gen-1 anchor is latched torn: the restart reset Volume::_teardown,
  // so WITHOUT the latch these renews (and the close below) would
  // dereference the freed gen-1 stripes — the exact use-after-free ASan
  // caught in review.
  REQUIRE_FALSE(rh1->renew_lease());
  REQUIRE(rh1->renew_lease_strict() == LeaseRenewal::kTorn);

  rh1->close();
  rh2->close();
  cache->stop();
  cache.reset();
  remove_cache_files(cache_path);
}

TEST_CASE("renew_lease is UAF-safe when the borrow outlives its Cache",
          "[lease][renew][teardown]") {
  // A client-paced zero-copy drain can hold a ReadHandle past Cache teardown
  // (the "handles closed before stop()" contract is violated by a long
  // serve).  The checked renew must fail cleanly rather than dereference a
  // freed Volume/Stripe.  volume_weak.lock() returning empty is the guard;
  // under ASan a regression to a raw Volume* would trip here.
  std::string cache_path = create_temp_file("renewteardown", kCacheSizeMB);

  CacheConfig config;
  config.ram_cache_size = 0;
  config.enable_checksum = false;
  config.read_lease_duration = std::chrono::milliseconds(30000);
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  std::unique_ptr<Cache> cache = std::move(*cache_result);

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = kCacheSize;
  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  REQUIRE(write_entry(*cache, "victim", make_content(0, kChunkSize)));
  auto rh = cache->read_sync(CacheKey("victim"));
  REQUIRE(rh.has_value());
  REQUIRE_FALSE(rh->is_ram_cache_hit());
  REQUIRE(rh->mapped_view().has_value());
  REQUIRE(rh->renew_lease());  // alive: renews

  // Destroy the Cache (and its Volumes) while the borrow is still held.
  cache->stop();
  cache.reset();

  // The Volume is gone: renew must return false without touching freed
  // memory.  Do NOT deref rh->content() (its mapping was unmapped by the
  // MappedFile destructor); the handle destructor is separately safe via
  // mapped_file_weak.
  REQUIRE_FALSE(rh->renew_lease());

  remove_cache_files(cache_path);
}

// strict-renew tests (lease amendment 2026-07-07): renew_lease_strict() is the
// intent-CHECKED variant the ALIASED zero-copy path must call before every
// send.  Unlike epoch-only renew_lease() it stamps THEN Dekker-checks (intent
// before epoch, like the initial borrow), returning kOk / kCopyNow (wrap in
// flight, de-alias) / kTorn (epoch moved, abort) / kLeasesOff.  The aliased
// writev's read is unobservable, so a raced normal wrap at the lease-lapse
// boundary can only be caught by the intent check — this is what closes it.

TEST_CASE(
    "renew_lease_strict returns kOk on a fresh borrow, kTorn once a forced "
    "wrap moves the epoch",
    "[lease][renew][strict][eviction]") {
  std::string cache_path = create_temp_file("strictfail", kCacheSizeMB);

  CacheConfig config;
  config.ram_cache_size = 0;
  config.enable_checksum = false;
  config.read_lease_duration =
      std::chrono::milliseconds(30000);                        // never lapses
  config.lease_wrap_ceiling = std::chrono::milliseconds(300);  // forces fast
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = kCacheSize;
  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  REQUIRE(write_entry(*cache, "victim", make_content(0, kChunkSize)));
  CacheKey victim_key("victim");
  auto rh = cache->read_sync(victim_key);
  REQUIRE(rh.has_value());
  REQUIRE_FALSE(rh->is_ram_cache_hit());
  REQUIRE(rh->mapped_view().has_value());
  REQUIRE(rh->renew_lease_strict() == LeaseRenewal::kOk);  // fresh, uncontended

  size_t warm = 0;
  auto filler = make_content(9999, kChunkSize);
  while (write_entry(*cache, "warm-" + std::to_string(warm), filler)) {
    ++warm;
    REQUIRE(warm < 1000);
  }

  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  int attempt = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    (void)cache->read_sync(victim_key);  // keep the lease live
    (void)write_entry(*cache, "c-" + std::to_string(attempt++), filler);
    if (cache->stats().wraps_forced_past_lease >= 1) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  REQUIRE(cache->stats().wraps_forced_past_lease >= 1);

  // The forced wrap moved the epoch: strict renew reports kTorn (abort), not
  // kOk — the aliased path must RST rather than stream overwritten bytes.
  REQUIRE(rh->renew_lease_strict() == LeaseRenewal::kTorn);

  rh->close();
  cache->stop();
  remove_cache_files(cache_path);
}

TEST_CASE(
    "renew_lease_strict returns kCopyNow on a raised wrap_intent where "
    "renew_lease ignores it (aliased-path de-alias)",
    "[lease][renew][strict][multiprocess]") {
  // THE S4 fix: a wrap decision in flight (intent raised, epoch not yet moved)
  // means a normal wrap may be about to overwrite the region.  The COPY path's
  // epoch-only renew_lease() deliberately ignores transient intent (the copy is
  // re-verified after), but the ALIASED path cannot re-verify an unobservable
  // socket send, so renew_lease_strict() must report kCopyNow so the embedder
  // de-aliases.  This test asserts the two diverge on the SAME state.
  std::string cache_path =
      fp(create_temp_file("strictintent", kCacheSizeMB), kCacheSize);

  CacheConfig config;
  config.ram_cache_size = 0;
  config.set_multi_process(0, 1);  // mmap (shared) directories
  config.read_lease_duration = std::chrono::milliseconds(30000);
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = kCacheSize;
  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  REQUIRE(write_entry(*cache, "victim", make_content(0, kChunkSize)));
  CacheKey victim_key("victim");
  auto rh = cache->read_sync(victim_key);
  REQUIRE(rh.has_value());
  REQUIRE_FALSE(rh->is_ram_cache_hit());
  REQUIRE(rh->renew_lease_strict() ==
          LeaseRenewal::kOk);  // baseline: no intent

  auto mf = MappedFile::create();
  REQUIRE(mf != nullptr);
  REQUIRE(mf->open(cache_path, MappedFile::OpenMode::ReadWrite).has_value());
  constexpr size_t kBuckets = static_cast<const size_t>(16 * 1024);
  auto region = mf->map_region(64, MmapDirectory::required_size(kBuckets),
                               MappedFile::MapMode::ReadWrite);
  REQUIRE(region.has_value());
  auto dir = MmapDirectory::open(*region);
  REQUIRE(dir.has_value());
  auto write_token = dir->acquire_write_lock();
  dir->set_wrap_intent(true);

  // Same state, two verdicts: epoch-only renew_lease() SUCCEEDS (intent alone
  // is inconclusive for the re-verified copy path); intent-checked
  // renew_lease_strict() returns kCopyNow (the aliased path must de-alias).
  REQUIRE(rh->renew_lease());
  REQUIRE(rh->renew_lease_strict() == LeaseRenewal::kCopyNow);

  dir->set_wrap_intent(false);
  dir->release_write_lock(write_token);
  // Intent cleared, epoch unchanged: strict renew is back to kOk.
  REQUIRE(rh->renew_lease_strict() == LeaseRenewal::kOk);

  mf->unmap_region(*region);
  rh->close();
  cache->stop();
  remove_cache_files(cache_path);
}

TEST_CASE(
    "renew_lease_strict returns kLeasesOff when leases are disabled (copy, "
    "never RST)",
    "[lease][renew][strict]") {
  // read_lease_duration == 0 disables the protocol.  strict renew must report
  // kLeasesOff (not kTorn) so a default-on aliased serve degrades to a copy and
  // NEVER turns every serve into a reset.
  std::string cache_path = create_temp_file("strictoff", kCacheSizeMB);

  CacheConfig config;
  config.ram_cache_size = 0;
  config.enable_checksum = false;
  config.read_lease_duration = std::chrono::milliseconds(0);  // leases OFF
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = kCacheSize;
  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  REQUIRE(write_entry(*cache, "victim", make_content(0, kChunkSize)));
  auto rh = cache->read_sync(CacheKey("victim"));
  REQUIRE(rh.has_value());
  REQUIRE_FALSE(rh->is_ram_cache_hit());
  REQUIRE(rh->mapped_view().has_value());
  REQUIRE(rh->renew_lease_strict() == LeaseRenewal::kLeasesOff);

  rh->close();
  cache->stop();
  remove_cache_files(cache_path);
}

// STEP-3 test (lease amendment 2026-07-07): the deferring writer publishes a
// cross-process per-stripe force-wrap DEADLINE that a zero-copy borrower reads
// (ns_until_forced_wrap) to copy the aliased bytes out BEFORE the force fires.

TEST_CASE(
    "ns_until_forced_wrap publishes the force-wrap deadline to a borrower "
    "(STEP-3)",
    "[lease][step3][multiprocess]") {
  std::string cache_path = create_temp_file("step3", kCacheSizeMB);

  CacheConfig config;
  config.ram_cache_size = 0;
  config.set_multi_process(0, 1);  // mmap shared header — the embedder's path
  config.read_lease_duration =
      std::chrono::milliseconds(30000);                        // never lapses
  config.lease_wrap_ceiling = std::chrono::milliseconds(500);  // forces fast
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = kCacheSize;
  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  REQUIRE(write_entry(*cache, "victim", make_content(0, kChunkSize)));
  auto rh = cache->read_sync(CacheKey("victim"));
  REQUIRE(rh.has_value());
  REQUIRE_FALSE(rh->is_ram_cache_hit());

  // No write pressure yet: no wrap deferred -> no reachable force.
  REQUIRE(rh->ns_until_forced_wrap() == UINT64_MAX);

  // Fill the tail; the first wrap-needing fill is deferred by our lease and
  // publishes the force-wrap deadline into the shared header.
  size_t warm = 0;
  auto filler = make_content(9999, kChunkSize);
  while (write_entry(*cache, "warm-" + std::to_string(warm), filler)) {
    ++warm;
    REQUIRE(warm < 1000);
  }

  // A deferral episode is now open: the deadline is published and bounded by
  // the ceiling.  A zero-copy embedder reads this to copy-out before the force.
  uint64_t ns = rh->ns_until_forced_wrap();
  REQUIRE(ns != UINT64_MAX);
  REQUIRE(ns <= static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::milliseconds(600))
                        .count()));

  rh->close();
  cache->stop();
  remove_cache_files(cache_path);
}

// Borrow-gate regression tests: the wrap gate is borrow-scoped.  Before the
// fix, every disk-hit read stamped a T=read_lease_duration lease with no
// release path, so ANY at-capacity stripe read more often than once per T
// accepted ~one (ceiling-forced) write per lease_wrap_ceiling — the
// write-starvation shape the consumer stress suites measured (writes
// dropped ~= writes attempted, admissions frozen in exact ceiling-length
// windows).  Now the residual lease timestamp of a CLOSED handle blocks
// nothing: only an OPEN handle (with a fresh lease) defers wraps.

TEST_CASE(
    "Steady reads with promptly-closed handles never starve writers at "
    "capacity",
    "[lease][eviction][starvation]") {
  std::string cache_path = create_temp_file("noleak", kCacheSizeMB);

  CacheConfig config;
  config.ram_cache_size = 0;  // Every read is a disk (mmap-borrow) hit
  config.enable_checksum = false;
  // Default-shaped lease config: T long enough that under the original
  // lease-gate semantics every read below would pin the stripe for the whole
  // test.
  config.read_lease_duration = std::chrono::milliseconds(30000);
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = kCacheSize;
  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  // Fill far past capacity so the stripe is at capacity and wrapping.
  auto filler = make_content(9999, kChunkSize);
  size_t num_entries = (kCacheSize / kChunkSize) * 2;
  for (size_t i = 0; i < num_entries; ++i) {
    REQUIRE(write_entry(*cache, "fill-" + std::to_string(i), filler));
  }
  REQUIRE(cache->stats().write_buffer_wraps >= 1);

  // The borrow-gate shape: interleave disk-hit reads (handle closed immediately
  // — the common copy-out consumer) with fills.  EVERY fill must land; under
  // the original gate each read re-stamped the stripe lease and every
  // wrap-needing fill here was dropped.
  for (int i = 0; i < 100; ++i) {
    std::string key = "rw-" + std::to_string(i);
    auto content = make_content(static_cast<size_t>(i), kChunkSize);
    REQUIRE(write_entry(*cache, key, content));
    auto rh = cache->read_sync(CacheKey(key));  // Just written: must hit
    REQUIRE(rh.has_value());
    REQUIRE_FALSE(rh->is_ram_cache_hit());
    REQUIRE(content_matches(rh->content(), static_cast<size_t>(i)));
    // rh closes here — write capacity is back before the next iteration.
  }

  auto stats = cache->stats();
  REQUIRE(stats.writes_dropped_by_lease == 0);  // No fill was starved
  REQUIRE(stats.wraps_forced_past_lease == 0);  // ... and none needed force
  REQUIRE(stats.write_buffer_wraps >= 2);       // Under real wrap pressure
  REQUIRE(stats.borrows_outstanding == 0);      // All handles released

  cache->stop();
  remove_cache_files(cache_path);
}

TEST_CASE(
    "Closing a held borrow returns write capacity immediately, not at "
    "lease expiry",
    "[lease][eviction][starvation]") {
  std::string cache_path = create_temp_file("closerelease", kCacheSizeMB);

  CacheConfig config;
  config.ram_cache_size = 0;
  config.enable_checksum = false;
  // T = 30s: if release still waited for lease expiry (the original
  // behavior), the
  // post-close write below could not succeed within this test's lifetime.
  config.read_lease_duration = std::chrono::milliseconds(30000);
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = kCacheSize;
  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  REQUIRE(write_entry(*cache, "victim", make_content(0, kChunkSize)));
  auto rh = cache->read_sync(CacheKey("victim"));
  REQUIRE(rh.has_value());
  REQUIRE_FALSE(rh->is_ram_cache_hit());
  REQUIRE(cache->stats().borrows_outstanding == 1);

  // Fill the tail; the terminating failure is the first drop deferred on
  // the held borrow's behalf.
  size_t warm = 0;
  auto filler = make_content(9999, kChunkSize);
  while (write_entry(*cache, "warm-" + std::to_string(warm), filler)) {
    ++warm;
    REQUIRE(warm < 1000);
  }
  REQUIRE(cache->stats().writes_dropped_by_lease >= 1);
  REQUIRE(content_matches(rh->content(), 0));  // Protected while open

  // Close the handle: the 30s lease is still fresh, but the very next
  // wrap-needing fill must succeed — release-on-close, no lease tail.
  rh->close();
  REQUIRE(cache->stats().borrows_outstanding == 0);
  REQUIRE(write_entry(*cache, "after-close", filler));
  {
    auto stats = cache->stats();
    REQUIRE(stats.write_buffer_wraps >= 1);
    REQUIRE(stats.wraps_forced_past_lease == 0);  // Natural, not forced
  }

  cache->stop();
  remove_cache_files(cache_path);
}

TEST_CASE(
    "A residual lease persisted by a closed borrow does not block a fresh "
    "view",
    "[lease][eviction][starvation][multiprocess]") {
  // The exact defect shape, through the on-disk shared header: view 1
  // disk-reads (stamping a 30s lease into the shared slot at offset 56),
  // CLOSES the handle (draining the borrow slot at offset 34) and shuts
  // down; a fresh view 2 floods past capacity.  With the original gate the
  // persisted timestamp alone write-starved view 2 (this file asserted exactly
  // that); now the drained borrow count lets every wrap proceed.
  std::string cache_path = create_temp_file("residual", kCacheSizeMB);

  auto make_view = [&]() {
    CacheConfig config;
    config.ram_cache_size = 0;
    config.set_multi_process(0, 1);  // mmap (shared) directories
    config.read_lease_duration = std::chrono::milliseconds(30000);
    auto cache_result = Cache::create(config);
    REQUIRE(cache_result.has_value());
    VolumeConfig vol_config;
    vol_config.path = cache_path;
    vol_config.size = kCacheSize;
    REQUIRE((*cache_result)->add_volume(vol_config).has_value());
    REQUIRE((*cache_result)->start().has_value());
    return std::move(*cache_result);
  };

  // --- View 1: write + disk-read (stamps a 30s lease), close, shut down.
  {
    auto cache = make_view();
    REQUIRE(write_entry(*cache, "victim", make_content(0, kChunkSize)));
    auto rh = cache->read_sync(CacheKey("victim"));
    REQUIRE(rh.has_value());
    REQUIRE_FALSE(rh->is_ram_cache_hit());
    rh->close();
    cache->stop();
  }

  // --- View 2: fresh Cache on the same file; flood past capacity. ---
  {
    auto cache = make_view();
    REQUIRE(cache->stats().borrows_outstanding == 0);
    auto filler = make_content(1, kChunkSize);
    size_t num_entries = (kCacheSize / kChunkSize) * 2;
    for (size_t i = 0; i < num_entries; ++i) {
      REQUIRE(write_entry(*cache, "flood-" + std::to_string(i), filler));
    }

    auto stats = cache->stats();
    REQUIRE(stats.write_buffer_wraps >= 1);       // Wraps proceeded
    REQUIRE(stats.writes_dropped_by_lease == 0);  // Nothing starved
    REQUIRE(stats.wraps_forced_past_lease == 0);  // ... without forcing
    cache->stop();
  }

  remove_cache_files(cache_path);
}
