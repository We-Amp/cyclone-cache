// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "cyclone/cache.hpp"
#include "cyclone/key.hpp"
#include "support/temp_cache.hpp"

using namespace cyclone;

namespace {

// Helper: write a key-value pair into the cache.  Returns true on success.
bool write_entry(Cache &cache, const std::string &key_str,
                 const std::string &content) {
  CacheKey key(key_str);
  std::vector<std::byte> data(content.size());
  std::memcpy(data.data(), content.data(), content.size());

  auto wh = cache.write_sync(key, data.size());
  if (!wh.has_value()) {
    return false;
  }
  auto wr = wh->write_sync(std::span<const std::byte>(data));
  auto cr = wh->close_sync();
  return wr.has_value() && cr.has_value();
}

// Helper: read a key from the cache.  Returns the content string on hit,
// or std::nullopt on miss.
std::optional<std::string> read_entry(Cache &cache,
                                      const std::string &key_str) {
  CacheKey key(key_str);
  auto rh = cache.read_sync(key);
  if (!rh.has_value()) {
    return std::nullopt;
  }
  auto content = rh->content();
  if (content.empty()) {
    return std::string{};
  }
  return std::string(reinterpret_cast<const char *>(content.data()),
                     content.size());
}

// Helper: find two key strings that share the same 12-bit tag.
// They will map to different buckets (with overwhelming probability), so both
// can coexist in the directory simultaneously.  This tests that remove
// correctly distinguishes entries with the same tag but different full keys.
std::pair<std::string, std::string> find_tag_colliding_keys() {
  // Map from tag -> first key string that produced it
  std::unordered_map<uint16_t, std::string> seen;

  for (int i = 0; i < 100000; ++i) {
    std::string key_str = "col-" + std::to_string(i);
    CacheKey key(key_str);
    uint16_t tag = key.tag();

    auto it = seen.find(tag);
    if (it != seen.end()) {
      return {it->second, key_str};
    }
    seen[tag] = key_str;
  }
  // With 4096 possible tag values, collisions are guaranteed by ~100 keys
  FAIL("Could not find tag-colliding keys within 100K attempts");
  return {"", ""};
}

// Helper: create a started Cache with a single volume.  The TempCacheDir
// cleans up on destruction.  MEMBER ORDER IS LOAD-BEARING: `dir` must be
// declared BEFORE `cache` so destruction (reverse order) tears down the Cache
// -- closing/unmapping the volume files -- before remove_all runs.  With the
// order flipped, a failing REQUIRE unwinding mid-test would remove_all while
// the Cache still holds the mmapped files open, which silently fails on
// Windows (cannot delete a memory-mapped file) and leaks the temp dir on
// persistent runners.
struct TestCache {
  TempCacheDir dir;
  std::unique_ptr<Cache> cache;
};

TestCache make_cache(size_t size_mb) {
  TempCacheDir dir;

  CacheConfig config;
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());

  VolumeConfig vol_config;
  vol_config.path = dir.path();
  vol_config.size = size_mb * 1024 * 1024;

  REQUIRE((*cache_result)->add_volume(vol_config).has_value());
  REQUIRE((*cache_result)->start().has_value());

  return TestCache{std::move(dir), std::move(*cache_result)};
}

}  // namespace

// =============================================================================
// Test 1: Tag collision during remove
// =============================================================================

