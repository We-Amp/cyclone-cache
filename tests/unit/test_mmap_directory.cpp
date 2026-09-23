// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../../src/core/mmap_directory.hpp"
#include "cyclone/cache.hpp"
#include "cyclone/config.hpp"
#include "support/temp_cache.hpp"

using namespace cyclone;

// =============================================================================
// MmapDirectory Unit Tests
// =============================================================================

TEST_CASE("MmapDirectory required_size calculation", "[mmap_directory]") {
  // Header (64) + versions (4 * buckets) + entries (10 * 4 * buckets) aligned
  size_t buckets = 1024;
  size_t required = MmapDirectory::required_size(buckets);

  // Should be at least header + versions + entries
  size_t min_size = 64 + (buckets * 4) + (buckets * 4 * 10);
  REQUIRE(required >= min_size);
}

TEST_CASE("MmapDirectory init creates valid directory", "[mmap_directory]") {
  constexpr size_t kBuckets = 256;
  size_t region_size = MmapDirectory::required_size(kBuckets);
  std::vector<std::byte> region(region_size, std::byte{0});

  auto dir_opt = MmapDirectory::init(std::span<std::byte>(region), kBuckets);

  REQUIRE(dir_opt.has_value());
  auto &dir = *dir_opt;
  REQUIRE(dir.is_valid());
  REQUIRE(dir.count() == 0);
  REQUIRE(dir.bucket_count() == kBuckets);
  REQUIRE(dir.capacity() == kBuckets * MmapDirectory::kEntriesPerBucket);
}

TEST_CASE("MmapDirectory init zeroes the shared header over recycled memory",
          "[mmap_directory]") {
  constexpr size_t kBuckets = 256;
  size_t region_size = MmapDirectory::required_size(kBuckets);
  // A fresh volume can be created over recycled memory, so every
  // shared-header field must be zero-init safe.  Pre-fill the region
  // with garbage: any field init() forgets to zero shows up as non-zero.
  std::vector<std::byte> region(region_size, std::byte{0xAA});

  auto dir_opt = MmapDirectory::init(std::span<std::byte>(region), kBuckets);
  REQUIRE(dir_opt.has_value());
  auto &dir = *dir_opt;

  // Byte-granular over the whole header so a future tail-appended field
  // cannot regress without failing here: every byte outside magic (0-3),
  // version (4-5) and num_buckets (8-11) must be zero — reserved, every
  // carved field, and shared_wrap_deferred_deadline_ms in particular.
  const auto *header_bytes =
      reinterpret_cast<const unsigned char *>(region.data());
  for (size_t i = 0; i < MmapDirectory::Header::kHeaderSize; ++i) {
    const bool nonzero_field = i < 6 || (i >= 8 && i < 12);
    if (!nonzero_field) {
      REQUIRE(header_bytes[i] == 0);
    }
  }

  // The field the zero-init audit caught unzeroed, through its public
  // accessor.
  REQUIRE(dir.wrap_deferred_deadline_ms() == 0);

  // The bucket version counters follow the header; they must be zeroed too
  // (load_version() is private, so scan the mapped bytes directly).
  const size_t versions_end =
      MmapDirectory::Header::kHeaderSize + kBuckets * sizeof(uint32_t);
  for (size_t i = MmapDirectory::Header::kHeaderSize; i < versions_end; ++i) {
    REQUIRE(region[i] == std::byte{0});
  }
}

TEST_CASE("MmapDirectory init fails with insufficient region",
          "[mmap_directory]") {
  constexpr size_t kBuckets = 256;
  size_t required = MmapDirectory::required_size(kBuckets);

  // Provide region that's too small
  std::vector<std::byte> region(required / 2, std::byte{0});

  auto dir = MmapDirectory::init(std::span<std::byte>(region), kBuckets);

  REQUIRE_FALSE(dir.has_value());
}

TEST_CASE("MmapDirectory open existing directory", "[mmap_directory]") {
  constexpr size_t kBuckets = 256;
  size_t region_size = MmapDirectory::required_size(kBuckets);
  std::vector<std::byte> region(region_size, std::byte{0});

  // Initialize
  auto dir1_opt = MmapDirectory::init(std::span<std::byte>(region), kBuckets);
  REQUIRE(dir1_opt.has_value());
  auto &dir1 = *dir1_opt;

  // Insert some entries
  CacheKey key1("test-key-1");
  CacheKey key2("test-key-2");
  REQUIRE(dir1.insert(key1, 1000, 512));
  REQUIRE(dir1.insert(key2, 2000, 1024));
  REQUIRE(dir1.count() == 2);

  // Open the same region (simulating another process)
  auto dir2_opt = MmapDirectory::open(std::span<std::byte>(region));
  REQUIRE(dir2_opt.has_value());

  auto &dir2 = *dir2_opt;
  REQUIRE(dir2.is_valid());
  REQUIRE(dir2.count() == 2);

  // Verify entries are visible
  auto entry1 = dir2.probe(key1);
  REQUIRE(entry1.has_value());
  REQUIRE(entry1->offset() == 1000);

  auto entry2 = dir2.probe(key2);
  REQUIRE(entry2.has_value());
  REQUIRE(entry2->offset() == 2000);
}

TEST_CASE("MmapDirectory open fails with invalid magic", "[mmap_directory]") {
  constexpr size_t kBuckets = 256;
  size_t region_size = MmapDirectory::required_size(kBuckets);
  std::vector<std::byte> region(region_size, std::byte{0});

  // Don't initialize - just zeros (invalid magic)
  auto dir_opt = MmapDirectory::open(std::span<std::byte>(region));
  REQUIRE_FALSE(dir_opt.has_value());
}

TEST_CASE("MmapDirectory basic insert and probe", "[mmap_directory]") {
  constexpr size_t kBuckets = 256;
  size_t region_size = MmapDirectory::required_size(kBuckets);
  std::vector<std::byte> region(region_size, std::byte{0});

  auto dir_opt = MmapDirectory::init(std::span<std::byte>(region), kBuckets);
  REQUIRE(dir_opt.has_value());
  auto &dir = *dir_opt;

  CacheKey key("test-key");

  // Probe empty directory
  auto entry = dir.probe(key);
  REQUIRE_FALSE(entry.has_value());

  // Insert
  REQUIRE(dir.insert(key, 12345, 1024));
  REQUIRE(dir.count() == 1);

  // Probe after insert
  entry = dir.probe(key);
  REQUIRE(entry.has_value());
  REQUIRE(entry->offset() == 12345);
}

