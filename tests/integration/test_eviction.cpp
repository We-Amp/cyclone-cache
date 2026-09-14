// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "cyclone/cache.hpp"
#include "cyclone/key.hpp"
#include "support/temp_cache.hpp"

using namespace cyclone;

namespace {

// Helper: write a key with content of the given size, return true on success
bool write_entry(Cache &cache, const std::string &key_str,
                 size_t content_size) {
  CacheKey key(key_str);
  std::vector<std::byte> content(content_size, std::byte{0x42});

  auto wh = cache.write_sync(key, content.size());
  if (!wh.has_value()) {
    return false;
  }
  auto wr = wh->write_sync(std::span<const std::byte>(content));
  if (!wr.has_value()) {
    return false;
  }
  auto cl = wh->close_sync();
  return cl.has_value();
}

}  // namespace

TEST_CASE("Stale entries invisible after wraparound", "[eviction]") {
  // Use a small cache (8 MB) so we can trigger wraparound quickly.
  // RAM cache disabled (size 0) so all reads go through the directory.
  // Checksums disabled to avoid checksum failures on overwritten regions.
  constexpr size_t kCacheSizeMB = 8;
  constexpr size_t kCacheSize = kCacheSizeMB * 1024 * 1024;
  constexpr size_t kChunkSize =
      static_cast<const size_t>(64 * 1024);  // 64 KB per entry

  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheConfig config;
  config.ram_cache_size = 0;  // Disable RAM cache
  config.enable_checksum = false;
  // This test verifies phase-based invalidation after a wrap that
  // immediately follows a read.  Read leases would (by design)
  // defer that wrap for read_lease_duration — disable them here; the
  // lease protocol has its own tests in test_lease_pinning.cpp.
  config.read_lease_duration = std::chrono::milliseconds(0);
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = kCacheSize;

  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  // Step 1: Write an early entry
  CacheKey early_key("early-key-eviction-test");
  {
    std::vector<std::byte> content(kChunkSize, std::byte{0xAA});
    auto wh = cache->write_sync(early_key, content.size());
    REQUIRE(wh.has_value());
    wh->write_sync(std::span<const std::byte>(content));
    REQUIRE(wh->close_sync().has_value());
  }

  // Verify it's readable
  {
    auto exists = cache->exists_sync(early_key);
    REQUIRE(exists.has_value());
    REQUIRE(*exists == true);

    auto rh = cache->read_sync(early_key);
    REQUIRE(rh.has_value());
  }

  // Step 2: Fill the cache to trigger wraparound.
  // Write enough data to exceed the cache size, causing the write position
  // to wrap around and overwrite the region where early_key was stored.
  size_t num_fill_entries = (kCacheSize / kChunkSize) + 20;
  for (size_t i = 0; i < num_fill_entries; ++i) {
    std::string key_str = "fill-key-" + std::to_string(i);
    // Ignore write failures (bucket full is OK for this test)
    write_entry(*cache, key_str, kChunkSize);
  }

  // Step 3: After wraparound, the early key's disk region has been
  // overwritten. The directory entry is now stale. Phase-based eviction
  // should make it invisible.
  {
    auto exists = cache->exists_sync(early_key);
    REQUIRE(exists.has_value());
    REQUIRE(*exists == false);
  }

  {
    auto rh = cache->read_sync(early_key);
    REQUIRE_FALSE(rh.has_value());
    REQUIRE(rh.error() == CacheError::NotFound);
  }

  // Step 4: Verify that recently-written entries ARE still readable.
  // The last few entries should still be in the current phase.
  {
    std::string recent_key_str =
        "fill-key-" + std::to_string(num_fill_entries - 1);
    CacheKey recent_key(recent_key_str);
    auto exists = cache->exists_sync(recent_key);
    REQUIRE(exists.has_value());
    REQUIRE(*exists == true);
  }

  cache->stop();
}

TEST_CASE("Insert succeeds after phase-based eviction frees stale slots",
          "[eviction]") {
  // Verify that stale directory slots are reused by insert(), so new
  // entries can be written even after all bucket slots were previously full.
  constexpr size_t kCacheSizeMB = 8;
  constexpr size_t kCacheSize = kCacheSizeMB * 1024 * 1024;
  constexpr size_t kChunkSize = static_cast<const size_t>(64 * 1024);

  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheConfig config;
  config.ram_cache_size = 0;
  config.enable_checksum = false;
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = kCacheSize;

  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  // Fill the cache to trigger multiple wraparounds, stressing the directory.
  // With 4 entries per bucket and many entries, some buckets will be full.
  size_t num_fill_entries = (kCacheSize / kChunkSize) * 2;
  size_t write_successes = 0;
  for (size_t i = 0; i < num_fill_entries; ++i) {
    std::string key_str = "stress-fill-" + std::to_string(i);
    if (write_entry(*cache, key_str, kChunkSize)) {
      ++write_successes;
    }
  }

  // After wraparound + phase toggle, stale slots should be reclaimable.
  // Write a new batch and verify some succeed (stale slots reused).
  size_t post_eviction_successes = 0;
  for (size_t i = 0; i < 20; ++i) {
    std::string key_str = "post-eviction-" + std::to_string(i);
    if (write_entry(*cache, key_str, kChunkSize)) {
      ++post_eviction_successes;
    }
  }

  // At least some of the new writes should succeed, proving stale slots
  // were reclaimed rather than all buckets being permanently full.
  REQUIRE(post_eviction_successes > 0);

  // Verify the post-eviction entries are readable
  for (size_t i = 0; i < post_eviction_successes && i < 5; ++i) {
    std::string key_str = "post-eviction-" + std::to_string(i);
    CacheKey key(key_str);
    auto exists = cache->exists_sync(key);
    REQUIRE(exists.has_value());
    REQUIRE(*exists == true);
  }

  cache->stop();
}
