// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <thread>
#include <vector>

#include "../../src/ram_cache/ram_cache.hpp"

using namespace cyclone;

TEST_CASE("CLFUS basic put and get", "[clfus]") {
  auto cache =
      RamCache::create(RamCacheType::CLFUS, static_cast<size_t>(1024 * 1024));

  CacheKey key("test-key");
  std::vector<std::byte> data(100, std::byte{0x42});

  cache->put(key, AlternateId::Original, data);
  cache->put(key, AlternateId::Original, data);

  auto result = cache->get(key, AlternateId::Original);
  REQUIRE(result.has_value());
  REQUIRE(result->size() == data.size());
}

TEST_CASE("CLFUS seen filter (scan resistance)", "[clfus]") {
  auto cache =
      RamCache::create(RamCacheType::CLFUS, static_cast<size_t>(1024 * 1024));

  CacheKey key("new-key");
  std::vector<std::byte> data(100, std::byte{0x42});

  bool first_put = cache->put(key, AlternateId::Original, data);
  REQUIRE_FALSE(first_put);

  bool second_put = cache->put(key, AlternateId::Original, data);
  REQUIRE(second_put);

  auto result = cache->get(key, AlternateId::Original);
  REQUIRE(result.has_value());
}

TEST_CASE("CLFUS put_if inserts only when the predicate passes", "[clfus]") {
  auto cache =
      RamCache::create(RamCacheType::CLFUS, static_cast<size_t>(1024 * 1024));

  CacheKey key("cond-key");
  std::vector<std::byte> data(100, std::byte{0x42});
  std::vector<std::byte> newer(100, std::byte{0x77});

  auto reject = [] { return false; };
  auto admit = [] { return true; };

  // A rejected put neither inserts NOR marks the key as seen: the next
  // admitted put is still the first sighting and only feeds the filter.
  REQUIRE_FALSE(cache->put_if(key, AlternateId::Original, data, reject));
  REQUIRE_FALSE(cache->get(key, AlternateId::Original).has_value());
  REQUIRE_FALSE(cache->put_if(key, AlternateId::Original, data, admit));
  REQUIRE_FALSE(cache->get(key, AlternateId::Original).has_value());

  // Second admitted put: the filter passes and the entry is inserted.
  REQUIRE(cache->put_if(key, AlternateId::Original, data, admit));
  auto result = cache->get(key, AlternateId::Original);
  REQUIRE(result.has_value());
  REQUIRE((*result)[0] == std::byte{0x42});

  // A rejected put on an EXISTING entry leaves the old bytes untouched.
  REQUIRE_FALSE(cache->put_if(key, AlternateId::Original, newer, reject));
  result = cache->get(key, AlternateId::Original);
  REQUIRE(result.has_value());
  REQUIRE((*result)[0] == std::byte{0x42});
}

TEST_CASE("CLFUS eviction", "[clfus]") {
  auto cache = RamCache::create(RamCacheType::CLFUS, 1024);

  for (int i = 0; i < 100; ++i) {
    CacheKey key("key-" + std::to_string(i));
    std::vector<std::byte> data(50, std::byte(i));

    cache->put(key, AlternateId::Original, data);
    cache->put(key, AlternateId::Original, data);
  }

  REQUIRE(cache->bytes_used() <= 1024);

  auto stats = cache->stats();
  REQUIRE(stats.evictions > 0);
}

TEST_CASE("CLFUS hit count affects eviction", "[clfus]") {
  auto cache = RamCache::create(RamCacheType::CLFUS, 500);

  CacheKey hot_key("hot");
  CacheKey cold_key("cold");
  std::vector<std::byte> data(100, std::byte{0x00});

  cache->put(hot_key, AlternateId::Original, data);
  cache->put(hot_key, AlternateId::Original, data);

  for (int i = 0; i < 10; ++i) {
    cache->get(hot_key, AlternateId::Original);
  }

  cache->put(cold_key, AlternateId::Original, data);
  cache->put(cold_key, AlternateId::Original, data);

  for (int i = 0; i < 10; ++i) {
    CacheKey filler("filler-" + std::to_string(i));
    cache->put(filler, AlternateId::Original, data);
    cache->put(filler, AlternateId::Original, data);
  }

  auto hot_result = cache->get(hot_key, AlternateId::Original);
  REQUIRE(hot_result.has_value());
}