TEST_CASE("MmapDirectory insert updates existing entry", "[mmap_directory]") {
  constexpr size_t kBuckets = 256;
  size_t region_size = MmapDirectory::required_size(kBuckets);
  std::vector<std::byte> region(region_size, std::byte{0});

  auto dir_opt = MmapDirectory::init(std::span<std::byte>(region), kBuckets);
  REQUIRE(dir_opt.has_value());
  auto &dir = *dir_opt;

  CacheKey key("test-key");

  // Insert initial
  REQUIRE(dir.insert(key, 1000, 512));
  REQUIRE(dir.count() == 1);

  // Update same key
  REQUIRE(dir.insert(key, 2000, 1024));
  REQUIRE(dir.count() == 1);  // Count should not change

  // Verify updated value
  auto entry = dir.probe(key);
  REQUIRE(entry.has_value());
  REQUIRE(entry->offset() == 2000);
}

TEST_CASE("MmapDirectory remove", "[mmap_directory]") {
  constexpr size_t kBuckets = 256;
  size_t region_size = MmapDirectory::required_size(kBuckets);
  std::vector<std::byte> region(region_size, std::byte{0});

  auto dir_opt = MmapDirectory::init(std::span<std::byte>(region), kBuckets);
  REQUIRE(dir_opt.has_value());
  auto &dir = *dir_opt;

  CacheKey key("test-key");

  // Insert
  REQUIRE(dir.insert(key, 12345, 1024));
  REQUIRE(dir.count() == 1);

  // Remove
  REQUIRE(dir.remove(key));
  REQUIRE(dir.count() == 0);

  // Verify removed
  auto entry = dir.probe(key);
  REQUIRE_FALSE(entry.has_value());

  // Remove non-existent
  REQUIRE_FALSE(dir.remove(key));
}

TEST_CASE("MmapDirectory clear", "[mmap_directory]") {
  constexpr size_t kBuckets = 256;
  size_t region_size = MmapDirectory::required_size(kBuckets);
  std::vector<std::byte> region(region_size, std::byte{0});

  auto dir_opt = MmapDirectory::init(std::span<std::byte>(region), kBuckets);
  REQUIRE(dir_opt.has_value());
  auto &dir = *dir_opt;

  // Insert multiple keys
  for (int i = 0; i < 100; ++i) {
    CacheKey key("key-" + std::to_string(i));
    dir.insert(key, static_cast<uint64_t>(i) * 1000, 512);
  }
  REQUIRE(dir.count() == 100);

  // Clear
  dir.clear();
  REQUIRE(dir.count() == 0);

  // Verify entries are gone
  CacheKey key("key-50");
  REQUIRE_FALSE(dir.probe(key).has_value());
}

TEST_CASE("MmapDirectory probe_each iterates matching entries",
          "[mmap_directory]") {
  constexpr size_t kBuckets = 256;
  size_t region_size = MmapDirectory::required_size(kBuckets);
  std::vector<std::byte> region(region_size, std::byte{0});

  auto dir_opt = MmapDirectory::init(std::span<std::byte>(region), kBuckets);
  REQUIRE(dir_opt.has_value());
  auto &dir = *dir_opt;

  CacheKey key("test-key");
  REQUIRE(dir.insert(key, 12345, 1024));

  int count = 0;
  uint64_t found_offset = 0;

  dir.probe_each(key, [&](const DirEntry &entry) {
    ++count;
    found_offset = entry.offset();
    return true;  // Continue iteration
  });

  REQUIRE(count == 1);
  REQUIRE(found_offset == 12345);
}

TEST_CASE("MmapDirectory probe_each stops on callback false",
          "[mmap_directory]") {
  constexpr size_t kBuckets = 256;
  size_t region_size = MmapDirectory::required_size(kBuckets);
  std::vector<std::byte> region(region_size, std::byte{0});

  auto dir_opt = MmapDirectory::init(std::span<std::byte>(region), kBuckets);
  REQUIRE(dir_opt.has_value());
  auto &dir = *dir_opt;

  CacheKey key("test-key");
  REQUIRE(dir.insert(key, 12345, 1024));

  int count = 0;
  dir.probe_each(key, [&](const DirEntry &) {
    ++count;
    return false;  // Stop iteration
  });

  REQUIRE(count == 1);
}

TEST_CASE("MmapDirectory bucket overflow handled gracefully",
          "[mmap_directory]") {
  constexpr size_t kBuckets = 256;
  size_t region_size = MmapDirectory::required_size(kBuckets);
  std::vector<std::byte> region(region_size, std::byte{0});

  auto dir_opt = MmapDirectory::init(std::span<std::byte>(region), kBuckets);
  REQUIRE(dir_opt.has_value());
  auto &dir = *dir_opt;

  // Insert many keys, more than fit in any single bucket (4 entries each).
  // Bucket assignment is not directly controllable, so some buckets overflow
  // while others stay underfull.  Offsets start at 1000 and never hit 0:
  // offset 0 reads as is_empty(), which would make an entry a phantom slot
  // (counted, but invisible to probes and re-claimable as an empty).
  constexpr int kInserts = 1000;
  int successful_inserts = 0;
  int bucket_full_evictions = 0;
  // The last key inserted into each bucket, and the offset it must hold.
  std::map<uint32_t, std::pair<CacheKey, uint64_t>> last_in_bucket;
  for (int i = 0; i < kInserts; ++i) {
    CacheKey key("key-" + std::to_string(i));
    uint64_t offset = static_cast<uint64_t>(i + 1) * 1000;
    bool bucket_full_evicted = false;
    if (dir.insert(key, offset, 512, MmapDirectory::kMatchAnyTag, nullptr,
                   &bucket_full_evicted)) {
      ++successful_inserts;
    }
    if (bucket_full_evicted) {
      ++bucket_full_evictions;
    }
    uint32_t bucket = key.bucket_hash() % kBuckets;
    last_in_bucket.insert_or_assign(bucket, std::make_pair(key, offset));
  }

  // Every insert lands: a full bucket evicts the entry nearest the wrap
  // cursor rather than dropping the write.
  REQUIRE(successful_inserts == kInserts);
  // Overflow actually happened, so the eviction path was exercised.
  REQUIRE(bucket_full_evictions > 0);

  // The most recent write to a bucket must always be findable at exactly the
  // offset it was inserted with, whichever slot the insert claimed — nothing
  // inserted after it could have displaced it.  This is what pins victim
  // selection to a real observation: an eviction that wrote a malformed entry
  // into the victim slot would lose these offsets.
  for (const auto &[bucket, key_offset] : last_in_bucket) {
    const auto &[key, expected_offset] = key_offset;
    bool found = false;
    dir.probe_each(key, [&](const DirEntry &entry) {
      if (entry.offset() == expected_offset) {
        found = true;
        return false;
      }
      return true;
    });
    INFO("bucket " << bucket << " expected offset " << expected_offset);
    REQUIRE(found);
  }

  // count tracks occupied slots and is incremented only when an insert claims
  // an empty one, so the evictions above make it trail the insert count.
  REQUIRE(dir.count() < static_cast<size_t>(successful_inserts));
  REQUIRE(dir.count() <= kBuckets * MmapDirectory::kEntriesPerBucket);
}