TEST_CASE("Tag collision during remove", "[remove][collision][regression]") {
  auto [dir, cache] = make_cache(50);

  // Find two keys that share the same 12-bit tag.  They will (almost
  // certainly) hash to different buckets, so the directory can hold both
  // simultaneously.  This exercises the Volume-level remove path which must
  // verify the full SHA-256 digest — not just the 12-bit tag — before
  // clearing a directory entry.
  auto [key_a_str, key_b_str] = find_tag_colliding_keys();

  INFO("Colliding key A: " << key_a_str);
  INFO("Colliding key B: " << key_b_str);

  // Verify the keys actually collide on tag
  CacheKey key_a(key_a_str);
  CacheKey key_b(key_b_str);
  REQUIRE(key_a.tag() == key_b.tag());
  // Sanity: full digests must differ (otherwise they're the same key)
  REQUIRE(key_a != key_b);

  // Write both entries
  REQUIRE(write_entry(*cache, key_a_str, "content-A"));
  REQUIRE(write_entry(*cache, key_b_str, "content-B"));

  // Verify both are readable before removal
  auto val_a = read_entry(*cache, key_a_str);
  auto val_b = read_entry(*cache, key_b_str);
  REQUIRE(val_a.has_value());
  REQUIRE(val_b.has_value());
  REQUIRE(*val_a == "content-A");
  REQUIRE(*val_b == "content-B");

  // Remove key A
  auto rm = cache->remove_sync(CacheKey(key_a_str));
  REQUIRE(rm.has_value());

  // Key A must be gone
  auto after_a = read_entry(*cache, key_a_str);
  REQUIRE_FALSE(after_a.has_value());

  // Key B must still be readable with correct content
  auto after_b = read_entry(*cache, key_b_str);
  REQUIRE(after_b.has_value());
  REQUIRE(*after_b == "content-B");

  cache->stop();
}

// =============================================================================
// Test 2: Remove precision — no collateral damage
// =============================================================================

TEST_CASE("Remove precision - no collateral damage",
          "[remove][precision][regression]") {
  auto [dir, cache] = make_cache(50);

  constexpr int num_entries = 50;
  constexpr int remove_index = 25;  // Remove one in the middle

  // Write all entries
  for (int i = 0; i < num_entries; ++i) {
    std::string key_str = "precision-key-" + std::to_string(i);
    std::string content = "precision-content-" + std::to_string(i);
    REQUIRE(write_entry(*cache, key_str, content));
  }

  // Remove entry at remove_index
  {
    CacheKey key("precision-key-" + std::to_string(remove_index));
    auto rm = cache->remove_sync(key);
    REQUIRE(rm.has_value());
  }

  // Verify removed entry is gone
  {
    auto val =
        read_entry(*cache, "precision-key-" + std::to_string(remove_index));
    REQUIRE_FALSE(val.has_value());
  }

  // Verify all other 49 entries are still readable with correct content
  for (int i = 0; i < num_entries; ++i) {
    if (i == remove_index) {
      continue;
    }
    std::string key_str = "precision-key-" + std::to_string(i);
    std::string expected = "precision-content-" + std::to_string(i);

    auto val = read_entry(*cache, key_str);
    INFO("Checking key index " << i);
    REQUIRE(val.has_value());
    REQUIRE(*val == expected);
  }

  cache->stop();
}

// =============================================================================
// Test 3: Remove all entries individually
// =============================================================================

TEST_CASE("Remove all entries individually", "[remove][stress][regression]") {
  auto [dir, cache] = make_cache(50);

  constexpr int num_entries = 200;

  // Write all entries
  for (int i = 0; i < num_entries; ++i) {
    std::string key_str = "removeall-key-" + std::to_string(i);
    std::string content = "removeall-content-" + std::to_string(i);
    REQUIRE(write_entry(*cache, key_str, content));
  }

  // Remove each one individually, verifying NotFound after each removal
  for (int i = 0; i < num_entries; ++i) {
    std::string key_str = "removeall-key-" + std::to_string(i);
    CacheKey key(key_str);

    auto rm = cache->remove_sync(key);
    REQUIRE(rm.has_value());

    // Immediately verify the removed entry is gone
    auto val = read_entry(*cache, key_str);
    INFO("Entry " << i << " should be NotFound after removal");
    REQUIRE_FALSE(val.has_value());
  }

  // After all removals, verify entry_count is 0
  auto stats = cache->stats();
  REQUIRE(stats.current_entries == 0);

  cache->stop();
}

// =============================================================================
// Test 4: Concurrent remove + read safety
// =============================================================================