TEST_CASE("CLFUS remove", "[clfus]") {
  auto cache =
      RamCache::create(RamCacheType::CLFUS, static_cast<size_t>(1024 * 1024));

  CacheKey key("remove-test");
  std::vector<std::byte> data(100, std::byte{0x42});

  cache->put(key, AlternateId::Original, data);
  cache->put(key, AlternateId::Original, data);

  REQUIRE(cache->get(key, AlternateId::Original).has_value());

  REQUIRE(cache->remove(key, AlternateId::Original));
  REQUIRE_FALSE(cache->get(key, AlternateId::Original).has_value());
}

TEST_CASE("CLFUS clear", "[clfus]") {
  auto cache =
      RamCache::create(RamCacheType::CLFUS, static_cast<size_t>(1024 * 1024));

  for (int i = 0; i < 10; ++i) {
    CacheKey key("key-" + std::to_string(i));
    std::vector<std::byte> data(100, std::byte(i));
    cache->put(key, AlternateId::Original, data);
    cache->put(key, AlternateId::Original, data);
  }

  REQUIRE(cache->entry_count() > 0);

  cache->clear();

  REQUIRE(cache->entry_count() == 0);
  REQUIRE(cache->bytes_used() == 0);
}

TEST_CASE("CLFUS multiple alternates per key", "[clfus]") {
  auto cache =
      RamCache::create(RamCacheType::CLFUS, static_cast<size_t>(1024 * 1024));

  CacheKey key("multi-alt");
  std::vector<std::byte> webp_data(100, std::byte{0xAA});
  std::vector<std::byte> avif_data(200, std::byte{0xBB});
  std::vector<std::byte> orig_data(150, std::byte{0xCC});

  // Write three alternates for the same key
  cache->put(key, AlternateId::WebP, webp_data);
  cache->put(key, AlternateId::WebP, webp_data);
  cache->put(key, AlternateId::AVIF, avif_data);
  cache->put(key, AlternateId::AVIF, avif_data);
  cache->put(key, AlternateId::Original, orig_data);
  cache->put(key, AlternateId::Original, orig_data);

  // Each alternate should be independently retrievable
  auto webp = cache->get(key, AlternateId::WebP);
  REQUIRE(webp.has_value());
  REQUIRE(webp->size() == 100);
  REQUIRE((*webp)[0] == std::byte{0xAA});

  auto avif = cache->get(key, AlternateId::AVIF);
  REQUIRE(avif.has_value());
  REQUIRE(avif->size() == 200);
  REQUIRE((*avif)[0] == std::byte{0xBB});

  auto orig = cache->get(key, AlternateId::Original);
  REQUIRE(orig.has_value());
  REQUIRE(orig->size() == 150);
  REQUIRE((*orig)[0] == std::byte{0xCC});

  // Remove one alternate — others stay
  cache->remove(key, AlternateId::AVIF);
  REQUIRE_FALSE(cache->get(key, AlternateId::AVIF).has_value());
  REQUIRE(cache->get(key, AlternateId::WebP).has_value());
  REQUIRE(cache->get(key, AlternateId::Original).has_value());
}

