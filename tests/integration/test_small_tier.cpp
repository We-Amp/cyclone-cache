// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Integration tests for the caller-tagged small-object tier:
// a physically separate small volume, carved out of the configured cache
// size via CacheConfig::small_tier_percent, that Tier::kSmall operations
// route to.  Large-payload churn on the default tier toggles the default
// stripes' GC phase and invalidates everything there — the small volume
// never participates in default routing, so its entries must survive.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "core/volume.hpp"  // kMinStripeSize / VolumeHeader (sizing pins)
#include "cyclone/cache.hpp"
#include "cyclone/key.hpp"

#ifdef _WIN32
#include <process.h>
#define ST_GETPID _getpid
#else
#include <unistd.h>
#define ST_GETPID getpid
#endif

using namespace cyclone;

namespace {

// Sizing constants used in the expected-size math below, pinned against the
// implementation so a drift there fails the build rather than the assertions.
constexpr size_t kMB = static_cast<const size_t>(1024 * 1024);
constexpr size_t kMinStripe = 128 * kMB;
constexpr size_t kVolumeHeaderSize = 64;
static_assert(kMinStripe == kMinStripeSize,
              "test sizing math must match the implementation's stripe floor");
static_assert(kVolumeHeaderSize == VolumeHeader::kSize,
              "test sizing math must match the implementation's header size");
constexpr const char *kSmallSuffix = ".small";

// Remove the raw path AND every structural-fingerprint sibling that
// Cache::add_volume() opens -- the carved default "<stem>-<fmt>-<hash>" and the
// ".small" "<stem>-<fmt>-<hash>.small", which both share the "<stem>-" prefix.
// A plain std::remove(raw) no longer clears the files the Volumes use under
// fingerprinting, so stale files must not survive across runs on a persistent
// /tmp.  Test-only directory iteration.
void remove_cache_files(const std::string &raw_path) {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::remove(raw_path, ec);
  fs::remove(raw_path + kSmallSuffix, ec);
  const fs::path p(raw_path);
  const std::string prefix = p.filename().string() + "-";
  for (fs::directory_iterator it(p.parent_path(), ec), end; it != end && !ec;
       it.increment(ec)) {
    const std::string n = it->path().filename().string();
    if (n.size() > prefix.size() && n.compare(0, prefix.size(), prefix) == 0) {
      std::error_code e2;
      fs::remove(it->path(), e2);
    }
  }
}

std::string temp_path(const std::string &name) {
  // Unique per process AND per call so no run on a persistent /tmp reuses
  // another run's (fingerprinted) cache files.
  static std::atomic<int> counter{0};
  std::string path =
      (std::filesystem::temp_directory_path() /
       ("cyclone_small_tier_" + name + "_" + std::to_string(ST_GETPID()) + "_" +
        std::to_string(counter.fetch_add(1))))
          .string();
  remove_cache_files(path);
  return path;
}

// Resolve the structural-fingerprint filenames Cache::add_volume() opens for
// the carved default + ".small" volumes (see fingerprint_cache_path), so raw
// file_size()/exists() checks target the files the cache actually created.  The
// ".small" volume is opened with stripe_size == kMinStripeSize (see
// Cache::add_volume small-tier carve-out); the default keeps stripe_size 0.
std::string fp_default(const std::string &path, size_t default_size, bool mp) {
  return fingerprint_cache_path(path, default_size, /*stripe_size=*/0, mp);
}
std::string fp_small(const std::string &path, size_t small_size, bool mp) {
  return fingerprint_cache_path(path + kSmallSuffix, small_size,
                                /*stripe_size=*/kMinStripe, mp);
}

void cleanup(const std::string &path) { remove_cache_files(path); }

// Deterministic per-key content so reads can be verified byte-for-byte.
std::vector<std::byte> make_content(const std::string &key_str, size_t size) {
  std::vector<std::byte> content(size);
  for (size_t i = 0; i < size; ++i) {
    content[i] = static_cast<std::byte>(
        (key_str[i % key_str.size()] + static_cast<char>(i)) & 0xFF);
  }
  return content;
}

bool write_entry(Cache &cache, const std::string &key_str,
                 std::span<const std::byte> content, Tier tier) {
  CacheKey key(key_str);
  auto wh = cache.write_sync(key, content.size(), tier);
  if (!wh.has_value()) {
    return false;
  }
  auto wr = wh->write_sync(content);
  if (!wr.has_value()) {
    return false;
  }
  return wh->close_sync().has_value();
}

// Read an entry and verify its content matches make_content().
bool read_and_verify(Cache &cache, const std::string &key_str, size_t size,
                     Tier tier) {
  CacheKey key(key_str);
  auto rh = cache.read_sync(key, tier);
  if (!rh.has_value()) {
    return false;
  }
  auto content = rh->content();
  if (content.size() != size) {
    return false;
  }
  auto expected = make_content(key_str, size);
  return std::memcmp(content.data(), expected.data(), size) == 0;
}

std::unique_ptr<Cache> make_cache(const std::string &path, size_t total_size,
                                  uint32_t percent) {
  CacheConfig config;
  config.ram_cache_size = 0;       // All reads go through the volumes.
  config.enable_checksum = false;  // Match test_eviction.cpp conventions.
  config.set_small_tier_percent(percent);

  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto cache = std::move(*cache_result);

  VolumeConfig vol_config;
  vol_config.path = path;
  vol_config.size = total_size;
  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());
  return cache;
}

}  // namespace