TEST_CASE("Concurrent remove + read safety", "[remove][concurrent]") {
  auto [dir, cache] = make_cache(50);

  // Pre-populate with some entries
  constexpr int initial_entries = 200;
  for (int i = 0; i < initial_entries; ++i) {
    std::string key_str = "conc-key-" + std::to_string(i);
    std::string content = "conc-content-" + std::to_string(i);
    write_entry(*cache, key_str, content);
  }

  std::atomic<bool> stop_flag{false};
  std::atomic<int> writes_done{0};
  std::atomic<int> reads_done{0};
  std::atomic<int> reads_ok{0};
  std::atomic<int> removes_done{0};
  std::atomic<int> content_mismatches{0};

  // 4 writer threads: continuously write new entries
  auto writer = [&](int thread_id) {
    int counter = 0;
    while (!stop_flag.load(std::memory_order_relaxed)) {
      std::string key_str =
          "conc-w" + std::to_string(thread_id) + "-" + std::to_string(counter);
      std::string content =
          "wc-" + std::to_string(thread_id) + "-" + std::to_string(counter);
      write_entry(*cache, key_str, content);
      ++writes_done;
      ++counter;
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
  };

  // 4 reader threads: read entries and verify content when found
  auto reader = [&](int thread_id) {
    std::mt19937 rng(thread_id + 100);
    std::uniform_int_distribution<int> dist(0, initial_entries - 1);

    while (!stop_flag.load(std::memory_order_relaxed)) {
      int idx = dist(rng);
      std::string key_str = "conc-key-" + std::to_string(idx);
      std::string expected = "conc-content-" + std::to_string(idx);

      auto val = read_entry(*cache, key_str);
      ++reads_done;
      if (val.has_value()) {
        ++reads_ok;
        // If found, content must match (never get wrong data)
        if (*val != expected) {
          ++content_mismatches;
        }
      }
      // NotFound is acceptable (entry may have been removed)
    }
  };

  // 1 remover thread: remove entries
  auto remover = [&]() {
    for (int i = 0;
         i < initial_entries && !stop_flag.load(std::memory_order_relaxed);
         ++i) {
      CacheKey key("conc-key-" + std::to_string(i));
      cache->remove_sync(key);
      ++removes_done;
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
  };

  std::vector<std::thread> threads;
  threads.reserve(4);
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back(writer, t);
  }
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back(reader, t);
  }
  threads.emplace_back(remover);

  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  stop_flag.store(true);

  for (auto &t : threads) {
    t.join();
  }

  INFO("Writes: " << writes_done << ", Reads: " << reads_done
                  << ", Reads OK: " << reads_ok << ", Removes: " << removes_done
                  << ", Content mismatches: " << content_mismatches);

  // No content mismatches: readers never got wrong data
  REQUIRE(content_mismatches == 0);
  // Sanity: threads actually did work
  REQUIRE(writes_done > 0);
  REQUIRE(reads_done > 0);
  REQUIRE(removes_done > 0);

  // Stats should be internally consistent (no negative values, etc.)
  auto stats = cache->stats();
  INFO("Stats after concurrent test - entries: "
       << stats.current_entries << ", bytes: " << stats.current_bytes);

  cache->stop();
}

// =============================================================================
// Test 5: Remove + re-insert
// =============================================================================

TEST_CASE("Remove and re-insert with different content",
          "[remove][lifecycle]") {
  auto [dir, cache] = make_cache(50);

  std::string key_str = "lifecycle-key";

  // Write original content
  REQUIRE(write_entry(*cache, key_str, "original-content"));

  // Verify readable
  {
    auto val = read_entry(*cache, key_str);
    REQUIRE(val.has_value());
    REQUIRE(*val == "original-content");
  }

  // Remove
  {
    CacheKey key(key_str);
    auto rm = cache->remove_sync(key);
    REQUIRE(rm.has_value());
  }

  // Verify NotFound
  {
    auto val = read_entry(*cache, key_str);
    REQUIRE_FALSE(val.has_value());
  }

  // Re-insert with DIFFERENT content
  REQUIRE(write_entry(*cache, key_str, "new-content-after-remove"));

  // Read and verify NEW content is returned, not old
  {
    auto val = read_entry(*cache, key_str);
    REQUIRE(val.has_value());
    REQUIRE(*val == "new-content-after-remove");
  }

  cache->stop();
}