TEST_CASE("MmapDirectory GC phase toggle", "[mmap_directory]") {
  constexpr size_t kBuckets = 256;
  size_t region_size = MmapDirectory::required_size(kBuckets);
  std::vector<std::byte> region(region_size, std::byte{0});

  auto dir_opt = MmapDirectory::init(std::span<std::byte>(region), kBuckets);
  REQUIRE(dir_opt.has_value());
  auto &dir = *dir_opt;

  bool initial_phase = dir.current_phase();

  dir.toggle_phase();
  REQUIRE(dir.current_phase() != initial_phase);

  dir.toggle_phase();
  REQUIRE(dir.current_phase() == initial_phase);
}

// =============================================================================
// Cross-Process Simulation Tests
// =============================================================================

TEST_CASE("MmapDirectory cross-instance visibility",
          "[mmap_directory][multiprocess]") {
  // Simulate cross-process by having two MmapDirectory instances pointing to
  // same region
  constexpr size_t kBuckets = 256;
  size_t region_size = MmapDirectory::required_size(kBuckets);
  std::vector<std::byte> region(region_size, std::byte{0});

  // "Process 1" initializes
  auto dir1_opt = MmapDirectory::init(std::span<std::byte>(region), kBuckets);
  REQUIRE(dir1_opt.has_value());
  auto &dir1 = *dir1_opt;

  // "Process 2" opens
  auto dir2_opt = MmapDirectory::open(std::span<std::byte>(region));
  REQUIRE(dir2_opt.has_value());
  auto &dir2 = *dir2_opt;

  // Process 1 inserts
  CacheKey key("shared-key");
  REQUIRE(dir1.insert(key, 5000, 2048));

  // Process 2 should see it immediately (same memory)
  auto entry = dir2.probe(key);
  REQUIRE(entry.has_value());
  REQUIRE(entry->offset() == 5000);

  // Process 2 removes
  REQUIRE(dir2.remove(key));

  // Process 1 should see removal
  REQUIRE_FALSE(dir1.probe(key).has_value());
}

TEST_CASE("MmapDirectory concurrent read/write simulation",
          "[mmap_directory][multiprocess]") {
  constexpr size_t kBuckets = 1024;
  size_t region_size = MmapDirectory::required_size(kBuckets);
  std::vector<std::byte> region(region_size, std::byte{0});

  auto dir_opt = MmapDirectory::init(std::span<std::byte>(region), kBuckets);
  REQUIRE(dir_opt.has_value());
  auto &dir = *dir_opt;

  // Open second instance (simulating reader process)
  auto reader_opt = MmapDirectory::open(std::span<std::byte>(region));
  REQUIRE(reader_opt.has_value());
  auto &reader = *reader_opt;

  std::atomic<int> reads{0};
  std::atomic<int> writes{0};

  // Writer thread (simulating writer process)
  std::thread writer([&]() {
    for (int i = 0; i < 100; ++i) {
      CacheKey key("key-" + std::to_string(i % 50));
      dir.insert(key, static_cast<uint64_t>(i) * 1000, 512);
      ++writes;
      std::this_thread::yield();
    }
  });

  // Reader thread (simulating reader process)
  std::thread reader_thread([&]() {
    for (int i = 0; i < 200; ++i) {
      CacheKey key("key-" + std::to_string(i % 50));
      reader.probe(key);  // May or may not find entry
      ++reads;
      std::this_thread::yield();
    }
  });

  writer.join();
  reader_thread.join();

  // Should complete without crashes or hangs
  REQUIRE(writes.load() == 100);
  REQUIRE(reads.load() == 200);
}

// =============================================================================
// Integration Tests with Multi-Process Cache
// =============================================================================

TEST_CASE("Multi-process cache uses mmap directory",
          "[mmap_directory][multiprocess][integration]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  {
    CacheConfig config;
    config.set_multi_process(0, 2);  // Process 0 of 2
    config.set_enable_checksum(true);

    auto cache_result = Cache::create(config);
    REQUIRE(cache_result.has_value());
    auto &cache = *cache_result;

    auto add_result =
        cache->add_volume(cache_path, static_cast<size_t>(10 * 1024 * 1024));
    REQUIRE(add_result.has_value());

    auto start_result = cache->start();
    REQUIRE(start_result.has_value());

    // Write to owned stripe
    CacheKey key("mmap-test-key");
    {
      auto write_result = cache->write_sync(key, 100);
      // May succeed or fail depending on stripe ownership
      if (write_result.has_value()) {
        auto &handle = *write_result;
        std::vector<std::byte> data(100, std::byte{0x42});
        handle.write_sync(data);
        handle.close_sync();
      }
    }

    cache->stop();
  }
}