TEST_CASE("Small-tier entries survive default-tier churn",
          "[smalltier][eviction]") {
  // The headline regression for metadata-class entries written
  // with Tier::kSmall must survive a large-payload churn that wraps the
  // default volume multiple times.
  const std::string path = temp_path("churn");

  // 40% of 320MB is just under the 128MB small-volume floor, so the small
  // volume gets the floor and the default volume ~192MB.
  constexpr size_t kTotal = 320 * kMB;
  auto cache = make_cache(path, kTotal, 40);
  REQUIRE(cache->volume_count() == 2);
  REQUIRE(cache->small_tier_active());

  // Write small (metadata-class) entries with the small tier tag.
  constexpr size_t kSmallEntries = 50;
  constexpr size_t kSmallSize = 2048;
  for (size_t i = 0; i < kSmallEntries; ++i) {
    std::string key_str = "small-entry-" + std::to_string(i);
    auto content = make_content(key_str, kSmallSize);
    REQUIRE(write_entry(*cache, key_str, content, Tier::kSmall));
  }

  // An early default-tier entry, as churn evidence: it must be gone after
  // the wraparounds below.
  const std::string early_key = "early-default-entry";
  {
    auto content = make_content(early_key, kSmallSize);
    REQUIRE(write_entry(*cache, early_key, content, Tier::kDefault));
  }

  // Churn the default tier with large payloads: ~2.5x the default volume
  // size, forcing multiple wraparounds (phase toggles) there.
  constexpr size_t kChunkSize = 8 * kMB;
  const size_t default_size = kTotal - (kMinStripe + kVolumeHeaderSize);
  const size_t churn_writes = (default_size / kChunkSize) * 5 / 2;
  std::vector<std::byte> chunk(kChunkSize, std::byte{0x42});
  for (size_t i = 0; i < churn_writes; ++i) {
    std::string key_str = "churn-" + std::to_string(i);
    // Ignore individual failures (full bucket is OK for this test).
    write_entry(*cache, key_str, chunk, Tier::kDefault);
  }

  // The default volume must actually have evicted (wrapped)...
  REQUIRE(cache->stats().evictions > 0);
  // ...taking the early default entry with it...
  REQUIRE_FALSE(cache->read_sync(CacheKey(early_key)).has_value());

  // ...while EVERY small-tier entry still reads back correct.
  for (size_t i = 0; i < kSmallEntries; ++i) {
    std::string key_str = "small-entry-" + std::to_string(i);
    INFO("small-tier entry " << i << " must survive default-tier churn");
    REQUIRE(read_and_verify(*cache, key_str, kSmallSize, Tier::kSmall));
  }

  cache->stop();
  cleanup(path);
}