// =============================================================================
// Test 6: Bulk remove (purge-all simulation)
// =============================================================================

TEST_CASE("Bulk remove - purge all simulation", "[remove][stress][purge]") {
  auto [dir, cache] = make_cache(50);

  constexpr int num_entries = 500;

  // Write all entries
  for (int i = 0; i < num_entries; ++i) {
    std::string key_str = "purge-key-" + std::to_string(i);
    std::string content = "purge-content-" + std::to_string(i);
    REQUIRE(write_entry(*cache, key_str, content));
  }

  // Verify some entries are readable (spot check)
  for (int i = 0; i < num_entries; i += 50) {
    auto val = read_entry(*cache, "purge-key-" + std::to_string(i));
    REQUIRE(val.has_value());
  }

  // Remove all 500 entries
  for (int i = 0; i < num_entries; ++i) {
    CacheKey key("purge-key-" + std::to_string(i));
    auto rm = cache->remove_sync(key);
    REQUIRE(rm.has_value());
  }

  // Verify all return NotFound
  for (int i = 0; i < num_entries; ++i) {
    auto val = read_entry(*cache, "purge-key-" + std::to_string(i));
    INFO("Entry " << i << " should be gone after purge");
    REQUIRE_FALSE(val.has_value());
  }

  // Verify stats show 0 entries
  auto stats = cache->stats();
  REQUIRE(stats.current_entries == 0);

  cache->stop();
}

// =============================================================================
// Test 7: Remove non-existent key
// =============================================================================

TEST_CASE("Remove non-existent key", "[remove][edge]") {
  auto [dir, cache] = make_cache(50);

  // Try to remove a key that was never written
  CacheKey key("this-key-was-never-written");
  auto rm = cache->remove_sync(key);

  // Should return an error (NotFound), not crash
  REQUIRE_FALSE(rm.has_value());
  REQUIRE(rm.error() == CacheError::NotFound);

  cache->stop();
}

// =============================================================================
// Test 8: Remove key that has alternates
// =============================================================================

TEST_CASE("Remove key that has alternates", "[remove][alternate]") {
  auto [dir, cache] = make_cache(50);

  std::string key_str = "alt-remove-key";
  CacheKey key(key_str);

  // Write primary entry
  REQUIRE(write_entry(*cache, key_str, "primary-content"));

  // Write Brotli alternate
  {
    std::string alt_str = "brotli-alternate-content";
    std::vector<std::byte> alt_data(alt_str.size());
    std::memcpy(alt_data.data(), alt_str.data(), alt_str.size());

    auto wh =
        cache->write_alternate_sync(key, AlternateId::Brotli, alt_data.size());
    REQUIRE(wh.has_value());
    wh->write_sync(std::span<const std::byte>(alt_data));
    wh->close_sync();
  }

  // Write Gzip alternate
  {
    std::string alt_str = "gzip-alternate-content";
    std::vector<std::byte> alt_data(alt_str.size());
    std::memcpy(alt_data.data(), alt_str.data(), alt_str.size());

    auto wh =
        cache->write_alternate_sync(key, AlternateId::Gzip, alt_data.size());
    REQUIRE(wh.has_value());
    wh->write_sync(std::span<const std::byte>(alt_data));
    wh->close_sync();
  }

  // Verify all 3 alternates exist
  {
    auto alts = cache->list_alternates_sync(key);
    REQUIRE(alts.has_value());
    REQUIRE(alts->size() == 3);
  }

  // Remove the primary key
  auto rm = cache->remove_sync(key);
  REQUIRE(rm.has_value());

  // Primary read should return NotFound
  auto val = read_entry(*cache, key_str);
  REQUIRE_FALSE(val.has_value());

  // Document actual behavior: after remove_sync, the entire alternate chain
  // may or may not be cleaned up.  list_alternates_sync should either return
  // NotFound (chain fully purged) or an empty/reduced list.
  auto alts_after = cache->list_alternates_sync(key);
  INFO("After remove_sync: list_alternates returned "
       << (alts_after.has_value()
               ? std::to_string(alts_after->size()) + " alternates"
               : "NotFound"));
  // Either NotFound (whole chain removed) or the alternates are gone
  if (alts_after.has_value()) {
    // If the implementation keeps orphaned alternates, that's acceptable but
    // worth noting.  The primary entry must still be gone.
    auto primary_recheck = read_entry(*cache, key_str);
    REQUIRE_FALSE(primary_recheck.has_value());
  }

  cache->stop();
}