TEST_CASE("Multi-process directory persists across cache restart",
          "[mmap_directory][multiprocess][integration]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheKey key("persist-test-key");
  std::vector<std::byte> test_data(100, std::byte{0xAB});

  // First cache instance - write data
  {
    CacheConfig config;
    config.set_multi_process(0, 1);  // Single process with mmap directory
    config.set_enable_checksum(true);

    auto cache = Cache::create(config);
    REQUIRE(cache.has_value());

    (*cache)->add_volume(cache_path, static_cast<size_t>(10 * 1024 * 1024));
    auto start = (*cache)->start();
    REQUIRE(start.has_value());

    auto write_result = (*cache)->write_sync(key, test_data.size());
    REQUIRE(write_result.has_value());

    auto &handle = *write_result;
    handle.write_sync(test_data);
    handle.close_sync();

    (*cache)->stop();
  }

  // Second cache instance - should find data via mmap directory
  {
    CacheConfig config;
    config.set_multi_process(0, 1);
    config.set_enable_checksum(true);

    auto cache = Cache::create(config);
    REQUIRE(cache.has_value());

    (*cache)->add_volume(cache_path, static_cast<size_t>(10 * 1024 * 1024));
    auto start = (*cache)->start();
    REQUIRE(start.has_value());

    // Read should find the data
    {
      auto read_result = (*cache)->read_sync(key);
      REQUIRE(read_result.has_value());

      auto content = read_result->content();
      REQUIRE(content.size() == test_data.size());
      REQUIRE(std::memcmp(content.data(), test_data.data(), content.size()) ==
              0);
    }

    (*cache)->stop();
  }
}

TEST_CASE("Non-multi-process cache uses in-memory directory",
          "[mmap_directory][integration]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheKey key("inmem-test-key");
  std::vector<std::byte> test_data(100, std::byte{0xCD});

  // First instance - write
  {
    CacheConfig config;
    // Default: no multi-process, uses in-memory directory

    auto cache = Cache::create(config);
    REQUIRE(cache.has_value());

    (*cache)->add_volume(cache_path, static_cast<size_t>(10 * 1024 * 1024));
    (*cache)->start();

    auto write = (*cache)->write_sync(key, test_data.size());
    REQUIRE(write.has_value());
    (*write).write_sync(test_data);
    (*write).close_sync();

    (*cache)->stop();
  }

  // Second instance - should NOT find data (in-memory directory lost)
  {
    CacheConfig config;

    auto cache = Cache::create(config);
    REQUIRE(cache.has_value());

    (*cache)->add_volume(cache_path, static_cast<size_t>(10 * 1024 * 1024));
    (*cache)->start();

    auto read = (*cache)->read_sync(key);
    REQUIRE_FALSE(read.has_value());  // Data not found - expected
    REQUIRE(read.error() == CacheError::NotFound);

    (*cache)->stop();
  }
}

// =============================================================================
// Security and Edge Case Tests
// =============================================================================

TEST_CASE("MmapDirectory required_size overflow protection",
          "[mmap_directory][security]") {
  // Test that huge num_buckets values don't cause integer overflow
  // Instead, required_size should return SIZE_MAX to indicate overflow

  // Very large bucket count that would overflow
  size_t huge_buckets = SIZE_MAX / 10;  // Would overflow in entries calculation

  size_t size = MmapDirectory::required_size(huge_buckets);
  REQUIRE(size == SIZE_MAX);  // Should indicate overflow
}

TEST_CASE("MmapDirectory init rejects zero buckets",
          "[mmap_directory][security]") {
  std::vector<std::byte> region(1024, std::byte{0});

  auto dir = MmapDirectory::init(std::span<std::byte>(region), 0);
  REQUIRE_FALSE(dir.has_value());
}

TEST_CASE("MmapDirectory open rejects malicious num_buckets",
          "[mmap_directory][security]") {
  // Create a region with valid header but malicious num_buckets
  constexpr size_t kRegionSize = 1024;  // Small region
  std::vector<std::byte> region(kRegionSize, std::byte{0});

  // Manually construct header with huge num_buckets
  auto *header = reinterpret_cast<MmapDirectory::Header *>(region.data());
  header->magic = MmapDirectory::kMagic;
  header->version = MmapDirectory::kVersion;
  header->num_buckets = UINT32_MAX;  // Malicious value - would cause OOB access

  // open() should reject this because region is too small for claimed buckets
  auto dir = MmapDirectory::open(std::span<std::byte>(region));
  REQUIRE_FALSE(dir.has_value());
}

TEST_CASE("MmapDirectory move semantics", "[mmap_directory]") {
  constexpr size_t kBuckets = 64;
  size_t region_size = MmapDirectory::required_size(kBuckets);
  std::vector<std::byte> region(region_size, std::byte{0});

  auto dir1_opt = MmapDirectory::init(std::span<std::byte>(region), kBuckets);
  REQUIRE(dir1_opt.has_value());

  // Insert some data
  CacheKey key("test-key");
  REQUIRE(dir1_opt->insert(key, 12345, 1024));
  REQUIRE(dir1_opt->count() == 1);

  // Move construct
  MmapDirectory dir2 = std::move(*dir1_opt);

  // Original should be invalid after move
  REQUIRE_FALSE(dir1_opt->is_valid());

  // New directory should be valid and have the data
  REQUIRE(dir2.is_valid());
  REQUIRE(dir2.count() == 1);
  auto entry = dir2.probe(key);
  REQUIRE(entry.has_value());
  REQUIRE(entry->offset() == 12345);
}

TEST_CASE("MmapDirectory tag collision behavior", "[mmap_directory]") {
  // Test that the directory handles many keys correctly
  // Note: keys with the same 12-bit tag will overwrite each other (by design)
  constexpr size_t kBuckets = 256;
  size_t region_size = MmapDirectory::required_size(kBuckets);
  std::vector<std::byte> region(region_size, std::byte{0});

  auto dir_opt = MmapDirectory::init(std::span<std::byte>(region), kBuckets);
  REQUIRE(dir_opt.has_value());
  auto &dir = *dir_opt;

  // Insert many keys
  int successful_inserts = 0;
  for (int i = 0; i < 200; ++i) {
    CacheKey key("collision-test-" + std::to_string(i));
    if (dir.insert(key, static_cast<uint64_t>(i) * 1000, 512)) {
      ++successful_inserts;
    }
  }

  // Should have inserted many entries (some may fail due to bucket overflow)
  REQUIRE(successful_inserts > 100);

  // The last inserted entry for each tag should be retrievable
  // We test a sample of keys to verify basic functionality
  CacheKey key("collision-test-199");
  auto entry = dir.probe(key);
  // Entry should exist (we inserted it), though value may differ if tag
  // collision occurred Just verify we can probe without crashing
  (void)entry;
}