TEST_CASE("Small tier is a physically separate keyspace",
          "[smalltier][routing]") {
  const std::string path = temp_path("routing");
  auto cache = make_cache(path, 320 * kMB, 40);
  REQUIRE(cache->volume_count() == 2);
  REQUIRE(cache->small_tier_active());

  const std::string key_str = "tier-routing-key";
  CacheKey key(key_str);
  constexpr size_t kSize = 1024;

  SECTION("kSmall write is invisible to kDefault reads") {
    auto content = make_content(key_str, kSize);
    REQUIRE(write_entry(*cache, key_str, content, Tier::kSmall));

    REQUIRE(read_and_verify(*cache, key_str, kSize, Tier::kSmall));

    auto default_read = cache->read_sync(key, Tier::kDefault);
    REQUIRE_FALSE(default_read.has_value());
    REQUIRE(default_read.error() == CacheError::NotFound);

    auto default_exists = cache->exists_sync(key, Tier::kDefault);
    REQUIRE(default_exists.has_value());
    REQUIRE(*default_exists == false);

    auto small_exists = cache->exists_sync(key, Tier::kSmall);
    REQUIRE(small_exists.has_value());
    REQUIRE(*small_exists == true);
  }

  SECTION("same key holds independent entries per tier") {
    auto small_content = make_content(key_str + "|small", kSize);
    auto default_content = make_content(key_str + "|default", 2 * kSize);
    {
      CacheKey k(key_str);
      auto wh = cache->write_sync(k, small_content.size(), Tier::kSmall);
      REQUIRE(wh.has_value());
      REQUIRE(wh->write_sync(std::span<const std::byte>(small_content))
                  .has_value());
      REQUIRE(wh->close_sync().has_value());
    }
    {
      CacheKey k(key_str);
      auto wh = cache->write_sync(k, default_content.size(), Tier::kDefault);
      REQUIRE(wh.has_value());
      REQUIRE(wh->write_sync(std::span<const std::byte>(default_content))
                  .has_value());
      REQUIRE(wh->close_sync().has_value());
    }

    auto small_read = cache->read_sync(key, Tier::kSmall);
    REQUIRE(small_read.has_value());
    REQUIRE(small_read->content().size() == small_content.size());

    auto default_read = cache->read_sync(key, Tier::kDefault);
    REQUIRE(default_read.has_value());
    REQUIRE(default_read->content().size() == default_content.size());

    // Removing the default entry must not touch the small entry.
    REQUIRE(cache->remove_sync(key, Tier::kDefault).has_value());
    REQUIRE_FALSE(cache->read_sync(key, Tier::kDefault).has_value());
    REQUIRE(cache->read_sync(key, Tier::kSmall).has_value());

    // And removing with kSmall removes the small entry.
    REQUIRE(cache->remove_sync(key, Tier::kSmall).has_value());
    REQUIRE_FALSE(cache->read_sync(key, Tier::kSmall).has_value());
  }

  cache->stop();
  cleanup(path);
}

TEST_CASE("kSmall falls back to default routing when tier disabled",
          "[smalltier][routing]") {
  const std::string path = temp_path("fallback");

  // percent = 0: feature off — single volume, no ".small" file.
  auto cache = make_cache(path, 8 * kMB, 0);
  REQUIRE(cache->volume_count() == 1);
  REQUIRE_FALSE(cache->small_tier_active());
  REQUIRE_FALSE(std::filesystem::exists(path + kSmallSuffix));

  // kSmall operations behave identically to kDefault: one shared keyspace.
  const std::string key_str = "fallback-key";
  CacheKey key(key_str);
  constexpr size_t kSize = 512;
  auto content = make_content(key_str, kSize);
  REQUIRE(write_entry(*cache, key_str, content, Tier::kSmall));

  REQUIRE(read_and_verify(*cache, key_str, kSize, Tier::kDefault));
  REQUIRE(read_and_verify(*cache, key_str, kSize, Tier::kSmall));

  auto exists_default = cache->exists_sync(key, Tier::kDefault);
  REQUIRE(exists_default.has_value());
  REQUIRE(*exists_default == true);

  REQUIRE(cache->remove_sync(key, Tier::kSmall).has_value());
  REQUIRE_FALSE(cache->read_sync(key, Tier::kDefault).has_value());

  cache->stop();
  cleanup(path);
}