TEST_CASE("CLFUS concurrent entry_count safety", "[clfus][concurrent]") {
  auto cache =
      RamCache::create(RamCacheType::CLFUS, static_cast<size_t>(1024 * 1024));

  constexpr int kNumWriters = 4;
  constexpr int kOpsPerWriter = 500;
  std::atomic<bool> stop_flag{false};
  std::atomic<size_t> reader_iterations{0};

  // Reader thread: repeatedly call entry_count()
  std::thread reader([&]() {
    while (!stop_flag.load(std::memory_order_relaxed)) {
      size_t count = cache->entry_count();
      (void)count;
      ++reader_iterations;
    }
  });

  // Writer threads: put entries concurrently
  std::vector<std::thread> writers;
  writers.reserve(kNumWriters);
  for (int i = 0; i < kNumWriters; ++i) {
    writers.emplace_back([&, i]() {
      for (int j = 0; j < kOpsPerWriter; ++j) {
        CacheKey key("concurrent-" + std::to_string(i) + "-" +
                     std::to_string(j));
        std::vector<std::byte> data(50, std::byte(j & 0xFF));
        cache->put(key, AlternateId::Original, data);
        cache->put(key, AlternateId::Original, data);
      }
    });
  }

  for (auto& t : writers) {
    t.join();
  }

  stop_flag.store(true);
  reader.join();

  // Verify reader ran a reasonable number of iterations
  REQUIRE(reader_iterations.load() > 0);

  // entry_count should be consistent
  size_t count = cache->entry_count();
  REQUIRE(count > 0);
  REQUIRE(count <= static_cast<size_t>(kNumWriters * kOpsPerWriter));
}

TEST_CASE("CLFUS remove_all", "[clfus]") {
  auto cache =
      RamCache::create(RamCacheType::CLFUS, static_cast<size_t>(1024 * 1024));

  CacheKey key("remove-all-test");
  std::vector<std::byte> data(50, std::byte{0x42});

  cache->put(key, AlternateId::Original, data);
  cache->put(key, AlternateId::Original, data);
  cache->put(key, AlternateId::WebP, data);
  cache->put(key, AlternateId::WebP, data);
  cache->put(key, AlternateId::AVIF, data);
  cache->put(key, AlternateId::AVIF, data);

  REQUIRE(cache->get(key, AlternateId::Original).has_value());
  REQUIRE(cache->get(key, AlternateId::WebP).has_value());
  REQUIRE(cache->get(key, AlternateId::AVIF).has_value());

  cache->remove_all(key);

  REQUIRE_FALSE(cache->get(key, AlternateId::Original).has_value());
  REQUIRE_FALSE(cache->get(key, AlternateId::WebP).has_value());
  REQUIRE_FALSE(cache->get(key, AlternateId::AVIF).has_value());
}

// =============================================================================
// Phase 6A: CLFUS Seen Filter and Eviction Behavior
// =============================================================================

TEST_CASE("CLFUS seen filter requires two puts", "[clfus]") {
  auto cache =
      RamCache::create(RamCacheType::CLFUS, static_cast<size_t>(1024 * 1024));

  CacheKey key("seen-filter-test-key");
  std::vector<std::byte> data(100, std::byte{0xAB});

  // First put for a completely new key should NOT be stored (seen filter)
  bool first_put = cache->put(key, AlternateId::Original, data);
  REQUIRE_FALSE(first_put);

  // After first put, get should return nothing (data was not stored)
  auto result1 = cache->get(key, AlternateId::Original);
  REQUIRE_FALSE(result1.has_value());

  // Second put for the same key should succeed (key is now "seen")
  bool second_put = cache->put(key, AlternateId::Original, data);
  REQUIRE(second_put);

  // Now get should return the data
  auto result2 = cache->get(key, AlternateId::Original);
  REQUIRE(result2.has_value());
  REQUIRE(result2->size() == data.size());
  REQUIRE((*result2)[0] == std::byte{0xAB});
}

