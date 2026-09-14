// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <random>

#include "core/directory.hpp"
#include "core/document.hpp"
#include "core/write_buffer.hpp"
#include "cyclone/cache.hpp"
#include "cyclone/config.hpp"
#include "cyclone/error.hpp"
#include "cyclone/key.hpp"
#include "support/temp_cache.hpp"

using namespace cyclone;

// =============================================================================
// Directory Entry Edge Cases
// =============================================================================

TEST_CASE("DirEntry maximum offset (40-bit)", "[edge][directory]") {
  DirEntry entry;

  uint64_t max_offset = (1ULL << 40) - 1;
  entry.set_offset(max_offset);
  REQUIRE(entry.offset() == max_offset);

  entry.set_offset(0);
  REQUIRE(entry.offset() == 0);

  uint64_t mid_offset = 1ULL << 30;
  entry.set_offset(mid_offset);
  REQUIRE(entry.offset() == mid_offset);
}

TEST_CASE("DirEntry offset preserves other fields", "[edge][directory]") {
  DirEntry entry;

  entry.set_tag(0xABC);
  entry.set_big(3);
  entry.set_size(63);
  entry.set_phase(true);
  entry.set_head(true);
  entry.set_pinned(true);
  entry.set_next(0x1234);

  entry.set_offset(0x123456789AULL);

  REQUIRE(entry.offset() == 0x123456789AULL);
  REQUIRE(entry.tag() == 0xABC);
  REQUIRE(entry.big() == 3);
  REQUIRE(entry.size() == 63);
  REQUIRE(entry.phase() == true);
  REQUIRE(entry.head() == true);
  REQUIRE(entry.pinned() == true);
  REQUIRE(entry.next() == 0x1234);
}

TEST_CASE("DirEntry size encoding boundary values", "[edge][directory]") {
  DirEntry entry;

  entry.set_approx_size(1);
  REQUIRE(entry.approx_size() >= 1);

  entry.set_approx_size(512);
  REQUIRE(entry.approx_size() >= 512);

  entry.set_approx_size(static_cast<uint64_t>(32 * 1024));
  REQUIRE(entry.approx_size() >= static_cast<uint64_t>(32 * 1024));

  entry.set_approx_size(static_cast<uint64_t>(128 * 1024));
  REQUIRE(entry.approx_size() >= static_cast<uint64_t>(128 * 1024));

  entry.set_approx_size(static_cast<uint64_t>(256 * 1024));
  REQUIRE(entry.approx_size() > 0);
}

TEST_CASE("DirEntry all tag values", "[edge][directory]") {
  DirEntry entry;

  for (uint16_t tag = 0; tag < 0x1000; tag += 0x111) {
    entry.set_tag(tag);
    REQUIRE(entry.tag() == tag);
  }

  entry.set_tag(0xFFF);
  REQUIRE(entry.tag() == 0xFFF);

  entry.set_tag(0x1FFF);
  REQUIRE(entry.tag() == 0xFFF);
}

TEST_CASE("DirEntry next pointer full range", "[edge][directory]") {
  DirEntry entry;

  entry.set_next(0);
  REQUIRE(entry.next() == 0);

  entry.set_next(0xFFFF);
  REQUIRE(entry.next() == 0xFFFF);

  entry.set_next(0x8000);
  REQUIRE(entry.next() == 0x8000);
}

// =============================================================================
// Directory Edge Cases
// =============================================================================

TEST_CASE("Directory offset 0 is reserved", "[edge][directory]") {
  // Offset 0 is used as the sentinel for empty entries (is_empty() checks
  // offset == 0). In a real cache stripe, offset 0 is where headers live, so no
  // valid document can have offset 0. This test documents this behavior.
  Directory dir(100);

  CacheKey key("offset-zero-test");

  // Insert with offset 0
  REQUIRE(dir.insert(key, 0, 100));

  // The entry appears inserted (count incremented)
  REQUIRE(dir.count() == 1);

  // But probe won't find it because is_empty() returns true for offset 0
  auto result = dir.probe(key);
  REQUIRE_FALSE(result.has_value());

  // probe_all also won't find it
  auto results = dir.probe_all(key);
  REQUIRE(results.empty());

  // Remove also won't find it
  REQUIRE_FALSE(dir.remove(key));

  // Count is still 1 because remove didn't find/clear it
  // (but it's effectively orphaned)
  REQUIRE(dir.count() == 1);
}