TEST_CASE("Small-tier sizing rules", "[smalltier][sizing]") {
  SECTION("configured percentage is honored") {
    const std::string path = temp_path("sizing_pct");
    constexpr size_t kTotal = 1024 * kMB;
    auto cache = make_cache(path, kTotal, 25);
    REQUIRE(cache->volume_count() == 2);
    REQUIRE(cache->small_tier_active());

    // 25% of 1GB is far above the floor, so the carve-out is exact
    // (total / 100 * pct, matching the implementation's overflow-safe math).
    const size_t expected_small = kTotal / 100 * 25;
    REQUIRE(std::filesystem::file_size(fp_small(path, expected_small, false)) ==
            expected_small);
    REQUIRE(std::filesystem::file_size(
                fp_default(path, kTotal - expected_small, false)) ==
            kTotal - expected_small);
    REQUIRE(cache->total_capacity() == kTotal);

    cache->stop();
    cleanup(path);
  }

  SECTION("small volume is floored at one minimum-size stripe") {
    const std::string path = temp_path("sizing_floor");
    constexpr size_t kTotal = 512 * kMB;
    // 5% of 512MB = 25.6MB, well below the 128MB stripe floor.
    auto cache = make_cache(path, kTotal, 5);
    REQUIRE(cache->volume_count() == 2);
    REQUIRE(cache->small_tier_active());

    const size_t expected_small = kMinStripe + kVolumeHeaderSize;
    REQUIRE(std::filesystem::file_size(fp_small(path, expected_small, false)) ==
            expected_small);
    REQUIRE(std::filesystem::file_size(
                fp_default(path, kTotal - expected_small, false)) ==
            kTotal - expected_small);

    cache->stop();
    cleanup(path);
  }

  SECTION("out-of-range percent is clamped to 50") {
    const std::string path = temp_path("sizing_clamp");
    constexpr size_t kTotal = 768 * kMB;
    auto cache = make_cache(path, kTotal, 90);
    REQUIRE(cache->volume_count() == 2);
    REQUIRE(cache->small_tier_active());

    const size_t expected_small = kTotal / 100 * 50;
    REQUIRE(std::filesystem::file_size(fp_small(path, expected_small, false)) ==
            expected_small);
    REQUIRE(std::filesystem::file_size(
                fp_default(path, kTotal - expected_small, false)) ==
            kTotal - expected_small);

    cache->stop();
    cleanup(path);
  }

  SECTION("too-small total gracefully disables the tier") {
    const std::string path = temp_path("sizing_disable");
    // 200MB cannot host the 128MB small-volume floor plus a 128MB default
    // stripe: single-volume behavior, kSmall falls back to default routing.
    auto cache = make_cache(path, 200 * kMB, 10);
    REQUIRE(cache->volume_count() == 1);
    REQUIRE_FALSE(cache->small_tier_active());
    REQUIRE_FALSE(std::filesystem::exists(path + kSmallSuffix));

    const std::string key_str = "disabled-tier-key";
    auto content = make_content(key_str, 512);
    REQUIRE(write_entry(*cache, key_str, content, Tier::kSmall));
    REQUIRE(read_and_verify(*cache, key_str, 512, Tier::kDefault));

    cache->stop();
    cleanup(path);
  }
}

