// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <thread>
#include <vector>

#include "../../src/ram_cache/ram_cache.hpp"

using namespace cyclone;

TEST_CASE("LRU basic put and get", "[lru]") {
  auto cache =
      RamCache::create(RamCacheType::LRU, static_cast<size_t>(1024 * 1024));

  CacheKey key("test-key");
  std::vector<std::byte> data(100, std::byte{0x42});

  REQUIRE(cache->put(key, AlternateId::Original, data));

  auto result = cache->get(key, AlternateId::Original);
  REQUIRE(result.has_value());
  REQUIRE(result->size() == data.size());
}

TEST_CASE("LRU eviction order", "[lru]") {
  auto cache = RamCache::create(RamCacheType::LRU, 300);

  CacheKey key1("key1");
  CacheKey key2("key2");
  CacheKey key3("key3");
  std::vector<std::byte> data(100, std::byte{0x42});

  cache->put(key1, AlternateId::Original, data);
  cache->put(key2, AlternateId::Original, data);
  cache->put(key3, AlternateId::Original, data);

  cache->get(key1, AlternateId::Original);

  CacheKey key4("key4");
  cache->put(key4, AlternateId::Original, data);

  REQUIRE(cache->get(key1, AlternateId::Original).has_value());
  REQUIRE_FALSE(cache->get(key2, AlternateId::Original).has_value());
  REQUIRE(cache->get(key3, AlternateId::Original).has_value());
}

TEST_CASE("LRU update existing", "[lru]") {
  auto cache =
      RamCache::create(RamCacheType::LRU, static_cast<size_t>(1024 * 1024));

  CacheKey key("test-key");
  std::vector<std::byte> data1(100, std::byte{0x11});
  std::vector<std::byte> data2(200, std::byte{0x22});

  cache->put(key, AlternateId::Original, data1);
  cache->put(key, AlternateId::Original, data2);

  auto result = cache->get(key, AlternateId::Original);
  REQUIRE(result.has_value());
  REQUIRE(result->size() == data2.size());
  REQUIRE((*result)[0] == std::byte{0x22});
}

TEST_CASE("LRU remove", "[lru]") {
  auto cache =
      RamCache::create(RamCacheType::LRU, static_cast<size_t>(1024 * 1024));

  CacheKey key("test-key");
  std::vector<std::byte> data(100, std::byte{0x42});

  cache->put(key, AlternateId::Original, data);
  REQUIRE(cache->get(key, AlternateId::Original).has_value());

  REQUIRE(cache->remove(key, AlternateId::Original));
  REQUIRE_FALSE(cache->get(key, AlternateId::Original).has_value());

  REQUIRE_FALSE(cache->remove(key, AlternateId::Original));
}

TEST_CASE("LRU clear", "[lru]") {
  auto cache =
      RamCache::create(RamCacheType::LRU, static_cast<size_t>(1024 * 1024));

  for (int i = 0; i < 10; ++i) {
    CacheKey key("key-" + std::to_string(i));
    std::vector<std::byte> data(100, std::byte(i));
    cache->put(key, AlternateId::Original, data);
  }

  REQUIRE(cache->entry_count() == 10);

  cache->clear();

  REQUIRE(cache->entry_count() == 0);
  REQUIRE(cache->bytes_used() == 0);
}

TEST_CASE("LRU stats", "[lru]") {
  auto cache =
      RamCache::create(RamCacheType::LRU, static_cast<size_t>(1024 * 1024));

  CacheKey key("test");
  std::vector<std::byte> data(100, std::byte{0x42});

  cache->put(key, AlternateId::Original, data);
  cache->get(key, AlternateId::Original);
  cache->get(key, AlternateId::Original);

  CacheKey missing("missing");
  cache->get(missing, AlternateId::Original);

  auto stats = cache->stats();
  REQUIRE(stats.hits == 2);
  REQUIRE(stats.misses == 1);
  REQUIRE(stats.bytes_used == 100);
  REQUIRE(stats.entry_count == 1);
}

TEST_CASE("LRU multiple alternates per key", "[lru]") {
  auto cache =
      RamCache::create(RamCacheType::LRU, static_cast<size_t>(1024 * 1024));

  CacheKey key("multi-alt");
  std::vector<std::byte> webp_data(100, std::byte{0xAA});
  std::vector<std::byte> avif_data(200, std::byte{0xBB});

  cache->put(key, AlternateId::WebP, webp_data);
  cache->put(key, AlternateId::AVIF, avif_data);

  // Both alternates should coexist
  auto webp = cache->get(key, AlternateId::WebP);
  REQUIRE(webp.has_value());
  REQUIRE(webp->size() == 100);
  REQUIRE((*webp)[0] == std::byte{0xAA});

  auto avif = cache->get(key, AlternateId::AVIF);
  REQUIRE(avif.has_value());
  REQUIRE(avif->size() == 200);
  REQUIRE((*avif)[0] == std::byte{0xBB});

  // Different CacheKey, same AlternateId — independent
  CacheKey key2("other-url");
  REQUIRE_FALSE(cache->get(key2, AlternateId::WebP).has_value());
}

TEST_CASE("LRU put_if inserts only when the predicate passes", "[lru]") {
  auto cache =
      RamCache::create(RamCacheType::LRU, static_cast<size_t>(1024 * 1024));

  CacheKey key("cond-key");
  std::vector<std::byte> data(100, std::byte{0x42});
  std::vector<std::byte> newer(100, std::byte{0x77});

  int calls = 0;
  auto reject = [&] {
    ++calls;
    return false;
  };
  auto admit = [&] {
    ++calls;
    return true;
  };

  // A rejected put neither inserts nor marks anything.
  REQUIRE_FALSE(cache->put_if(key, AlternateId::Original, data, reject));
  REQUIRE(calls == 1);
  REQUIRE_FALSE(cache->get(key, AlternateId::Original).has_value());

  // An admitted put inserts and serves.
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

TEST_CASE("LRU remove_all", "[lru]") {
  auto cache =
      RamCache::create(RamCacheType::LRU, static_cast<size_t>(1024 * 1024));

  CacheKey key("remove-all");
  std::vector<std::byte> data(50, std::byte{0x42});

  cache->put(key, AlternateId::Original, data);
  cache->put(key, AlternateId::WebP, data);
  cache->put(key, AlternateId::AVIF, data);

  // All three present
  REQUIRE(cache->entry_count() == 3);

  cache->remove_all(key);

  REQUIRE_FALSE(cache->get(key, AlternateId::Original).has_value());
  REQUIRE_FALSE(cache->get(key, AlternateId::WebP).has_value());
  REQUIRE_FALSE(cache->get(key, AlternateId::AVIF).has_value());
  REQUIRE(cache->entry_count() == 0);
}

TEST_CASE("LRU concurrent get+put data race safety", "[lru][concurrent]") {
  auto cache =
      RamCache::create(RamCacheType::LRU, static_cast<size_t>(1024 * 1024));

  CacheKey key("race-key");
  std::vector<std::byte> initial_data(200, std::byte{0xBB});

  cache->put(key, AlternateId::Original, initial_data);

  constexpr int kIterations = 5000;
  std::atomic<bool> stop_flag{false};
  std::atomic<size_t> read_count{0};
  std::atomic<size_t> valid_reads{0};

  auto reader_fn = [&]() {
    while (!stop_flag.load(std::memory_order_relaxed)) {
      auto data = cache->get(key, AlternateId::Original);
      ++read_count;
      if (data) {
        ++valid_reads;
        std::byte checksum{0};
        for (auto b : *data) {
          checksum ^= b;
        }
        (void)checksum;
      }
    }
  };

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