TEST_CASE("CLFUS eviction preserves high-hit entries", "[clfus]") {
  // Create a small CLFUS cache (2KB)
  auto cache = RamCache::create(RamCacheType::CLFUS, 2048);

  CacheKey hot_key("hot-entry");
  std::vector<std::byte> hot_data(100, std::byte{0xFF});

  // Insert the hot entry (requires two puts due to seen filter)
  cache->put(hot_key, AlternateId::Original, hot_data);
  cache->put(hot_key, AlternateId::Original, hot_data);

  // Read the hot entry many times to accumulate hits
  for (int i = 0; i < 50; ++i) {
    auto result = cache->get(hot_key, AlternateId::Original);
    REQUIRE(result.has_value());
  }

  // Now fill the cache with many other entries to trigger eviction
  for (int i = 0; i < 100; ++i) {
    CacheKey filler_key("filler-" + std::to_string(i));
    std::vector<std::byte> filler_data(80, std::byte(i & 0xFF));

    // Two puts needed (seen filter)
    cache->put(filler_key, AlternateId::Original, filler_data);
    cache->put(filler_key, AlternateId::Original, filler_data);
  }

  // The hot entry should survive eviction due to its high hit count
  auto hot_result = cache->get(hot_key, AlternateId::Original);
  REQUIRE(hot_result.has_value());
  REQUIRE(hot_result->size() == hot_data.size());

  // Verify cache size stays within limits
  REQUIRE(cache->bytes_used() <= 2048);

  // Verify evictions occurred
  auto stats = cache->stats();
  REQUIRE(stats.evictions > 0);
}

TEST_CASE("CLFUS overwriting an entry in place enforces the byte budget",
          "[clfus]") {
  // A growing overwrite adjusts the accounted bytes but must also evict:
  // a workload that only overwrites existing keys can otherwise push the
  // cache past its configured budget without limit.
  auto cache = RamCache::create(RamCacheType::CLFUS, 10000);

  CacheKey a("overwrite-a");
  CacheKey b("overwrite-b");
  std::vector<std::byte> small(100, std::byte{0x01});
  std::vector<std::byte> large(9950, std::byte{0x02});

  cache->put(a, AlternateId::Original, small);
  cache->put(a, AlternateId::Original, small);
  cache->put(b, AlternateId::Original, small);
  cache->put(b, AlternateId::Original, small);
  REQUIRE(cache->bytes_used() == 200);

  REQUIRE(cache->put(a, AlternateId::Original, large));

  REQUIRE(cache->bytes_used() <= cache->max_bytes());
  auto result = cache->get(a, AlternateId::Original);
  REQUIRE(result.has_value());
  REQUIRE(result->size() == large.size());
}

TEST_CASE("CLFUS overwrite does not evict the entry it just wrote", "[clfus]") {
  // Eviction requeues hit-bearing tail entries to the FRONT of the active
  // list, and every requeue pushes the just-updated front entry one step
  // toward the tail.  With enough requeues it reaches the tail itself and
  // is evicted by its own put() while put() reports success.  The updated
  // entry must stay off the list while eviction makes room for it.
  auto cache = RamCache::create(RamCacheType::CLFUS, 10000);

  CacheKey a("overwrite-evict-a");
  CacheKey b("overwrite-evict-b");
  CacheKey c("overwrite-evict-c");
  std::vector<std::byte> small(3000, std::byte{0x01});
  std::vector<std::byte> large(4500, std::byte{0x02});

  cache->put(a, AlternateId::Original, small);
  cache->put(a, AlternateId::Original, small);
  cache->put(b, AlternateId::Original, small);
  cache->put(b, AlternateId::Original, small);
  cache->put(c, AlternateId::Original, small);
  cache->put(c, AlternateId::Original, small);

  for (int i = 0; i < 10; ++i) {
    cache->get(b, AlternateId::Original);
    cache->get(c, AlternateId::Original);
  }

  REQUIRE(cache->put(a, AlternateId::Original, large));

  auto result = cache->get(a, AlternateId::Original);
  REQUIRE(result.has_value());
  REQUIRE(result->size() == large.size());
  REQUIRE((*result)[0] == std::byte{0x02});
  REQUIRE(cache->bytes_used() <= cache->max_bytes());
}