// =============================================================================
// Test 9: Multi-process ownership — total_processes=1 means all owned
// =============================================================================

TEST_CASE("Multi-process total_processes=1 means all stripes owned",
          "[remove][multiprocess]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheConfig config;
  config.set_multi_process(0, 1);
  config.set_enable_checksum(true);

  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(50 * 1024 * 1024);

  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  // Write entries — none should get NotOwned when total_processes == 1
  constexpr int num_entries = 50;
  for (int i = 0; i < num_entries; ++i) {
    std::string key_str = "mp1-key-" + std::to_string(i);
    std::string content = "mp1-content-" + std::to_string(i);

    CacheKey key(key_str);
    std::vector<std::byte> data(content.size());
    std::memcpy(data.data(), content.data(), content.size());

    auto wh = cache->write_sync(key, data.size());
    INFO("Write key " << i << " should not get NotOwned");
    REQUIRE(wh.has_value());

    wh->write_sync(std::span<const std::byte>(data));
    wh->close_sync();
  }

  // Remove all — none should get NotOwned
  for (int i = 0; i < num_entries; ++i) {
    CacheKey key("mp1-key-" + std::to_string(i));
    auto rm = cache->remove_sync(key);
    INFO("Remove key " << i << " should not get NotOwned");
    REQUIRE(rm.has_value());
  }

  // All entries should be gone
  for (int i = 0; i < num_entries; ++i) {
    CacheKey key("mp1-key-" + std::to_string(i));
    auto rh = cache->read_sync(key);
    REQUIRE_FALSE(rh.has_value());
  }

  cache->stop();
}

// =============================================================================
// Test 10: Multi-process ownership — non-owned stripe rejection
// =============================================================================

TEST_CASE("Multi-process non-owned stripe rejection on remove",
          "[remove][multiprocess]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheConfig config;
  config.set_multi_process(0, 2);
  config.set_enable_checksum(true);

  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(512 * 1024 * 1024);
  vol_config.stripe_size = static_cast<size_t>(
      128 * 1024 * 1024);  // 4 stripes, process 0 owns ~half

  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  // Categorize keys into owned vs non-owned by attempting writes
  std::vector<std::string> owned_keys;
  std::vector<std::string> non_owned_keys;

  for (int i = 0; i < 200; ++i) {
    std::string key_str = "mp2-key-" + std::to_string(i);
    CacheKey key(key_str);
    std::string content = "mp2-content-" + std::to_string(i);
    std::vector<std::byte> data(content.size());
    std::memcpy(data.data(), content.data(), content.size());

    auto wh = cache->write_sync(key, data.size());
    if (wh.has_value()) {
      wh->write_sync(std::span<const std::byte>(data));
      wh->close_sync();
      owned_keys.push_back(key_str);
    } else {
      REQUIRE(wh.error() == CacheError::NotOwned);
      non_owned_keys.push_back(key_str);
    }
  }

  // Sanity: with 2 processes and 4 stripes, we should see both owned and
  // non-owned
  INFO("Owned keys: " << owned_keys.size()
                      << ", Non-owned keys: " << non_owned_keys.size());
  REQUIRE_FALSE(owned_keys.empty());
  REQUIRE_FALSE(non_owned_keys.empty());

  // Remove owned keys — should all succeed
  for (const auto &key_str : owned_keys) {
    CacheKey key(key_str);
    auto rm = cache->remove_sync(key);
    INFO("Removing owned key: " << key_str);
    REQUIRE(rm.has_value());
  }

  // Attempt to remove non-owned keys — should all get NotOwned
  for (const auto &key_str : non_owned_keys) {
    CacheKey key(key_str);
    auto rm = cache->remove_sync(key);
    INFO("Removing non-owned key: " << key_str);
    REQUIRE_FALSE(rm.has_value());
    REQUIRE(rm.error() == CacheError::NotOwned);
  }

  cache->stop();
}