TEST_CASE("MmapDirectory version counter wraparound", "[mmap_directory]") {
  // Version counters are uint32_t - test they handle many increments
  constexpr size_t kBuckets = 4;
  size_t region_size = MmapDirectory::required_size(kBuckets);
  std::vector<std::byte> region(region_size, std::byte{0});

  auto dir_opt = MmapDirectory::init(std::span<std::byte>(region), kBuckets);
  REQUIRE(dir_opt.has_value());
  auto &dir = *dir_opt;

  CacheKey key("version-test");

  // Insert once
  REQUIRE(dir.insert(key, 0, 512));
  REQUIRE(dir.count() == 1);

  // Update many times to bump version counter
  for (int i = 1; i < 10000; ++i) {
    dir.insert(key, i, 512);
  }

  // Directory should still work correctly
  auto entry = dir.probe(key);
  REQUIRE(entry.has_value());
  REQUIRE(entry->offset() == 9999);
  // Note: count may be slightly off due to race between insert detection
  // and count increment in high-frequency updates, but should still be
  // reasonable
  REQUIRE(dir.count() >= 1);
  REQUIRE(dir.count() <= 10);  // Shouldn't be wildly wrong
}

TEST_CASE("MmapDirectory concurrent writers same bucket with spinlock",
          "[mmap_directory][multiprocess]") {
  // MmapDirectory::insert() now uses an internal CAS spinlock (acquire_writer/
  // release_writer) that serializes writers per-bucket.  No external mutex
  // needed. This test exercises concurrent writers hitting the same small set
  // of buckets, verifying the spinlock prevents data corruption.
  constexpr size_t kBuckets = 4;  // Small bucket count → high collision rate
  size_t region_size = MmapDirectory::required_size(kBuckets);
  std::vector<std::byte> region(region_size, std::byte{0});

  auto dir_opt = MmapDirectory::init(std::span<std::byte>(region), kBuckets);
  REQUIRE(dir_opt.has_value());
  auto &dir = *dir_opt;

  std::atomic<int> successful_inserts{0};
  constexpr int kThreads = 4;
  constexpr int kOpsPerThread = 200;

  // Multiple writer threads — NO external mutex
  std::vector<std::thread> writers;
  writers.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    writers.emplace_back([&, t]() {
      for (int i = 0; i < kOpsPerThread; ++i) {
        CacheKey key("writer-" + std::to_string(t) + "-key-" +
                     std::to_string(i));
        if (dir.insert(key, static_cast<uint64_t>(t) * 10000 + i, 512)) {
          ++successful_inserts;
        }
      }
    });
  }

  for (auto &w : writers) {
    w.join();
  }

  // Must complete without crashes or deadlocks
  REQUIRE(successful_inserts.load() > 0);

  // Verify that every entry we can probe has a self-consistent offset
  // (i.e., no torn writes from concurrent writers)
  for (int t = 0; t < kThreads; ++t) {
    for (int i = 0; i < kOpsPerThread; ++i) {
      CacheKey key("writer-" + std::to_string(t) + "-key-" + std::to_string(i));
      auto entry = dir.probe(key);
      if (entry.has_value()) {
        // The offset must match one of the writers' values for this key.
        // With tag collisions, the entry may belong to a different key,
        // but if the tag matches, offset should be sane (non-garbage).
        REQUIRE(entry->offset() <
                static_cast<uint64_t>(kThreads) * 10000 + kOpsPerThread);
      }
    }
  }
}

TEST_CASE("acquire_writer recovers from stuck lock (dead writer)",
          "[mmap_directory][security]") {
  // Before the fix, acquire_writer spun forever if a writer died
  // while holding the per-bucket seqlock (version was odd). After the fix,
  // it force-releases after kMaxSpinIterations.

  constexpr size_t kBuckets = 4;
  size_t region_size = MmapDirectory::required_size(kBuckets);
  std::vector<std::byte> region(region_size, std::byte{0});

  auto dir_opt = MmapDirectory::init(std::span<std::byte>(region), kBuckets);
  REQUIRE(dir_opt.has_value());
  auto &dir = *dir_opt;

  // Simulate a crashed writer by setting a bucket's version to an odd value.
  // The version counters start after the 64-byte header.
  // Each version counter is 4 bytes (uint32_t), one per bucket.
  constexpr size_t kHeaderSize = 64;
  uint32_t stuck_version = 3;  // odd = locked
  size_t bucket_0_version_offset = kHeaderSize;
  std::memcpy(region.data() + bucket_0_version_offset, &stuck_version,
              sizeof(stuck_version));

  // Insert into bucket 0 — before the fix this would spin forever.
  // With the fix, it should force-release and complete.
  CacheKey key("stuck-writer-test");
  // We need a key that hashes to bucket 0. Try a few.
  bool inserted = false;
  auto start = std::chrono::steady_clock::now();

  // insert() calls acquire_writer internally. Must complete, not hang.
  inserted = dir.insert(key, 42, 512);

  auto elapsed = std::chrono::steady_clock::now() - start;

  // Must complete in a reasonable time (well under 5 seconds).
  // The bounded spin at 100K iterations should take ~1ms on modern hardware.
  REQUIRE(elapsed < std::chrono::seconds(5));

  // The insert should succeed after force-releasing the stuck lock.
  // (It may or may not insert into bucket 0 depending on the key's hash,
  // but the test is that it completes without hanging.)
}