TEST_CASE("Small-tier entries persist across close and reopen",
          "[smalltier][multiprocess][persistence]") {
  // Mirrors "Multi-process directory persists across cache restart":
  // persistence requires the mmap'd (multi-process) directory — in-memory
  // directories are rebuilt empty on every open.
  const std::string path = temp_path("persist");
  constexpr size_t kTotal = 320 * kMB;
  constexpr size_t kEntries = 10;
  constexpr size_t kSize = 1024;

  auto make_mp_cache = [&]() {
    CacheConfig config;
    config.ram_cache_size = 0;
    config.set_multi_process(0, 1);  // Single process with mmap directory
    config.set_enable_checksum(true);
    config.set_small_tier_percent(40);

    auto cache_result = Cache::create(config);
    REQUIRE(cache_result.has_value());
    auto cache = std::move(*cache_result);
    REQUIRE(cache->add_volume(path, kTotal).has_value());
    REQUIRE(cache->start().has_value());
    REQUIRE(cache->volume_count() == 2);
    REQUIRE(cache->small_tier_active());
    return cache;
  };

  // First instance: write small-tier entries.
  {
    auto cache = make_mp_cache();
    for (size_t i = 0; i < kEntries; ++i) {
      std::string key_str = "persist-small-" + std::to_string(i);
      auto content = make_content(key_str, kSize);
      REQUIRE(write_entry(*cache, key_str, content, Tier::kSmall));
    }
    cache->stop();
  }

  // Second instance: every small-tier entry must read back correct, and
  // stay invisible to the default tier.
  {
    auto cache = make_mp_cache();
    for (size_t i = 0; i < kEntries; ++i) {
      std::string key_str = "persist-small-" + std::to_string(i);
      INFO("small-tier entry " << i << " must survive close+reopen");
      REQUIRE(read_and_verify(*cache, key_str, kSize, Tier::kSmall));
      REQUIRE_FALSE(
          cache->read_sync(CacheKey(key_str), Tier::kDefault).has_value());
    }
    cache->stop();
  }

  cleanup(path);
}

TEST_CASE("Multi-process small tier gives every process an owned stripe",
          "[smalltier][multiprocess]") {
  // Stripe ownership is stripe_index % total_processes, and writes to
  // non-owned stripes are rejected NotOwned (the caller drops them).  The
  // small volume therefore gets a minimum of total_processes stripes — with
  // fewer (e.g. a single 128MB stripe), every process except one could NEVER
  // write small-tier entries.  This pins the sizing side and the resulting
  // per-process write availability.
  const std::string path = temp_path("mp_ownership");
  constexpr uint32_t kProcesses = 2;
  constexpr size_t kTotal = 512 * kMB;
  constexpr size_t kKeys = 40;
  constexpr size_t kSize = 512;

  auto make_process_cache = [&](uint32_t process_index) {
    CacheConfig config;
    config.ram_cache_size = 0;
    config.set_multi_process(process_index, kProcesses);
    config.set_enable_checksum(true);
    config.set_small_tier_percent(10);

    auto cache_result = Cache::create(config);
    REQUIRE(cache_result.has_value());
    auto cache = std::move(*cache_result);
    REQUIRE(cache->add_volume(path, kTotal).has_value());
    REQUIRE(cache->start().has_value());
    return cache;
  };

  // Sizing: the 10% carve-out (51MB) is floored to total_processes stripes
  // (2 x 128MB) plus the volume header.
  std::vector<bool> written_by_p0(kKeys, false);
  {
    auto cache = make_process_cache(0);
    REQUIRE(cache->volume_count() == 2);
    REQUIRE(cache->small_tier_active());
    const size_t expected_small = kProcesses * kMinStripe + kVolumeHeaderSize;
    REQUIRE(std::filesystem::file_size(fp_small(path, expected_small, true)) ==
            expected_small);

    size_t successes = 0;
    size_t rejections = 0;
    for (size_t i = 0; i < kKeys; ++i) {
      std::string key_str = "mp-small-" + std::to_string(i);
      auto content = make_content(key_str, kSize);
      CacheKey key(key_str);
      auto wh = cache->write_sync(key, content.size(), Tier::kSmall);
      if (wh.has_value()) {
        REQUIRE(
            wh->write_sync(std::span<const std::byte>(content)).has_value());
        REQUIRE(wh->close_sync().has_value());
        written_by_p0[i] = true;
        ++successes;
      } else {
        REQUIRE(wh.error() == CacheError::NotOwned);
        ++rejections;
      }
    }
    // Process 0 owns exactly one of the two small stripes: it must be able
    // to write some keys and must be rejected on the others.
    REQUIRE(successes > 0);
    REQUIRE(rejections > 0);
    cache->stop();
  }

  // Process 1 (same files): must own exactly the complementary keys, and
  // must be able to read what process 0 wrote (shared mmap directory).
  {
    auto cache = make_process_cache(1);
    for (size_t i = 0; i < kKeys; ++i) {
      std::string key_str = "mp-small-" + std::to_string(i);
      if (written_by_p0[i]) {
        // Cross-process visibility of process 0's small-tier writes.
        INFO("process 1 must read process 0's small-tier entry " << i);
        REQUIRE(read_and_verify(*cache, key_str, kSize, Tier::kSmall));
        // ...and must NOT be able to write this key (owned by process 0).
        auto wh = cache->write_sync(CacheKey(key_str), kSize, Tier::kSmall);
        REQUIRE_FALSE(wh.has_value());
        REQUIRE(wh.error() == CacheError::NotOwned);
      } else {
        // Rejected for process 0 → owned (writable) by process 1: every
        // small-tier key is writable by exactly one of the two processes.
        auto content = make_content(key_str, kSize);
        INFO("process 1 must own small-tier key " << i);
        REQUIRE(write_entry(*cache, key_str, content, Tier::kSmall));
        REQUIRE(read_and_verify(*cache, key_str, kSize, Tier::kSmall));
      }
    }
    cache->stop();
  }

  cleanup(path);
}