// =============================================================================
// Test 11: Purge then re-warm cache
// =============================================================================

TEST_CASE("Purge then re-warm cache with new content",
          "[remove][lifecycle][regression]") {
  auto [dir, cache] = make_cache(50);

  constexpr int num_entries = 100;

  // Phase 1: Write 100 entries with "old" content
  for (int i = 0; i < num_entries; ++i) {
    std::string key_str = "rewarm-key-" + std::to_string(i);
    std::string content = "old-content-" + std::to_string(i);
    REQUIRE(write_entry(*cache, key_str, content));
  }

  // Verify old content is readable (spot check)
  for (int i = 0; i < num_entries; i += 25) {
    auto val = read_entry(*cache, "rewarm-key-" + std::to_string(i));
    REQUIRE(val.has_value());
    REQUIRE(*val == "old-content-" + std::to_string(i));
  }

  // Phase 2: Purge all 100 entries
  for (int i = 0; i < num_entries; ++i) {
    CacheKey key("rewarm-key-" + std::to_string(i));
    auto rm = cache->remove_sync(key);
    REQUIRE(rm.has_value());
  }

  // Verify all are gone
  for (int i = 0; i < num_entries; ++i) {
    auto val = read_entry(*cache, "rewarm-key-" + std::to_string(i));
    INFO("Entry " << i << " should be gone after purge");
    REQUIRE_FALSE(val.has_value());
  }

  // Verify stats reflect the purge
  {
    auto stats = cache->stats();
    REQUIRE(stats.current_entries == 0);
  }

  // Phase 3: Re-warm with same keys but different content
  for (int i = 0; i < num_entries; ++i) {
    std::string key_str = "rewarm-key-" + std::to_string(i);
    std::string content = "new-content-" + std::to_string(i);
    REQUIRE(write_entry(*cache, key_str, content));
  }

  // Phase 4: Verify all 100 entries return the NEW content, not old
  for (int i = 0; i < num_entries; ++i) {
    std::string key_str = "rewarm-key-" + std::to_string(i);
    std::string expected = "new-content-" + std::to_string(i);

    auto val = read_entry(*cache, key_str);
    INFO("Re-warmed entry " << i << " should have new content");
    REQUIRE(val.has_value());
    REQUIRE(*val == expected);
  }

  // Verify entry count is correct after re-warm
  {
    auto stats = cache->stats();
    REQUIRE(stats.current_entries == num_entries);
  }

  cache->stop();
}

// =============================================================================
// Test 12: Revert-proof — remove after eviction returns NotFound
// =============================================================================