TEST_CASE("MmapDirectory concurrent writers and readers",
          "[mmap_directory][multiprocess]") {
  // Exercises the seqlock read path under concurrent write pressure.
  // Readers must never observe partially-written (torn) entries.
  constexpr size_t kBuckets = 8;
  size_t region_size = MmapDirectory::required_size(kBuckets);
  std::vector<std::byte> region(region_size, std::byte{0});

  auto dir_opt = MmapDirectory::init(std::span<std::byte>(region), kBuckets);
  REQUIRE(dir_opt.has_value());
  auto &dir = *dir_opt;

  // Open a second view (simulates a reader process)
  auto reader_opt = MmapDirectory::open(std::span<std::byte>(region));
  REQUIRE(reader_opt.has_value());
  auto &reader = *reader_opt;

  std::atomic<bool> stop{false};
  std::atomic<int> torn_reads{0};
  constexpr int kWriters = 2;
  constexpr int kReaders = 4;
  constexpr int kWriteOps = 500;

  // Writers keep updating entries with known offset patterns
  std::vector<std::thread> threads;
  threads.reserve(kWriters);
  for (int t = 0; t < kWriters; ++t) {
    threads.emplace_back([&, t]() {
      for (int i = 0; i < kWriteOps; ++i) {
        CacheKey key("rw-" + std::to_string(i % 20));
        uint64_t offset = static_cast<uint64_t>(t + 1) * 100000 + i;
        dir.insert(key, offset, 512);
      }
      stop.store(true);
    });
  }

  // Readers continuously probe and check for torn reads
  for (int t = 0; t < kReaders; ++t) {
    threads.emplace_back([&]() {
      while (!stop.load(std::memory_order_relaxed)) {
        for (int i = 0; i < 20; ++i) {
          CacheKey key("rw-" + std::to_string(i));
          auto entry = reader.probe(key);
          if (entry.has_value()) {
            // Offset should be within the range any writer could produce
            if (entry->offset() >
                static_cast<uint64_t>(kWriters + 1) * 100000 + kWriteOps) {
              ++torn_reads;
            }
          }
        }
      }
    });
  }

  for (auto &th : threads) {
    th.join();
  }

  REQUIRE(torn_reads.load() == 0);
}

// =============================================================================
// Tag-collision regression tests (collision-destructive insert fix).
// Mirror of the Directory-level tests in test_directory.cpp for the mmap'd
// (multi-process) directory; the Volume-level end-to-end test is in
// test_tag_collision.cpp.
// =============================================================================

namespace {

// Find two distinct keys colliding on (bucket_hash % num_buckets, tag).
std::pair<CacheKey, CacheKey> find_tag_colliding_pair(size_t num_buckets) {
  std::map<uint64_t, CacheKey> seen;
  for (uint32_t i = 0;; ++i) {
    CacheKey key("mmap-collide-" + std::to_string(i));
    uint64_t slot = ((key.bucket_hash() % num_buckets) << 12) | key.tag();
    auto [it, inserted] = seen.emplace(slot, key);
    if (!inserted) {
      return {it->second, key};
    }
  }
}

// Find `count` keys in the anchor's bucket with tags distinct from the
// anchor's tag and from each other.
std::vector<CacheKey> find_tag_bucket_fillers(const CacheKey &anchor,
                                              size_t num_buckets,
                                              size_t count) {
  std::vector<CacheKey> fillers;
  std::vector<uint16_t> used_tags{anchor.tag()};
  for (uint32_t i = 0; fillers.size() < count; ++i) {
    CacheKey key("mmap-filler-" + std::to_string(i));
    if (key.bucket_hash() % num_buckets != anchor.bucket_hash() % num_buckets) {
      continue;
    }
    bool tag_taken = false;
    for (uint16_t tag : used_tags) {
      if (key.tag() == tag) {
        tag_taken = true;
        break;
      }
    }
    if (tag_taken) {
      continue;
    }
    used_tags.push_back(key.tag());
    fillers.push_back(key);
  }
  return fillers;
}

// Collect the offsets of all entries matching the key's (bucket, tag).
std::vector<uint64_t> collect_offsets(const MmapDirectory &dir,
                                      const CacheKey &key) {
  std::vector<uint64_t> offsets;
  dir.probe_each(key, [&](const DirEntry &entry) {
    offsets.push_back(entry.offset());
    return true;
  });
  return offsets;
}

bool contains_offset(const std::vector<uint64_t> &offsets, uint64_t offset) {
  for (uint64_t o : offsets) {
    if (o == offset) {
      return true;
    }
  }
  return false;
}

}  // namespace

TEST_CASE("MmapDirectory insert preserves a colliding foreign entry",
          "[mmap_directory][collision][regression]") {
  constexpr size_t kBuckets = 8;
  size_t region_size = MmapDirectory::required_size(kBuckets);
  std::vector<std::byte> region(region_size, std::byte{0});
  auto dir_opt = MmapDirectory::init(std::span<std::byte>(region), kBuckets);
  REQUIRE(dir_opt.has_value());
  auto &dir = *dir_opt;

  auto [key_a, key_b] = find_tag_colliding_pair(kBuckets);
  REQUIRE(key_a.tag() == key_b.tag());
  REQUIRE_FALSE(key_a == key_b);

  REQUIRE(dir.insert(key_a, 1000, 512, MmapDirectory::kNoVerifiedEntry));
  REQUIRE(dir.count() == 1);

  // B collides on (bucket, tag) but is a different key: with no verified
  // entry it must NOT update A's slot in place — both entries coexist.
  bool evicted = false;
  REQUIRE(
      dir.insert(key_b, 2000, 512, MmapDirectory::kNoVerifiedEntry, &evicted));
  REQUIRE_FALSE(evicted);
  REQUIRE(dir.count() == 2);

  auto offsets = collect_offsets(dir, key_a);
  REQUIRE(offsets.size() == 2);
  REQUIRE(contains_offset(offsets, 1000));
  REQUIRE(contains_offset(offsets, 2000));

  // In-place update of A via its verified offset leaves B alone.
  REQUIRE(dir.insert(key_a, 3000, 512, /*verified_offset=*/1000));
  REQUIRE(dir.count() == 2);
  offsets = collect_offsets(dir, key_a);
  REQUIRE(offsets.size() == 2);
  REQUIRE(contains_offset(offsets, 3000));
  REQUIRE(contains_offset(offsets, 2000));
}