TEST_CASE("Small-tier reads do not pollute default-tier hit counts",
          "[smalltier][hittracker]") {
  // The hit tracker's keyspace is (CacheKey, AlternateId) with no tier
  // dimension, and its flush callback resolves first-match-wins over the
  // default volumes.  If the small volume shared the tracker, small-tier
  // read hits on a key present in BOTH tiers would be flushed into the
  // DEFAULT entry's persisted hit count.  The small volume therefore has no
  // hit tracker wired (see Cache::add_volume_locked); this pins the
  // observable consequence via the persisted per-document hit counts.
  const std::string path = temp_path("hit_counts");
  constexpr size_t kTotal = 320 * kMB;
  constexpr size_t kSize = 1024;
  const std::string key_str = "hit-count-key";
  constexpr size_t kDefaultReads = 3;
  constexpr size_t kSmallReads = 5;

  // Multi-process mode (single process): the mmap directory persists, so a
  // reopen can observe the hit counts that reached disk — no flush-timing
  // polling needed (Cache::stop() performs the tracker's final flush).
  auto make_mp_cache = [&]() {
    CacheConfig config;
    config.ram_cache_size = 0;  // Every read must hit the volume.
    config.set_multi_process(0, 1);
    config.set_enable_checksum(true);
    config.set_small_tier_percent(40);

    auto cache_result = Cache::create(config);
    REQUIRE(cache_result.has_value());
    auto cache = std::move(*cache_result);
    REQUIRE(cache->add_volume(path, kTotal).has_value());
    REQUIRE(cache->start().has_value());
    REQUIRE(cache->small_tier_active());
    return cache;
  };

  {
    auto cache = make_mp_cache();

    // Same key in both tiers.
    auto content = make_content(key_str, kSize);
    REQUIRE(write_entry(*cache, key_str, content, Tier::kDefault));
    REQUIRE(write_entry(*cache, key_str, content, Tier::kSmall));

    for (size_t i = 0; i < kDefaultReads; ++i) {
      REQUIRE(read_and_verify(*cache, key_str, kSize, Tier::kDefault));
    }
    for (size_t i = 0; i < kSmallReads; ++i) {
      REQUIRE(read_and_verify(*cache, key_str, kSize, Tier::kSmall));
    }

    cache->stop();  // Final hit flush happens here.
  }

  // Reopen and inspect the persisted per-document hit counts.
  {
    auto cache = make_mp_cache();
    CacheKey key(key_str);

    // The default entry's count reflects the default-tier reads ONLY — it
    // must not have absorbed the small-tier reads.
    auto default_alts = cache->list_alternates_sync(key, Tier::kDefault);
    REQUIRE(default_alts.has_value());
    REQUIRE(default_alts->size() == 1);
    REQUIRE((*default_alts)[0].hit_count == kDefaultReads);

    // And the small entry records no hits at all (no tracker wired).
    auto small_alts = cache->list_alternates_sync(key, Tier::kSmall);
    REQUIRE(small_alts.has_value());
    REQUIRE(small_alts->size() == 1);
    REQUIRE((*small_alts)[0].hit_count == 0);

    cache->stop();
  }

  cleanup(path);
}