TEST_CASE("Remove after eviction returns NotFound, no collateral damage",
          "[remove][eviction][regression]") {
  // Use a VERY small cache (2 MB) so wraparound happens quickly.
  // Disable RAM cache so all reads go through the directory/disk path.
  // Disable checksums to avoid checksum failures on overwritten regions.
  constexpr size_t kCacheSizeMB = 2;
  constexpr size_t kCacheSize = kCacheSizeMB * 1024 * 1024;

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

  // Wave 1: write 20 entries with known content (~32 KB each, ~640 KB total)
  constexpr int kWave1Count = 20;
  constexpr int kWave2Count = 200;
  constexpr size_t kEntrySize =
      static_cast<const size_t>(32 * 1024);  // 32 KB per entry

  for (int i = 0; i < kWave1Count; ++i) {
    std::string key_str = "wave-1-key-" + std::to_string(i);
    std::string content(kEntrySize, 'A' + static_cast<char>(i % 26));
    REQUIRE(write_entry(*cache, key_str, content));
  }

  // Wave 2: write 200 entries to force the write cursor to wrap around
  // and overwrite wave-1 data.  200 * 32 KB = ~6.4 MB >> 2 MB cache.
  for (int i = 0; i < kWave2Count; ++i) {
    std::string key_str = "wave-2-key-" + std::to_string(i);
    std::string content(kEntrySize, 'a' + static_cast<char>(i % 26));
    // Some writes may fail (bucket full) — that's OK for this test.
    write_entry(*cache, key_str, content);
  }

  // Snapshot: which wave-2 entries are currently readable?
  // We record both the key and expected content for later verification.
  struct Wave2Snapshot {
    std::string key_str;
    std::string content;
  };
  std::vector<Wave2Snapshot> readable_wave2;
  for (int i = 0; i < kWave2Count; ++i) {
    std::string key_str = "wave-2-key-" + std::to_string(i);
    auto val = read_entry(*cache, key_str);
    if (val.has_value()) {
      readable_wave2.push_back({key_str, *val});
    }
  }

  INFO("Wave-2 entries readable before wave-1 removes: "
       << readable_wave2.size());
  // Sanity: at least some wave-2 entries should be readable
  REQUIRE(!readable_wave2.empty());

  // Now attempt to remove every wave-1 key.
  // With the fix: remove_sync reads the document at the stale offset, sees a
  // wave-2 key's SHA-256 there, and returns NotFound (correct — wave-1 data is
  // gone).  Some wave-1 keys might not have been evicted yet; those can be
  // removed successfully.
  //
  // Without the fix (reverted code): remove_sync would match the 12-bit tag,
  // blindly clear the directory entry, and return success — even though the
  // offset now points to a wave-2 document.  This makes that wave-2 entry
  // unreachable.
  int wave1_not_found = 0;
  int wave1_removed = 0;
  for (int i = 0; i < kWave1Count; ++i) {
    CacheKey key("wave-1-key-" + std::to_string(i));
    auto rm = cache->remove_sync(key);
    if (rm.has_value()) {
      ++wave1_removed;
    } else {
      REQUIRE(rm.error() == CacheError::NotFound);
      ++wave1_not_found;
    }
  }

  INFO("Wave-1 removes: " << wave1_removed << " succeeded, " << wave1_not_found
                          << " returned NotFound");
  // With a 2 MB cache and >6 MB of wave-2 data, most wave-1 entries should
  // have been evicted.  We expect at least some NotFound results.
  REQUIRE(wave1_not_found > 0);

  // THE CRITICAL CHECK: every wave-2 entry that was readable before the
  // wave-1 remove attempts must STILL be readable with the same content.
  // If the old (buggy) code were in place, some wave-2 entries would have
  // become unreachable because remove_sync blindly cleared their directory
  // entries when a stale wave-1 tag matched.
  int collateral_damage = 0;
  for (const auto &snap : readable_wave2) {
    auto val = read_entry(*cache, snap.key_str);
    if (!val.has_value() || *val != snap.content) {
      INFO("Collateral damage: "
           << snap.key_str << " was readable before wave-1 removes but is now "
           << (val.has_value() ? "corrupted" : "missing"));
      ++collateral_damage;
    }
  }

  REQUIRE(collateral_damage == 0);

  cache->stop();
}