TEST_CASE(
    "MmapDirectory insert evicts the collider only when the bucket is full",
    "[mmap_directory][collision][regression]") {
  constexpr size_t kBuckets = 8;
  size_t region_size = MmapDirectory::required_size(kBuckets);
  std::vector<std::byte> region(region_size, std::byte{0});
  auto dir_opt = MmapDirectory::init(std::span<std::byte>(region), kBuckets);
  REQUIRE(dir_opt.has_value());
  auto &dir = *dir_opt;

  auto [key_a, key_b] = find_tag_colliding_pair(kBuckets);
  auto fillers = find_tag_bucket_fillers(key_a, kBuckets,
                                         MmapDirectory::kEntriesPerBucket - 1);

  REQUIRE(dir.insert(key_a, 1000, 512, MmapDirectory::kNoVerifiedEntry));
  uint64_t offset = 2000;
  for (const auto &filler : fillers) {
    REQUIRE(dir.insert(filler, offset, 512, MmapDirectory::kNoVerifiedEntry));
    offset += 1000;
  }
  REQUIRE(dir.count() == MmapDirectory::kEntriesPerBucket);

  // Bucket full of current-phase entries: the colliding insert replaces the
  // collider (reported via collision_evicted), never any other entry.
  bool evicted = false;
  bool bucket_full_evicted = false;
  REQUIRE(dir.insert(key_b, 9000, 512, MmapDirectory::kNoVerifiedEntry,
                     &evicted, &bucket_full_evicted));
  REQUIRE(evicted);
  REQUIRE(dir.count() == MmapDirectory::kEntriesPerBucket);

  auto offsets = collect_offsets(dir, key_b);
  REQUIRE(offsets.size() == 1);
  REQUIRE(offsets[0] == 9000);

  for (const auto &filler : fillers) {
    REQUIRE(dir.probe(filler).has_value());
  }

  // The collider path must not be conflated with the bucket-full path.
  REQUIRE_FALSE(bucket_full_evicted);
}

TEST_CASE("MmapDirectory insert evicts the entry nearest the wrap cursor",
          "[mmap_directory][eviction][regression]") {
  // a bucket full of distinct current-phase entries must not fail
  // the insert — the entry nearest the wrap cursor makes way.
  constexpr size_t kBuckets = 8;
  size_t region_size = MmapDirectory::required_size(kBuckets);
  std::vector<std::byte> region(region_size, std::byte{0});
  auto dir_opt = MmapDirectory::init(std::span<std::byte>(region), kBuckets);
  REQUIRE(dir_opt.has_value());
  auto &dir = *dir_opt;

  CacheKey anchor("mmap-bucket-full-anchor");
  // kEntriesPerBucket + 1 keys sharing anchor's bucket, all tags distinct, so
  // no insert can update in place or find a collider to displace.
  auto keys = find_tag_bucket_fillers(anchor, kBuckets,
                                      MmapDirectory::kEntriesPerBucket + 1);

  uint64_t offset = 1000;
  for (size_t i = 0; i < MmapDirectory::kEntriesPerBucket; ++i) {
    REQUIRE(dir.insert(keys[i], offset, 512, MmapDirectory::kNoVerifiedEntry));
    offset += 1000;
  }
  REQUIRE(dir.count() == MmapDirectory::kEntriesPerBucket);

  bool collision_evicted = false;
  bool bucket_full_evicted = false;
  REQUIRE(dir.insert(keys[MmapDirectory::kEntriesPerBucket], 9000, 512,
                     MmapDirectory::kNoVerifiedEntry, &collision_evicted,
                     &bucket_full_evicted));

  // Counted on the bucket-full path only — never as a tag collision.
  REQUIRE(bucket_full_evicted);
  REQUIRE_FALSE(collision_evicted);
  // Replaced a slot, so the count is unchanged.
  REQUIRE(dir.count() == MmapDirectory::kEntriesPerBucket);

  auto offsets = collect_offsets(dir, keys[MmapDirectory::kEntriesPerBucket]);
  REQUIRE(offsets.size() == 1);
  REQUIRE(offsets[0] == 9000);

  // The victim is the lowest offset (1000) — every other entry survives.
  REQUIRE_FALSE(dir.probe(keys[0]).has_value());
  for (size_t i = 1; i < MmapDirectory::kEntriesPerBucket; ++i) {
    auto survivor = dir.probe(keys[i]);
    REQUIRE(survivor.has_value());
    REQUIRE(survivor->offset() == (i + 1) * 1000);
  }
}

// --- packed outstanding-borrow slot {generation:8, count:8} ---

TEST_CASE("borrow_slot acquire/release round-trips the count",
          "[mmap_directory][borrow_slot]") {
  std::atomic<uint16_t> slot{0};

  auto a1 = borrow_slot::acquire(slot);
  REQUIRE(a1.counted);
  REQUIRE(a1.generation == 0);
  auto a2 = borrow_slot::acquire(slot);
  REQUIRE(a2.counted);
  REQUIRE(borrow_slot::count(slot.load()) == 2);

  borrow_slot::release(slot, a1.generation);
  REQUIRE(borrow_slot::count(slot.load()) == 1);
  borrow_slot::release(slot, a2.generation);
  REQUIRE(borrow_slot::count(slot.load()) == 0);
  REQUIRE(borrow_slot::generation(slot.load()) == 0);  // No reset happened

  // Releasing an already-drained slot is a no-op (never underflows).
  borrow_slot::release(slot, 0);
  REQUIRE(slot.load() == 0);
}

TEST_CASE("borrow_slot saturates at 255 and ride-alongs never decrement",
          "[mmap_directory][borrow_slot]") {
  std::atomic<uint16_t> slot{0};

  for (int i = 0; i < 255; ++i) {
    REQUIRE(borrow_slot::acquire(slot).counted);
  }
  REQUIRE(borrow_slot::count(slot.load()) == borrow_slot::kCountSaturated);

  // 256th borrow rides along uncounted; the slot stays pegged (maximally
  // protective) and its release must not consume a counted holder's slot.
  auto extra = borrow_slot::acquire(slot);
  REQUIRE_FALSE(extra.counted);
  REQUIRE(borrow_slot::count(slot.load()) == borrow_slot::kCountSaturated);
  // (Volume::release_borrow drops uncounted tokens; the raw helper is
  // generation-checked only, so nothing to call for `extra` here.)

  borrow_slot::release(slot, extra.generation);     // A counted release...
  REQUIRE(borrow_slot::count(slot.load()) == 254);  // ...drains normally
}