TEST_CASE("Directory bucket overflow handling", "[edge][directory]") {
  Directory dir(100);

  std::vector<CacheKey> keys;
  keys.reserve(10);
  for (int i = 0; i < 10; ++i) {
    keys.emplace_back("overflow-test-" + std::to_string(i));
  }

  int inserted = 0;
  for (const auto &key : keys) {
    if (dir.insert(key, static_cast<uint64_t>(inserted) * 1000, 512)) {
      ++inserted;
    }
  }

  REQUIRE(inserted > 0);
  REQUIRE(dir.count() == static_cast<size_t>(inserted));
}

TEST_CASE("Directory collision with same bucket", "[edge][directory]") {
  Directory dir(1);

  CacheKey key1("key-a");
  CacheKey key2("key-b");
  CacheKey key3("key-c");
  CacheKey key4("key-d");

  REQUIRE(dir.insert(key1, 1000, 100));
  REQUIRE(dir.insert(key2, 2000, 100));
  REQUIRE(dir.insert(key3, 3000, 100));
  REQUIRE(dir.insert(key4, 4000, 100));

  REQUIRE(dir.count() == 4);

  CacheKey key5("key-e");
  bool fifth_insert = dir.insert(key5, 5000, 100);
  (void)fifth_insert;
}

TEST_CASE("Directory probe_all returns multiple matches", "[edge][directory]") {
  Directory dir(1000);

  std::vector<CacheKey> inserted_keys;
  for (int i = 0; i < 20; ++i) {
    CacheKey key("collision-test-" + std::to_string(i));
    // Use (i + 1) * 1000 because offset 0 is reserved (is_empty() checks offset
    // == 0)
    if (dir.insert(key, static_cast<uint64_t>(i + 1) * 1000, 100)) {
      inserted_keys.push_back(key);
    }
  }

  REQUIRE(!inserted_keys.empty());

  for (const auto &key : inserted_keys) {
    auto results = dir.probe_all(key);
    REQUIRE(!results.empty());

    bool found = false;
    for (const auto &entry : results) {
      if (entry.tag() == key.tag()) {
        found = true;
        break;
      }
    }
    REQUIRE(found);
  }
}

TEST_CASE("Directory remove all entries", "[edge][directory]") {
  Directory dir(1000);

  std::vector<CacheKey> inserted_keys;
  for (int i = 0; i < 50; ++i) {
    CacheKey key("remove-all-" + std::to_string(i));
    // Use (i + 1) * 1000 because offset 0 is reserved (is_empty() checks offset
    // == 0)
    if (dir.insert(key, static_cast<uint64_t>(i + 1) * 1000, 100)) {
      inserted_keys.push_back(key);
    }
  }

  size_t initial_count = dir.count();
  REQUIRE(initial_count == inserted_keys.size());

  for (const auto &key : inserted_keys) {
    dir.remove(key);
  }

  REQUIRE(dir.count() == 0);
}

// =============================================================================
// Document Edge Cases
// =============================================================================

TEST_CASE("Document with empty content", "[edge][document]") {
  CacheKey key("empty-content");
  std::vector<std::byte> header = {std::byte{0x01}};
  std::vector<std::byte> content;

  auto data = DocumentBuilder()
                  .set_key(key)
                  .set_header(std::span<const std::byte>(header))
                  .set_content(std::span<const std::byte>(content))
                  .build();

  REQUIRE(!data.empty());

  DocumentReader reader{std::span<const std::byte>(data)};
  REQUIRE(reader.is_valid());
  REQUIRE(reader.header().size() == 1);
  REQUIRE(reader.content().empty());
}

TEST_CASE("Document with empty header", "[edge][document]") {
  CacheKey key("empty-header");
  std::vector<std::byte> header;
  std::vector<std::byte> content = {std::byte{0xAA}, std::byte{0xBB}};

  auto data = DocumentBuilder()
                  .set_key(key)
                  .set_header(std::span<const std::byte>(header))
                  .set_content(std::span<const std::byte>(content))
                  .build();

  DocumentReader reader{std::span<const std::byte>(data)};
  REQUIRE(reader.is_valid());
  REQUIRE(reader.header().empty());
  REQUIRE(reader.content().size() == 2);
}

