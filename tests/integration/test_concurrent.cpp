// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include "cyclone/cache.hpp"
#include "cyclone/key.hpp"
#include "support/temp_cache.hpp"

using namespace cyclone;

TEST_CASE("Concurrent writes from multiple threads",
          "[concurrent][integration]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheConfig config;
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(50 * 1024 * 1024);

  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  constexpr int num_threads = 4;
  constexpr int writes_per_thread = 50;

  std::atomic<int> successful_writes{0};
  std::atomic<int> failed_writes{0};

  auto writer = [&](int thread_id) {
    for (int i = 0; i < writes_per_thread; ++i) {
      CacheKey key("thread-" + std::to_string(thread_id) + "-key-" +
                   std::to_string(i));
      std::string content = "Content from thread " + std::to_string(thread_id) +
                            " item " + std::to_string(i);

      std::vector<std::byte> data(content.size());
      std::memcpy(data.data(), content.data(), content.size());

      auto wh = cache->write_sync(key, data.size());
      if (wh.has_value()) {
        auto write_result = wh->write_sync(std::span<const std::byte>(data));
        auto close_result = wh->close_sync();
        if (write_result.has_value() && close_result.has_value()) {
          ++successful_writes;
        } else {
          ++failed_writes;
        }
      } else {
        ++failed_writes;
      }
    }
  };

  std::vector<std::thread> threads;
  threads.reserve(num_threads);
  for (int t = 0; t < num_threads; ++t) {
    threads.emplace_back(writer, t);
  }

  for (auto &t : threads) {
    t.join();
  }

  REQUIRE(successful_writes > 0);
  INFO("Successful writes: " << successful_writes
                             << ", Failed: " << failed_writes);

  cache->stop();
}

TEST_CASE("Concurrent reads from multiple threads",
          "[concurrent][integration]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheConfig config;
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(50 * 1024 * 1024);

  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  constexpr int num_entries = 100;
  for (int i = 0; i < num_entries; ++i) {
    CacheKey key("read-test-key-" + std::to_string(i));
    std::string content = "Content for entry " + std::to_string(i);

    std::vector<std::byte> data(content.size());
    std::memcpy(data.data(), content.data(), content.size());

    auto wh = cache->write_sync(key, data.size());
    REQUIRE(wh.has_value());
    wh->write_sync(std::span<const std::byte>(data));
    wh->close_sync();
  }

  constexpr int num_threads = 8;
  constexpr int reads_per_thread = 100;

  std::atomic<int> successful_reads{0};
  std::atomic<int> failed_reads{0};

  auto reader = [&](int thread_id) {
    std::mt19937 rng(thread_id);
    std::uniform_int_distribution<int> dist(0, num_entries - 1);

    for (int i = 0; i < reads_per_thread; ++i) {
      int idx = dist(rng);
      CacheKey key("read-test-key-" + std::to_string(idx));

      auto rh = cache->read_sync(key);
      if (rh.has_value()) {
        auto content = rh->content();
        if (!content.empty()) {
          ++successful_reads;
        } else {
          ++failed_reads;
        }
      } else {
        ++failed_reads;
      }
    }
  };

  std::vector<std::thread> threads;
  threads.reserve(num_threads);
  for (int t = 0; t < num_threads; ++t) {
    threads.emplace_back(reader, t);
  }

  for (auto &t : threads) {
    t.join();
  }

  REQUIRE(successful_reads == num_threads * reads_per_thread);
  REQUIRE(failed_reads == 0);

  cache->stop();
}