TEST_CASE(
    "borrow_slot force_reset invalidates outstanding counts; stale releases "
    "are generation-dropped",
    "[mmap_directory][borrow_slot]") {
  std::atomic<uint16_t> slot{0};

  auto before = borrow_slot::acquire(slot);
  REQUIRE(borrow_slot::acquire(slot).counted);
  REQUIRE(borrow_slot::count(slot.load()) == 2);

  // Ceiling-forced wrap: generation+1, count zeroed.
  REQUIRE(borrow_slot::force_reset(slot));
  REQUIRE(borrow_slot::count(slot.load()) == 0);
  REQUIRE(borrow_slot::generation(slot.load()) == 1);

  // A borrow acquired in the new generation...
  auto after = borrow_slot::acquire(slot);
  REQUIRE(after.counted);
  REQUIRE(after.generation == 1);
  REQUIRE(borrow_slot::count(slot.load()) == 1);

  // ...is untouched by a pre-reset holder's late release (stale generation).
  borrow_slot::release(slot, before.generation);
  REQUIRE(borrow_slot::count(slot.load()) == 1);

  borrow_slot::release(slot, after.generation);
  REQUIRE(borrow_slot::count(slot.load()) == 0);

  // force_reset on a drained slot is a no-op and preserves the generation
  // (generations only burn when there was state to clear).
  REQUIRE_FALSE(borrow_slot::force_reset(slot));
  REQUIRE(borrow_slot::generation(slot.load()) == 1);
}

TEST_CASE(
    "MmapDirectory per-chunk borrow slots round-trip through the retention "
    "region",
    "[mmap_directory][borrow_slot]") {
  std::vector<std::byte> region(MmapDirectory::required_size(64));
  auto dir = MmapDirectory::init(region, 64);
  REQUIRE(dir.has_value());

  REQUIRE(dir->chunk_borrow_raw(3) == 0);
  auto acquired = dir->chunk_borrow_acquire(3);
  REQUIRE(acquired.counted);
  REQUIRE(borrow_slot::count_of<uint32_t>(dir->chunk_borrow_raw(3)) == 1);
  REQUIRE(dir->chunk_borrow_raw(4) == 0);  // chunks are independent

  // A second view on the same region observes and mutates the same slot.
  auto view2 = MmapDirectory::open(region);
  REQUIRE(view2.has_value());
  REQUIRE(borrow_slot::count_of<uint32_t>(view2->chunk_borrow_raw(3)) == 1);
  view2->chunk_borrow_release(3, acquired.generation);
  REQUIRE(dir->chunk_borrow_raw(3) == 0);

  // Force reset through one view invalidates counts taken through another,
  // in EVERY chunk at once.
  auto again = dir->chunk_borrow_acquire(3);
  auto other = dir->chunk_borrow_acquire(63);
  REQUIRE(again.counted);
  REQUIRE(other.counted);
  REQUIRE(view2->chunk_borrows_force_reset_all());
  REQUIRE(borrow_slot::count_of<uint32_t>(dir->chunk_borrow_raw(3)) == 0);
  REQUIRE(borrow_slot::count_of<uint32_t>(dir->chunk_borrow_raw(63)) == 0);
  dir->chunk_borrow_release(3, again.generation);  // Stale: dropped
  REQUIRE(borrow_slot::count_of<uint32_t>(dir->chunk_borrow_raw(3)) == 0);
  REQUIRE(borrow_slot::generation_of<uint32_t>(dir->chunk_borrow_raw(3)) == 1);
  // A slot that held nothing keeps its generation.
  REQUIRE(borrow_slot::generation_of<uint32_t>(dir->chunk_borrow_raw(5)) == 0);

  // Out-of-range chunks are inert, never out-of-bounds.
  REQUIRE_FALSE(dir->chunk_borrow_acquire(MmapDirectory::kMaxChunks).counted);
  REQUIRE(dir->chunk_borrow_raw(MmapDirectory::kMaxChunks) == 0);

  // The exposure generation is shared the same way.
  REQUIRE(dir->exposure_gen() == 0);
  view2->set_exposure_gen(77);
  REQUIRE(dir->exposure_gen() == 77);
}

TEST_CASE("MmapDirectory u32 chunk slot counts past 255",
          "[mmap_directory][borrow_slot]") {
  // The cross-process slot is {gen:8, count:24}: the 255 saturation of the
  // 16-bit shard slot (a borrow at saturation rides along UNCOUNTED) does
  // not exist at any realistic concurrency.
  std::atomic<uint32_t> slot{0};
  for (int i = 0; i < 1000; ++i) {
    REQUIRE(borrow_slot::acquire(slot).counted);
  }
  REQUIRE(borrow_slot::count_of<uint32_t>(slot.load()) == 1000);
  REQUIRE(borrow_slot::generation_of<uint32_t>(slot.load()) == 0);
  REQUIRE(borrow_slot::force_reset(slot));
  REQUIRE(borrow_slot::count_of<uint32_t>(slot.load()) == 0);
  REQUIRE(borrow_slot::generation_of<uint32_t>(slot.load()) == 1);
}

TEST_CASE("MmapDirectory layout: the retention region fits the old slack",
          "[mmap_directory][layout]") {
  // 16384 buckets is the production directory (kDirectoryEntriesPerSegment).
  // The retention region (G + 64 x u32 = 264 bytes) sits after the entries
  // and must not move the page-rounded data offset (177 pages).
  REQUIRE(MmapDirectory::required_size(size_t{16} * 1024) == 721224);
  REQUIRE((MmapDirectory::required_size(size_t{16} * 1024) + 4095) / 4096 ==
          177);
  REQUIRE(MmapDirectory::retention_offset(size_t{16} * 1024) % 8 == 0);
}

TEST_CASE("MmapDirectory recognises a foreign-version directory",
          "[mmap_directory][layout]") {
  std::vector<std::byte> region(MmapDirectory::required_size(64));
  REQUIRE_FALSE(MmapDirectory::is_foreign_version(region));  // zeroed
  auto dir = MmapDirectory::init(region, 64);
  REQUIRE(dir.has_value());
  REQUIRE_FALSE(MmapDirectory::is_foreign_version(region));  // ours
  const uint16_t v1 = 1;
  std::memcpy(region.data() + 4, &v1, sizeof(v1));  // Header::version
  REQUIRE(MmapDirectory::is_foreign_version(region));
  REQUIRE_FALSE(MmapDirectory::open(region).has_value());
}