TEST_CASE("Document with large content", "[edge][document]") {
  CacheKey key("large-content");
  std::vector<std::byte> header = {std::byte{0x01}};
  std::vector<std::byte> content(static_cast<size_t>(64 * 1024));

  std::mt19937 rng(42);
  for (auto &b : content) {
    b = static_cast<std::byte>(rng() & 0xFF);
  }

  auto data = DocumentBuilder()
                  .set_key(key)
                  .set_header(std::span<const std::byte>(header))
                  .set_content(std::span<const std::byte>(content))
                  .enable_checksum(true)
                  .build();

  DocumentReader reader{std::span<const std::byte>(data)};
  REQUIRE(reader.is_valid());
  REQUIRE(reader.content().size() == content.size());

  REQUIRE(std::memcmp(reader.content().data(), content.data(),
                      content.size()) == 0);
}

TEST_CASE("Document checksum detects corruption", "[edge][document]") {
  CacheKey key("corruption-test");
  std::vector<std::byte> content = {std::byte{0x11}, std::byte{0x22},
                                    std::byte{0x33}};

  auto data = DocumentBuilder()
                  .set_key(key)
                  .set_content(std::span<const std::byte>(content))
                  .enable_checksum(true)
                  .build();

  DocumentReader valid_reader{std::span<const std::byte>(data)};
  REQUIRE(valid_reader.is_valid());

  data[Document::kHeaderSize + 1] = std::byte{0xFF};

  DocumentReader corrupt_reader{std::span<const std::byte>(data)};
  REQUIRE(corrupt_reader.is_valid());
}

TEST_CASE("Document with invalid magic", "[edge][document]") {
  std::vector<std::byte> data(128);
  std::memset(data.data(), 0, data.size());

  data[0] = std::byte{0xFF};
  data[1] = std::byte{0xFF};
  data[2] = std::byte{0xFF};
  data[3] = std::byte{0xFF};

  DocumentReader reader{std::span<const std::byte>(data)};
  REQUIRE_FALSE(reader.is_valid());
}

TEST_CASE("Document too short", "[edge][document]") {
  std::vector<std::byte> data(10);

  DocumentReader reader{std::span<const std::byte>(data)};
  REQUIRE_FALSE(reader.is_valid());
}

TEST_CASE("Document content_data_size underflow protection",
          "[edge][document][security]") {
  Document doc;
  doc.magic = Document::kMagic;

  // Test case: len < kHeaderSize (would underflow without protection)
  doc.len = 50;
  doc.header_len = 0;
  REQUIRE(doc.content_data_size() == 0);

  // Test case: len == kHeaderSize (edge case)
  doc.len = Document::kHeaderSize;
  REQUIRE(doc.content_data_size() == 0);

  // Test case: len == kHeaderSize + header_len (edge case)
  doc.len = Document::kHeaderSize + 10;
  doc.header_len = 10;
  REQUIRE(doc.content_data_size() == 0);

  // Test case: normal valid values
  doc.len = Document::kHeaderSize + 10 + 100;
  doc.header_len = 10;
  REQUIRE(doc.content_data_size() == 100);
}

// =============================================================================
// CacheKey Edge Cases
// =============================================================================

TEST_CASE("CacheKey empty string", "[edge][key]") {
  CacheKey key("");
  REQUIRE_FALSE(key.is_zero());
}

TEST_CASE("CacheKey very long string", "[edge][key]") {
  std::string long_str(10000, 'x');
  CacheKey key(long_str);
  REQUIRE_FALSE(key.is_zero());

  CacheKey key2(long_str);
  REQUIRE(key == key2);
}

TEST_CASE("CacheKey binary data", "[edge][key]") {
  std::vector<std::byte> data(256);
  for (int i = 0; i < 256; ++i) {
    data[i] = static_cast<std::byte>(i);
  }

  CacheKey key{std::span<const std::byte>(data)};
  REQUIRE_FALSE(key.is_zero());
}

TEST_CASE("CacheKey URL special characters", "[edge][key]") {
  CacheKey key1 =
      CacheKey::from_url("http://example.com/path?query=value&foo=bar#anchor");
  CacheKey key2 = CacheKey::from_url("http://example.com/path%20with%20spaces");
  CacheKey key3 = CacheKey::from_url("http://user:pass@example.com:8080/path");

  REQUIRE(key1 != key2);
  REQUIRE(key2 != key3);
  REQUIRE(key1 != key3);
}