TEST_CASE("CLFUS promoting the oldest history entry under eviction pressure",
          "[clfus]") {
  // One segment (cache below the per-segment budget floor), history at its
  // cap, and the first-evicted key still at the history tail.  Promoting
  // that tail key evicts an active entry to make room, and that eviction
  // trims the history tail: the very node the promotion is holding.  The
  // promotion must not touch freed nodes (heap-use-after-free under
  // sanitizers, list corruption otherwise).
  constexpr size_t kEntryBytes = 100;
  constexpr int kHistoryCap = 1024;
  auto cache = RamCache::create(RamCacheType::CLFUS, kEntryBytes * kHistoryCap);

  // Fill past capacity: kHistoryCap evictions fill history to its cap,
  // leaving the first key ("hist-0") at the tail.
  for (int i = 0; i < 2 * kHistoryCap; ++i) {
    CacheKey key("hist-" + std::to_string(i));
    std::vector<std::byte> data(kEntryBytes, std::byte(i & 0xFF));
    cache->put(key, AlternateId::Original, data);
    cache->put(key, AlternateId::Original, data);
  }
  REQUIRE(cache->stats().evictions >= kHistoryCap);

  // Re-reference the history tail.  The value of a history entry beats the
  // average of evicted entries, so this takes the promotion path.
  CacheKey tail("hist-0");
  std::vector<std::byte> fresh(kEntryBytes, std::byte{0x5A});
  REQUIRE(cache->put(tail, AlternateId::Original, fresh));

  auto result = cache->get(tail, AlternateId::Original);
  REQUIRE(result.has_value());
  REQUIRE(result->size() == kEntryBytes);
  REQUIRE((*result)[0] == std::byte{0x5A});
  REQUIRE(cache->bytes_used() <= cache->max_bytes());
}

TEST_CASE("CLFUS concurrent get+put data race safety", "[clfus][concurrent]") {
  auto cache =
      RamCache::create(RamCacheType::CLFUS, static_cast<size_t>(1024 * 1024));

  CacheKey key("race-key");
  std::vector<std::byte> initial_data(200, std::byte{0xAA});

  // Seed the entry (two puts for seen filter)
  cache->put(key, AlternateId::Original, initial_data);
  cache->put(key, AlternateId::Original, initial_data);

  constexpr int kIterations = 5000;
  std::atomic<bool> stop_flag{false};
  std::atomic<size_t> read_count{0};
  std::atomic<size_t> valid_reads{0};

  // Reader threads: get() and read the returned data
  auto reader_fn = [&]() {
    while (!stop_flag.load(std::memory_order_relaxed)) {
      auto data = cache->get(key, AlternateId::Original);
      ++read_count;
      if (data) {
        ++valid_reads;
        // Access every byte to trigger TSan on any race
        std::byte checksum{0};
        for (auto b : *data) {
          checksum ^= b;
        }
        (void)checksum;
      }
    }
  };

  // Writer thread: put() with different data sizes
  auto writer_fn = [&]() {
    for (int i = 0; i < kIterations; ++i) {
      size_t sz = 100 + (i % 300);
      std::vector<std::byte> new_data(sz, std::byte(i & 0xFF));
      cache->put(key, AlternateId::Original, new_data);
    }
  };

  std::vector<std::thread> readers;
  readers.reserve(4);
  for (int i = 0; i < 4; ++i) {
    readers.emplace_back(reader_fn);
  }
  std::thread writer(writer_fn);

  writer.join();
  stop_flag.store(true);
  for (auto& t : readers) {
    t.join();
  }

  REQUIRE(read_count.load() > 0);
  REQUIRE(valid_reads.load() > 0);
}

TEST_CASE("RAM cache hit counters saturate instead of wrapping", "[clfus]") {
  std::atomic<uint32_t> counter{0};
  saturating_increment(counter);
  REQUIRE(counter.load() == 1);

  counter.store(UINT32_MAX - 1);
  saturating_increment(counter);
  REQUIRE(counter.load() == UINT32_MAX);
  saturating_increment(counter);
  REQUIRE(counter.load() == UINT32_MAX);
}