TEST_CASE("Mixed concurrent reads and writes", "[concurrent][integration]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheConfig config;
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(50 * 1024 * 1024);

  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  for (int i = 0; i < 50; ++i) {
    CacheKey key("mixed-key-" + std::to_string(i));
    std::string content = "Initial content " + std::to_string(i);

    std::vector<std::byte> data(content.size());
    std::memcpy(data.data(), content.data(), content.size());

    auto wh = cache->write_sync(key, data.size());
    if (wh.has_value()) {
      wh->write_sync(std::span<const std::byte>(data));
      wh->close_sync();
    }
  }

  constexpr int num_readers = 4;
  constexpr int num_writers = 2;

  std::atomic<int> read_hits{0};
  std::atomic<int> read_misses{0};
  std::atomic<int> writes{0};
  std::atomic<bool> stop_flag{false};

  auto reader = [&](int thread_id) {
    std::mt19937 rng(thread_id);
    std::uniform_int_distribution<int> dist(0, 99);

    while (!stop_flag.load()) {
      int idx = dist(rng);
      CacheKey key("mixed-key-" + std::to_string(idx));

      auto rh = cache->read_sync(key);
      if (rh.has_value()) {
        ++read_hits;
      } else {
        ++read_misses;
      }
    }
  };

  auto writer = [&](int thread_id) {
    int counter = 0;
    while (!stop_flag.load()) {
      CacheKey key("mixed-key-" +
                   std::to_string(50 + thread_id * 100 + (counter % 50)));
      std::string content = "New content " + std::to_string(counter);

      std::vector<std::byte> data(content.size());
      std::memcpy(data.data(), content.data(), content.size());

      auto wh = cache->write_sync(key, data.size());
      if (wh.has_value()) {
        wh->write_sync(std::span<const std::byte>(data));
        wh->close_sync();
        ++writes;
      }
      ++counter;

      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
  };

  std::vector<std::thread> threads;
  threads.reserve(num_readers);
  for (int t = 0; t < num_readers; ++t) {
    threads.emplace_back(reader, t);
  }
  for (int t = 0; t < num_writers; ++t) {
    threads.emplace_back(writer, t);
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  stop_flag.store(true);

  for (auto &t : threads) {
    t.join();
  }

  INFO("Read hits: " << read_hits << ", Read misses: " << read_misses
                     << ", Writes: " << writes);
  REQUIRE(read_hits > 0);
  REQUIRE(writes > 0);

  cache->stop();
}

TEST_CASE("Concurrent exists checks", "[concurrent][integration]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheConfig config;
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(20 * 1024 * 1024);

  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  for (int i = 0; i < 100; ++i) {
    if (i % 2 == 0) {
      CacheKey key("exists-key-" + std::to_string(i));
      std::string content = "content";
      std::vector<std::byte> data(content.size());
      std::memcpy(data.data(), content.data(), content.size());

      auto wh = cache->write_sync(key, data.size());
      if (wh.has_value()) {
        wh->write_sync(std::span<const std::byte>(data));
        wh->close_sync();
      }
    }
  }

  constexpr int num_threads = 8;
  std::atomic<int> correct_exists{0};
  std::atomic<int> correct_not_exists{0};
  std::atomic<int> errors{0};

  auto checker = [&](int thread_id) {
    for (int i = thread_id; i < 100; i += num_threads) {
      CacheKey key("exists-key-" + std::to_string(i));
      auto result = cache->exists_sync(key);

      if (!result.has_value()) {
        ++errors;
        continue;
      }

      bool expected = (i % 2 == 0);
      if (*result == expected) {
        if (expected) {
          ++correct_exists;
        } else {
          ++correct_not_exists;
        }
      } else {
        ++errors;
      }
    }
  };

  std::vector<std::thread> threads;
  threads.reserve(num_threads);
  for (int t = 0; t < num_threads; ++t) {
    threads.emplace_back(checker, t);
  }

  for (auto &t : threads) {
    t.join();
  }

  REQUIRE(errors == 0);
  REQUIRE(correct_exists == 50);
  REQUIRE(correct_not_exists == 50);

  cache->stop();
}

TEST_CASE("Concurrent remove operations", "[concurrent][integration]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheConfig config;
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(30 * 1024 * 1024);

  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  for (int i = 0; i < 100; ++i) {
    CacheKey key("remove-key-" + std::to_string(i));
    std::string content = "content to remove";
    std::vector<std::byte> data(content.size());
    std::memcpy(data.data(), content.data(), content.size());

    auto wh = cache->write_sync(key, data.size());
    if (wh.has_value()) {
      wh->write_sync(std::span<const std::byte>(data));
      wh->close_sync();
    }
  }

  constexpr int num_threads = 4;
  std::atomic<int> successful_removes{0};
  std::atomic<int> already_removed{0};

  auto remover = [&](int thread_id) {
    for (int i = thread_id; i < 100; i += num_threads) {
      CacheKey key("remove-key-" + std::to_string(i));
      auto result = cache->remove_sync(key);

      if (result.has_value()) {
        ++successful_removes;
      } else if (result.error() == CacheError::NotFound) {
        ++already_removed;
      }
    }
  };

  std::vector<std::thread> threads;
  threads.reserve(num_threads);
  for (int t = 0; t < num_threads; ++t) {
    threads.emplace_back(remover, t);
  }

  for (auto &t : threads) {
    t.join();
  }

  REQUIRE(successful_removes + already_removed == 100);

  for (int i = 0; i < 100; ++i) {
    CacheKey key("remove-key-" + std::to_string(i));
    auto exists = cache->exists_sync(key);
    REQUIRE(exists.has_value());
    REQUIRE(*exists == false);
  }

  cache->stop();
}

TEST_CASE("Stress test with many small entries", "[stress][integration]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheConfig config;
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(100 * 1024 * 1024);

  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  constexpr int num_entries = 1000;
  int successful = 0;

  for (int i = 0; i < num_entries; ++i) {
    CacheKey key("stress-small-" + std::to_string(i));
    std::string content = "Small content " + std::to_string(i);

    std::vector<std::byte> data(content.size());
    std::memcpy(data.data(), content.data(), content.size());

    auto wh = cache->write_sync(key, data.size());
    if (wh.has_value()) {
      wh->write_sync(std::span<const std::byte>(data));
      if (wh->close_sync().has_value()) {
        ++successful;
      }
    }
  }

  INFO("Successfully wrote " << successful << " of " << num_entries
                             << " entries");
  REQUIRE(successful > num_entries / 2);

  int read_success = 0;
  for (int i = 0; i < num_entries; ++i) {
    CacheKey key("stress-small-" + std::to_string(i));
    auto rh = cache->read_sync(key);
    if (rh.has_value()) {
      ++read_success;
    }
  }

  INFO("Successfully read " << read_success << " entries");

  cache->stop();
}

TEST_CASE("Stress test with large entries", "[stress][integration]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheConfig config;
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(200 * 1024 * 1024);

  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  constexpr int num_entries = 50;
  constexpr size_t entry_size = static_cast<const size_t>(1024 * 1024);

  std::vector<std::byte> large_content(entry_size);
  std::mt19937 rng(42);
  for (auto &b : large_content) {
    b = static_cast<std::byte>(rng() & 0xFF);
  }

  int successful = 0;
  for (int i = 0; i < num_entries; ++i) {
    CacheKey key("stress-large-" + std::to_string(i));

    auto wh = cache->write_sync(key, large_content.size());
    if (wh.has_value()) {
      wh->write_sync(std::span<const std::byte>(large_content));
      if (wh->close_sync().has_value()) {
        ++successful;
      }
    }
  }

  INFO("Successfully wrote " << successful << " large entries");
  REQUIRE(successful > 0);

  auto stats = cache->stats();
  INFO("Cache stats - entries: " << stats.current_entries
                                 << ", bytes: " << stats.current_bytes);

  cache->stop();
}

// =============================================================================
// Phase 4C: Concurrency Edge Cases
// =============================================================================

TEST_CASE("Concurrent read during write", "[concurrent][security]") {
  // One thread writes entries continuously while another thread reads.
  // Verify no crashes and reads either succeed with valid data or return
  // NotFound.

  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheConfig config;
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(50 * 1024 * 1024);

  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  std::atomic<bool> stop_flag{false};
  std::atomic<int> writes_completed{0};
  std::atomic<int> reads_completed{0};
  std::atomic<int> reads_found{0};
  std::atomic<int> reads_not_found{0};

  // Writer thread: continuously writes entries with known content
  auto writer = [&]() {
    for (int i = 0; i < 200 && !stop_flag.load(); ++i) {
      CacheKey key("rw-concurrent-" + std::to_string(i % 50));
      std::string content_str = "Content-" + std::to_string(i);
      std::vector<std::byte> content(content_str.size());
      std::memcpy(content.data(), content_str.data(), content_str.size());

      auto wh = cache->write_sync(key, content.size());
      if (wh.has_value()) {
        wh->write_sync(std::span<const std::byte>(content));
        wh->close_sync();
        ++writes_completed;
      }
    }
  };

  // Reader thread: continuously reads entries
  auto reader = [&]() {
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> dist(0, 49);

    while (!stop_flag.load()) {
      int idx = dist(rng);
      CacheKey key("rw-concurrent-" + std::to_string(idx));

      auto rh = cache->read_sync(key);
      ++reads_completed;

      if (rh.has_value()) {
        ++reads_found;
        // Verify the content is not empty when found
        auto content = rh->content();
        // Content should be non-empty if found
        // (it's possible to get an empty content for zero-length writes,
        // but our writes always have content)
        REQUIRE(!content.empty());
      } else {
        ++reads_not_found;
        // Should only get NotFound, not crashes or other errors
        REQUIRE(rh.error() == CacheError::NotFound);
      }
    }
  };

  // Start reader first, then writer
  std::thread reader_thread(reader);
  std::thread writer_thread(writer);

  // Wait for writer to finish
  writer_thread.join();

  // Let reader run a bit more after writes are done
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  stop_flag.store(true);
  reader_thread.join();

  INFO("Writes: " << writes_completed << ", Reads: " << reads_completed
                  << ", Found: " << reads_found
                  << ", NotFound: " << reads_not_found);

  REQUIRE(writes_completed > 0);
  REQUIRE(reads_completed > 0);

  cache->stop();
}

TEST_CASE("Write position wraparound preserves data", "[edge][eviction]") {
  // Create a cache and write enough data to exceed the stripe's data area,
  // forcing the write position to wrap around. Verify the last written entry
  // is still readable.
  //
  // Since kMinStripeSize is 128MB, the volume must be at least that large.
  // We write large entries (64KB each) to fill the data area faster.
  // The data area is roughly 129MB - header - directory overhead (~128MB).
  // Writing ~3000 entries of 64KB each = ~192MB should force wraparound.
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheConfig config;
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(129 * 1024 * 1024);
  vol_config.sync_on_write = false;  // faster for test

  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  constexpr int num_entries = 3000;
  constexpr int entry_size = 64 * 1024;  // 64KB per entry

  std::vector<std::byte> content(entry_size, std::byte{0x42});

  int successful_writes = 0;
  CacheKey last_key("");  // track the last successfully written key

  for (int i = 0; i < num_entries; ++i) {
    CacheKey key("wrap-test-" + std::to_string(i));
    auto wh = cache->write_sync(key, content.size());
    if (wh.has_value()) {
      wh->write_sync(std::span<const std::byte>(content));
      if (wh->close_sync().has_value()) {
        ++successful_writes;
        last_key = key;
      }
    }
  }

  INFO("Successfully wrote " << successful_writes << " of " << num_entries
                             << " entries");
  REQUIRE(successful_writes > 0);

  // The last written entry should be readable (it was just written)
  if (!last_key.is_zero()) {
    auto rh = cache->read_sync(last_key);
    REQUIRE(rh.has_value());
    REQUIRE(rh->content().size() == static_cast<size_t>(entry_size));
  }

  // Verify evictions occurred (wraparound happened)
  auto stats = cache->stats();
  INFO("Evictions: " << stats.evictions);
  // With 3000 writes of 64KB each (~192MB) into a ~128MB data area, evictions
  // must happen
  REQUIRE(stats.evictions > 0);

  cache->stop();
}