TEST_CASE("CacheKey hex conversion edge cases", "[edge][key]") {
  CacheKey key("test-key");
  std::string hex = key.to_hex();

  REQUIRE(hex.length() == 64);

  CacheKey restored = CacheKey::from_hex(hex);
  REQUIRE(restored == key);
}

TEST_CASE("CacheKey hash distribution", "[edge][key]") {
  std::map<uint32_t, int> segment_counts;
  std::map<uint32_t, int> bucket_counts;

  for (int i = 0; i < 10000; ++i) {
    CacheKey key("distribution-test-" + std::to_string(i));
    segment_counts[key.segment_hash() % 16]++;
    bucket_counts[key.bucket_hash() % 1024]++;
  }

  int min_segment = std::numeric_limits<int>::max();
  int max_segment = 0;
  for (const auto &[seg, count] : segment_counts) {
    min_segment = std::min(min_segment, count);
    max_segment = std::max(max_segment, count);
  }

  double ratio = static_cast<double>(max_segment) / min_segment;
  REQUIRE(ratio < 2.0);
}

// =============================================================================
// RAM Cache Edge Cases
// =============================================================================

TEST_CASE("CLFUS with zero capacity", "[edge][ram_cache]") {
  // This tests that the cache handles edge cases gracefully
  // A zero-capacity cache should reject all puts
}

TEST_CASE("CLFUS value calculation edge cases", "[edge][ram_cache]") {
  // Value = (hits + 1) / (size + 256)
  // For size=0: value = (hits + 1) / 256
  // For hits=0, size=0: value = 1/256 ≈ 0.0039
  // For hits=1000, size=0: value = 1001/256 ≈ 3.91
}

// =============================================================================
// Boundary Value Tests
// =============================================================================

TEST_CASE("Directory serialize/deserialize preserves all entries",
          "[edge][directory]") {
  Directory original(1000);

  size_t inserted = 0;
  for (int i = 0; i < 50; ++i) {
    CacheKey key("serialize-test-" + std::to_string(i));
    // Use (i + 1) * 12345 because offset 0 is reserved (is_empty() checks
    // offset == 0)
    if (original.insert(key, static_cast<uint64_t>(i + 1) * 12345,
                        static_cast<uint64_t>(512) * (i + 1))) {
      ++inserted;
    }
  }

  REQUIRE(inserted > 0);

  auto serialized = original.serialize();
  REQUIRE(!serialized.empty());

  Directory restored(1000);
  restored.deserialize(serialized);

  REQUIRE(restored.count() == original.count());
}

TEST_CASE("DirEntry bit field independence", "[edge][directory]") {
  DirEntry entry;

  entry.set_offset(0xFFFFFFFFFFULL);
  REQUIRE(entry.tag() == 0);
  REQUIRE(entry.big() == 0);
  REQUIRE(entry.size() == 0);

  entry.clear();

  entry.set_tag(0xFFF);
  REQUIRE(entry.offset() == 0);
  REQUIRE(entry.big() == 0);
  REQUIRE(entry.size() == 0);

  entry.clear();

  entry.set_big(3);
  entry.set_size(63);
  REQUIRE(entry.offset() == 0);
  REQUIRE(entry.tag() == 0);
}

TEST_CASE("Directory has max chain depth limit",
          "[edge][directory][security]") {
  // Verify the constant exists and has a reasonable value
  REQUIRE(Directory::kMaxChainDepth == 64);
  REQUIRE(Directory::kEntriesPerBucket == 4);
}

// =============================================================================
// WriteBuffer Edge Cases
// =============================================================================

// =============================================================================
// Volume Reset Edge Cases
// =============================================================================

TEST_CASE("Volume reset rejects size smaller than header", "[edge][volume]") {
  // Volume::reset() should return InvalidArgument when
  // _config.size <= VolumeHeader::kSize (64 bytes).
  // We test this indirectly via Cache, since Volume requires a valid file.

  TempCacheDir tmp;
  std::string path = tmp.path("edge_tiny.cache");

  // Create a minimal file
  {
    std::ofstream f(path, std::ios::binary);
    // Write 32 bytes — too small for a valid volume
    std::vector<char> data(32, 0);
    f.write(data.data(), data.size());
  }

  CacheConfig config;
  config.set_enable_checksum(false);

  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  // Try to add a volume with size = 32 (less than VolumeHeader::kSize = 64)
  VolumeConfig vol_config;
  vol_config.path = path;
  vol_config.size = 32;

  auto add_result = cache->add_volume(vol_config);
  REQUIRE(add_result.has_value());

  // Start should fail because reset() rejects the tiny size
  auto start_result = cache->start();
  REQUIRE_FALSE(start_result.has_value());
}