// =============================================================================
// Test 13: remove_alternate_sync collision safety
// =============================================================================

TEST_CASE("remove_alternate_sync preserves other alternates",
          "[remove][alternate][regression]") {
  // This test exercises the remove_entry_at path in remove_alternate_sync.
  // After removing one specific alternate, the primary entry and the other
  // alternate must remain intact and readable.
  auto [dir, cache] = make_cache(50);

  std::string key_str = "alt-collision-safety-key";
  CacheKey key(key_str);

  // Write primary (Original) entry
  {
    std::string content = "primary-original-content";
    std::vector<std::byte> data(content.size());
    std::memcpy(data.data(), content.data(), content.size());

    auto wh = cache->write_sync(key, data.size());
    REQUIRE(wh.has_value());
    wh->write_sync(std::span<const std::byte>(data));
    REQUIRE(wh->close_sync().has_value());
  }

  // Write Gzip alternate
  {
    std::string content = "gzip-alternate-content";
    std::vector<std::byte> data(content.size());
    std::memcpy(data.data(), content.data(), content.size());

    auto wh = cache->write_alternate_sync(key, AlternateId::Gzip, data.size());
    REQUIRE(wh.has_value());
    wh->write_sync(std::span<const std::byte>(data));
    REQUIRE(wh->close_sync().has_value());
  }

  // Write Brotli alternate
  {
    std::string content = "brotli-alternate-content";
    std::vector<std::byte> data(content.size());
    std::memcpy(data.data(), content.data(), content.size());

    auto wh =
        cache->write_alternate_sync(key, AlternateId::Brotli, data.size());
    REQUIRE(wh.has_value());
    wh->write_sync(std::span<const std::byte>(data));
    REQUIRE(wh->close_sync().has_value());
  }

  // Verify all 3 alternates exist: Brotli (head) -> Gzip -> Original
  {
    auto alts = cache->list_alternates_sync(key);
    REQUIRE(alts.has_value());
    REQUIRE(alts->size() == 3);
  }

  // Remove the Gzip alternate specifically
  {
    auto result = cache->remove_alternate_sync(key, AlternateId::Gzip);
    REQUIRE(result.has_value());
  }

  // Verify the chain now has exactly 2 alternates: Brotli and Original
  {
    auto alts = cache->list_alternates_sync(key);
    REQUIRE(alts.has_value());
    REQUIRE(alts->size() == 2);

    // Check that Gzip is gone and Brotli + Original remain
    bool has_brotli = false;
    bool has_original = false;
    bool has_gzip = false;
    for (const auto &alt : *alts) {
      if (alt.id == AlternateId::Brotli) has_brotli = true;
      if (alt.id == AlternateId::Original) has_original = true;
      if (alt.id == AlternateId::Gzip) has_gzip = true;
    }
    REQUIRE(has_brotli);
    REQUIRE(has_original);
    REQUIRE_FALSE(has_gzip);
  }

  // Verify the remaining alternates have correct content sizes.
  // read_sync returns the head of the chain (Brotli after Gzip removal),
  // so we verify via list_alternates_sync content lengths.
  {
    auto alts = cache->list_alternates_sync(key);
    REQUIRE(alts.has_value());
    for (const auto &alt : *alts) {
      if (alt.id == AlternateId::Brotli) {
        REQUIRE(alt.content_length ==
                std::string("brotli-alternate-content").size());
      } else if (alt.id == AlternateId::Original) {
        REQUIRE(alt.content_length ==
                std::string("primary-original-content").size());
      }
    }
  }

  // Verify read_sync returns the head alternate (Brotli) with correct content
  {
    auto val = read_entry(*cache, key_str);
    REQUIRE(val.has_value());
    REQUIRE(*val == "brotli-alternate-content");
  }

  // Verify removing an already-removed alternate returns AlternateNotFound
  {
    auto result = cache->remove_alternate_sync(key, AlternateId::Gzip);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == CacheError::AlternateNotFound);
  }

  cache->stop();
}