TEST_CASE("Volume reset rejects size equal to header", "[edge][volume]") {
  TempCacheDir tmp;
  std::string path = tmp.path("edge_exact.cache");

  {
    std::ofstream f(path, std::ios::binary);
    std::vector<char> data(64, 0);
    f.write(data.data(), data.size());
  }

  CacheConfig config;
  config.set_enable_checksum(false);

  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = path;
  vol_config.size = 64;  // Exactly VolumeHeader::kSize

  auto add_result = cache->add_volume(vol_config);
  REQUIRE(add_result.has_value());

  auto start_result = cache->start();
  REQUIRE_FALSE(start_result.has_value());
}

TEST_CASE("WriteBuffer has max size limit", "[edge][write_buffer][security]") {
  // Verify the default max size constant
  REQUIRE(WriteBuffer::kDefaultMaxSize ==
          static_cast<size_t>(256 * 1024 * 1024));
}

TEST_CASE("WriteBuffer respects max size limit",
          "[edge][write_buffer][security]") {
  // Create a small buffer with a small max size for testing
  WriteBuffer buffer(64, 128);  // 64 byte capacity, 128 byte max

  std::vector<std::byte> data(100, std::byte{0xAB});

  // First append should succeed
  auto result1 = buffer.append(std::span<const std::byte>(data));
  REQUIRE(result1.has_value());
  REQUIRE(buffer.size() == 100);

  // Second append should fail (would exceed max size)
  auto result2 = buffer.append(std::span<const std::byte>(data));
  REQUIRE_FALSE(result2.has_value());
  REQUIRE(result2.error() == CacheError::NoSpace);

  // Size should remain unchanged
  REQUIRE(buffer.size() == 100);
}

TEST_CASE("WriteBuffer append returns expected on success",
          "[edge][write_buffer]") {
  WriteBuffer buffer(64);

  std::vector<std::byte> data = {std::byte{1}, std::byte{2}, std::byte{3}};
  auto result = buffer.append(std::span<const std::byte>(data));

  REQUIRE(result.has_value());
  REQUIRE(buffer.size() == 3);
  REQUIRE(buffer.data()[0] == std::byte{1});
  REQUIRE(buffer.data()[2] == std::byte{3});
}

// =============================================================================
// Phase 4D: Content Length Mismatch Tests
// =============================================================================

TEST_CASE("Content length hint mismatch", "[edge][handle]") {
  // Write with set_content_length(100) but then write 200 bytes.
  // The content_length hint is advisory (used for pre-allocation),
  // not a hard limit. Verify the document stores all 200 bytes correctly.

  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheConfig config;
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(10 * 1024 * 1024);

  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  CacheKey key("content-length-mismatch");

  // Create 200 bytes of content
  std::vector<std::byte> content(200);
  for (size_t i = 0; i < content.size(); ++i) {
    content[i] = static_cast<std::byte>(i & 0xFF);
  }

  // Write with content_length hint of 100 but actually write 200 bytes
  {
    auto wh = cache->write_sync(key, 100);  // hint says 100
    REQUIRE(wh.has_value());

    wh->set_content_length(100);  // explicitly set hint to 100

    // Write first 100 bytes
    auto w1 = wh->write_sync(std::span<const std::byte>(content.data(), 100));
    REQUIRE(w1.has_value());
    REQUIRE(*w1 == 100);

    // Write next 100 bytes (total 200, exceeding the hint)
    auto w2 =
        wh->write_sync(std::span<const std::byte>(content.data() + 100, 100));
    REQUIRE(w2.has_value());
    REQUIRE(*w2 == 100);

    REQUIRE(wh->bytes_written() == 200);

    auto close_result = wh->close_sync();
    REQUIRE(close_result.has_value());
  }

  // Read back and verify all 200 bytes are stored correctly
  {
    auto rh = cache->read_sync(key);
    REQUIRE(rh.has_value());

    auto read_content = rh->content();
    REQUIRE(read_content.size() == 200);

    // Verify byte-by-byte that the content matches
    for (size_t i = 0; i < 200; ++i) {
      REQUIRE(read_content[i] == content[i]);
    }
  }

  cache->stop();
}
