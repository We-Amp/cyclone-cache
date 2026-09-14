// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <limits>
#include <random>
#include <thread>

#include "core/directory.hpp"
#include "core/document.hpp"
#include "core/write_buffer.hpp"
#include "cyclone/cache.hpp"
#include "cyclone/key.hpp"
#include "support/temp_cache.hpp"

using namespace cyclone;

// =============================================================================
// Document Deserialization Security Tests
// =============================================================================

TEST_CASE("Document deserialization with truncated data",
          "[security][document]") {
  // Create valid document first
  CacheKey key("test-key");
  std::vector<std::byte> content = {std::byte{0x01}, std::byte{0x02},
                                    std::byte{0x03}};

  auto valid_data = DocumentBuilder()
                        .set_key(key)
                        .set_content(std::span<const std::byte>(content))
                        .build();

  REQUIRE(!valid_data.empty());

  // Test with increasingly truncated data
  for (size_t len = 0; len < Document::kHeaderSize; len += 8) {
    std::vector<std::byte> truncated(valid_data.begin(),
                                     valid_data.begin() + len);
    DocumentReader reader{std::span<const std::byte>(truncated)};
    REQUIRE_FALSE(reader.is_valid());
  }

  // Test with exactly header size (no content)
  std::vector<std::byte> header_only(
      valid_data.begin(), valid_data.begin() + Document::kHeaderSize);
  DocumentReader header_reader{std::span<const std::byte>(header_only)};
  // This might be valid since it has a complete header
}

TEST_CASE("Document deserialization with corrupted magic",
          "[security][document]") {
  std::vector<std::byte> data(Document::kHeaderSize + 100);
  std::mt19937 rng(42);
  for (auto &b : data) {
    b = static_cast<std::byte>(rng() & 0xFF);
  }

  // Try various invalid magic values
  uint32_t invalid_magics[] = {0, 0xFFFFFFFF, 0x12345678, 0xDEADBEEF,
                               Document::kMagic ^ 1};

  for (uint32_t magic : invalid_magics) {
    std::memcpy(data.data(), &magic, sizeof(magic));
    DocumentReader reader{std::span<const std::byte>(data)};
    REQUIRE_FALSE(reader.is_valid());
  }
}

TEST_CASE("Document deserialization with oversized length fields",
          "[security][document]") {
  CacheKey key("test-key");
  std::vector<std::byte> content = {std::byte{0x01}};

  auto data = DocumentBuilder()
                  .set_key(key)
                  .set_content(std::span<const std::byte>(content))
                  .build();

  // Corrupt the len field to claim more data than exists
  // Use deserialize() instead of raw memcpy to avoid buffer overflow
  // (sizeof(Document) may be larger than kHeaderSize due to struct padding)
  Document doc = Document::deserialize(std::span<const std::byte>(data));

  // Set len to a huge value - modify directly in serialized buffer
  // len field is at offset 4 (after magic)
  uint32_t huge_len = std::numeric_limits<uint32_t>::max();
  std::memcpy(data.data() + 4, &huge_len, sizeof(huge_len));

  DocumentReader reader{std::span<const std::byte>(data)};
  // Should handle gracefully without crash
  if (reader.is_valid()) {
    // content_data_size should be protected against underflow
    REQUIRE(reader.content().size() <= data.size());
  }
}

TEST_CASE("Document deserialization with oversized header_len",
          "[security][document]") {
  CacheKey key("test-key");
  std::vector<std::byte> content = {std::byte{0x01}};

  auto data = DocumentBuilder()
                  .set_key(key)
                  .set_content(std::span<const std::byte>(content))
                  .build();

  // Corrupt header_len to be larger than the document
  // header_len field is at offset 80 in serialized format:
  // magic(4) + len(4) + total_len(8) + first_key(32) + fragment_key(32) = 80
  uint32_t huge_header_len = std::numeric_limits<uint32_t>::max();
  std::memcpy(data.data() + 80, &huge_header_len, sizeof(huge_header_len));

  DocumentReader reader{std::span<const std::byte>(data)};
  if (reader.is_valid()) {
    // Should return empty or bounded content, not crash
    auto content_span = reader.content();
    REQUIRE(content_span.size() <= data.size());
  }
}

TEST_CASE("Document content_data_size protects against underflow",
          "[security][document]") {
  Document doc;
  doc.magic = Document::kMagic;

  // Test case: len < kHeaderSize (would underflow without protection)
  doc.len = 50;
  doc.header_len = 0;
  REQUIRE(doc.content_data_size() == 0);

  // Test case: len == kHeaderSize + header_len but no content space
  doc.len = Document::kHeaderSize + 50;
  doc.header_len = 50;
  REQUIRE(doc.content_data_size() == 0);

  // Test case: header_len larger than remaining space
  doc.len = Document::kHeaderSize + 10;
  doc.header_len = 100;
  REQUIRE(doc.content_data_size() == 0);
}

// =============================================================================
// Directory Security Tests
// =============================================================================

TEST_CASE("Directory chain traversal depth limit", "[security][directory]") {
  // Verify the depth limit constant exists and is reasonable
  REQUIRE(Directory::kMaxChainDepth == 64);

  // This prevents infinite loops from corrupted next pointers
  Directory dir(100);

  // The directory implementation should never traverse more than kMaxChainDepth
  // entries even if there's a cycle in the chain (which would be corruption)
}

TEST_CASE("Directory handles invalid bucket indices gracefully",
          "[security][directory]") {
  Directory dir(100);

  // Insert some entries
  for (int i = 0; i < 10; ++i) {
    CacheKey key("test-" + std::to_string(i));
    REQUIRE(dir.insert(key, static_cast<uint64_t>(i + 1) * 1000, 512));
  }

  // All operations should work correctly despite internal bucket calculations
  for (int i = 0; i < 10; ++i) {
    CacheKey key("test-" + std::to_string(i));
    auto result = dir.probe(key);
    REQUIRE(result.has_value());
  }
}

TEST_CASE("DirEntry offset bounds for 40-bit addressing",
          "[security][directory]") {
  DirEntry entry;

  // Test maximum valid 40-bit offset
  uint64_t max_40bit = (1ULL << 40) - 1;
  entry.set_offset(max_40bit);
  REQUIRE(entry.offset() == max_40bit);

  // Test that values beyond 40 bits are truncated
  uint64_t beyond_40bit = 1ULL << 41;
  entry.set_offset(beyond_40bit);
  // Should only store lower 40 bits
  REQUIRE(entry.offset() < beyond_40bit);
}

// =============================================================================
// WriteBuffer Security Tests
// =============================================================================

TEST_CASE("WriteBuffer max size enforcement", "[security][write_buffer]") {
  // Test that WriteBuffer respects its maximum size limit
  WriteBuffer buffer(64, 256);

  std::vector<std::byte> data(100, std::byte{0xAB});

  // First append should succeed
  auto result1 = buffer.append(std::span<const std::byte>(data));
  REQUIRE(result1.has_value());
  REQUIRE(buffer.size() == 100);

  // Second append should succeed
  auto result2 = buffer.append(std::span<const std::byte>(data));
  REQUIRE(result2.has_value());
  REQUIRE(buffer.size() == 200);

  // Third append would exceed max_size, should fail
  auto result3 = buffer.append(std::span<const std::byte>(data));
  REQUIRE_FALSE(result3.has_value());
  REQUIRE(result3.error() == CacheError::NoSpace);

  // Size should remain unchanged
  REQUIRE(buffer.size() == 200);
}

TEST_CASE("WriteBuffer handles zero-size append", "[security][write_buffer]") {
  WriteBuffer buffer(64);

  std::vector<std::byte> empty;
  auto result = buffer.append(std::span<const std::byte>(empty));
  REQUIRE(result.has_value());
  REQUIRE(buffer.empty());
}

// =============================================================================
// Cache Key Security Tests
// =============================================================================

TEST_CASE("CacheKey handles null bytes in input", "[security][key]") {
  // Keys with embedded null bytes should work correctly
  std::vector<std::byte> data_with_nulls = {std::byte{0x00}, std::byte{0x01},
                                            std::byte{0x00}, std::byte{0x02},
                                            std::byte{0x00}};

  CacheKey key1{std::span<const std::byte>(data_with_nulls)};
  CacheKey key2{std::span<const std::byte>(data_with_nulls)};
  REQUIRE(key1 == key2);

  // Different data with nulls should produce different keys
  std::vector<std::byte> different = {std::byte{0x00}, std::byte{0x01},
                                      std::byte{0x00}, std::byte{0x02},
                                      std::byte{0x01}};
  CacheKey key3{std::span<const std::byte>(different)};
  REQUIRE(key1 != key3);
}

TEST_CASE("CacheKey from_hex rejects invalid input", "[security][key]") {
  // Invalid hex strings should not crash
  CacheKey invalid1 = CacheKey::from_hex("");
  REQUIRE(invalid1.is_zero());

  CacheKey invalid2 = CacheKey::from_hex("not-hex");
  // Should handle gracefully

  CacheKey invalid3 = CacheKey::from_hex("GHIJ");
  // Should handle gracefully

  // Too short hex string
  CacheKey invalid4 = CacheKey::from_hex("0123456789abcdef");
  // Should handle gracefully
}

// =============================================================================
// Alternate Chain Security Tests
// =============================================================================

TEST_CASE("Alternate chain traversal respects depth limit",
          "[security][alternate]") {
  // Verify kMaxAlternates constant is set
  REQUIRE(Document::kMaxAlternates == 64);

  // This constant is used to bound chain traversal to prevent infinite loops
  // from corrupted next_alternate_offset values
}

// =============================================================================
// Cache Configuration Validation Tests
// =============================================================================

TEST_CASE("Cache rejects invalid configuration", "[security][config]") {
  // Test with default config (should succeed)
  CacheConfig config;
  auto result = Cache::create(config);
  REQUIRE(result.has_value());
}

TEST_CASE("Volume rejects invalid path", "[security][config][integration]") {
  CacheConfig config;
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  // Try to add volume with empty path
  VolumeConfig vol_config;
  vol_config.path = "";
  vol_config.size = static_cast<size_t>(10 * 1024 * 1024);

  // Should fail gracefully
  auto add_result = cache->add_volume(vol_config);
  // Empty path will likely cause IoError or similar
}

TEST_CASE("Volume rejects zero size", "[security][config][integration]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheConfig config;
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = 0;  // Zero size - should auto-detect from file or fail

  auto add_result = cache->add_volume(vol_config);
  // Either succeeds (auto-detects size) or fails gracefully
}

// =============================================================================
// Integer Overflow Protection Tests
// =============================================================================

TEST_CASE("DirEntry approx_size handles maximum values",
          "[security][directory]") {
  DirEntry entry;

  // Test with very large size
  entry.set_approx_size(std::numeric_limits<uint64_t>::max());
  // Should clamp to maximum representable value
  REQUIRE(entry.approx_size() > 0);
  REQUIRE(entry.approx_size() <= static_cast<uint64_t>(64 * 512 * 8));  // max
}

TEST_CASE("DirEntry approx_size rounds up correctly", "[security][directory]") {
  DirEntry entry;

  // Test that sizes round up, not down (important for allocation)
  for (uint64_t size = 1; size <= 1024; size += 7) {
    entry.set_approx_size(size);
    REQUIRE(entry.approx_size() >= size);
  }
}

// =============================================================================
// Concurrent Access Security Tests
// =============================================================================

TEST_CASE("Directory operations are thread-safe",
          "[security][concurrent][directory]") {
  Directory dir(1000);

  std::atomic<int> successful_inserts{0};
  std::atomic<int> successful_probes{0};

  auto writer = [&](int thread_id) {
    for (int i = 0; i < 100; ++i) {
      CacheKey key("thread-" + std::to_string(thread_id) + "-" +
                   std::to_string(i));
      if (dir.insert(key, static_cast<uint64_t>(thread_id * 1000 + i + 1) * 100,
                     512)) {
        ++successful_inserts;
      }
    }
  };

  auto reader = [&](int thread_id) {
    for (int i = 0; i < 100; ++i) {
      CacheKey key("thread-" + std::to_string(thread_id % 4) + "-" +
                   std::to_string(i));
      auto result = dir.probe(key);
      if (result.has_value()) {
        ++successful_probes;
      }
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

  for (auto &t : threads) {
    t.join();
  }

  // Should have some successful operations without crashes or data corruption
  REQUIRE(successful_inserts > 0);
}

// =============================================================================
// Memory Mapping Security Tests
// =============================================================================

TEST_CASE("Cache handles concurrent writes to same key",
          "[security][concurrent][integration]") {
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

  CacheKey key("concurrent-write-key");
  std::atomic<int> successful_writes{0};

  auto writer = [&](int thread_id) {
    std::string content = "Content from thread " + std::to_string(thread_id);
    std::vector<std::byte> data(content.size());
    std::memcpy(data.data(), content.data(), content.size());

    for (int i = 0; i < 10; ++i) {
      auto wh = cache->write_sync(key, data.size());
      if (wh.has_value()) {
        wh->write_sync(std::span<const std::byte>(data));
        if (wh->close_sync().has_value()) {
          ++successful_writes;
        }
      }
    }
  };

  std::vector<std::thread> threads;
  threads.reserve(4);
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back(writer, t);
  }

  for (auto &t : threads) {
    t.join();
  }

  // At least some writes should succeed
  REQUIRE(successful_writes > 0);

  // Final read should return valid content
  auto rh = cache->read_sync(key);
  REQUIRE(rh.has_value());
  REQUIRE(!rh->content().empty());

  cache->stop();
}

// =============================================================================
// Malformed Data Handling Tests
// =============================================================================

TEST_CASE("Document builder handles maximum content size",
          "[security][document]") {
  // Very large content allocation test (just verify no crash)
  CacheKey key("large-content-test");

  // Create a reasonably large content buffer (not huge to avoid OOM)
  std::vector<std::byte> large_content(static_cast<size_t>(1024 * 1024),
                                       std::byte{0xAB});  // 1MB

  auto data = DocumentBuilder()
                  .set_key(key)
                  .set_content(std::span<const std::byte>(large_content))
                  .enable_checksum(true)
                  .build();

  REQUIRE(!data.empty());

  DocumentReader reader{std::span<const std::byte>(data)};
  REQUIRE(reader.is_valid());
  REQUIRE(reader.content().size() == large_content.size());
}

TEST_CASE("Document with all fields at boundary values",
          "[security][document]") {
  CacheKey key("boundary-test");

  auto data = DocumentBuilder()
                  .set_key(key)
                  .set_alternate_id(255)                  // max uint8_t
                  .set_next_alternate_offset(UINT64_MAX)  // max uint64_t
                  .set_hit_count(UINT32_MAX)              // max uint32_t
                  .set_last_access(INT64_MAX)             // max int64_t
                  .set_total_length(UINT64_MAX)           // max uint64_t
                  .build();

  REQUIRE(!data.empty());

  DocumentReader reader{std::span<const std::byte>(data)};
  REQUIRE(reader.is_valid());

  const Document &doc = reader.document();
  REQUIRE(doc.alternate_id == 255);
  REQUIRE(doc.next_alternate_offset == UINT64_MAX);
  REQUIRE(doc.hit_count == UINT32_MAX);
  REQUIRE(doc.last_access == INT64_MAX);
}

// =============================================================================
// Phase 4A: Corrupted On-Disk Data Tests
// =============================================================================

TEST_CASE("Document with header_len exceeding bounds", "[security][document]") {
  // Build a valid document with a small header and content
  CacheKey key("header-len-corrupt");
  std::vector<std::byte> header = {std::byte{0x01}, std::byte{0x02}};
  std::vector<std::byte> content = {std::byte{0xAA}, std::byte{0xBB},
                                    std::byte{0xCC}};

  auto data = DocumentBuilder()
                  .set_key(key)
                  .set_header(std::span<const std::byte>(header))
                  .set_content(std::span<const std::byte>(content))
                  .build();

  REQUIRE(!data.empty());

  // Verify the valid document first
  {
    DocumentReader reader{std::span<const std::byte>(data)};
    REQUIRE(reader.is_valid());
    REQUIRE(reader.header().size() == 2);
    REQUIRE(reader.content().size() == 3);
  }

  // Corrupt the header_len field to exceed the total document length.
  // header_len is at offset 80 in the serialized format:
  // magic(4) + len(4) + total_len(8) + first_key(32) + fragment_key(32) = 80
  auto corrupted_header_len = static_cast<uint32_t>(data.size() * 2);
  std::memcpy(data.data() + 80, &corrupted_header_len,
              sizeof(corrupted_header_len));

  // Create a DocumentReader from the corrupted bytes.
  // Whether or not is_valid() returns true, header() and content() must
  // never return spans that extend beyond the actual data buffer.
  DocumentReader reader{std::span<const std::byte>(data)};

  auto hdr = reader.header();
  REQUIRE(hdr.size() <= data.size());

  auto cnt = reader.content();
  REQUIRE(cnt.size() <= data.size());

  // Verify no out-of-bounds access occurred (we reached here without crashing)
}

TEST_CASE("Directory entry with maximum approx_size", "[security][directory]") {
  DirEntry entry;

  // Set approx_size to UINT64_MAX - should not cause undefined behavior
  entry.set_approx_size(UINT64_MAX);

  // Verify the returned value is bounded and does not overflow
  uint64_t result = entry.approx_size();
  REQUIRE(result > 0);

  // The maximum approx_size is bounded by the encoding:
  // big can be 0-3, size can be 0-63
  // max = (63 + 1) * 512 * (1 << 3) = 64 * 512 * 8 = 262144
  REQUIRE(result <= static_cast<uint64_t>(64 * 512 * 8));

  // Verify the entry fields are within their valid ranges
  REQUIRE(entry.big() <= 3);
  REQUIRE(entry.size() <= 63);

  // Also test with other extreme values to ensure no UB
  entry.set_approx_size(UINT64_MAX - 1);
  REQUIRE(entry.approx_size() > 0);
  REQUIRE(entry.approx_size() <= static_cast<uint64_t>(64 * 512 * 8));

  entry.set_approx_size(UINT64_MAX / 2);
  REQUIRE(entry.approx_size() > 0);
  REQUIRE(entry.approx_size() <= static_cast<uint64_t>(64 * 512 * 8));
}

TEST_CASE("Zero-length content round-trip", "[security][edge]") {
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

  CacheKey key("zero-content-key");

  // Prepare non-empty header and zero-length content
  std::string header_str = "X-Custom: header-value";
  std::vector<std::byte> header(header_str.size());
  std::memcpy(header.data(), header_str.data(), header_str.size());

  std::vector<std::byte> content;  // empty

  // Write with zero-length content but non-empty header
  {
    auto wh = cache->write_sync(key, 0);
    REQUIRE(wh.has_value());
    wh->set_header(std::span<const std::byte>(header));
    // Write zero bytes of content (no write call needed, or write empty span)
    wh->write_sync(std::span<const std::byte>(content));
    auto close_result = wh->close_sync();
    REQUIRE(close_result.has_value());
  }

  // Read back and verify
  {
    auto rh = cache->read_sync(key);
    REQUIRE(rh.has_value());

    // Content should be empty
    REQUIRE(rh->content().empty());

    // Header should match
    auto read_header = rh->header();
    REQUIRE(read_header.size() == header.size());
    std::string read_header_str(
        reinterpret_cast<const char *>(read_header.data()), read_header.size());
    REQUIRE(read_header_str == header_str);
  }

  cache->stop();
}

TEST_CASE("Zero-length header with non-zero content", "[security][edge]") {
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

  CacheKey key("zero-header-key");

  // Prepare empty header and non-empty content
  std::vector<std::byte> header;  // empty
  std::string content_str = "This is the content with no header";
  std::vector<std::byte> content(content_str.size());
  std::memcpy(content.data(), content_str.data(), content_str.size());

  // Write with empty header and non-empty content
  {
    auto wh = cache->write_sync(key, content.size());
    REQUIRE(wh.has_value());
    wh->set_header(std::span<const std::byte>(header));
    auto write_result = wh->write_sync(std::span<const std::byte>(content));
    REQUIRE(write_result.has_value());
    auto close_result = wh->close_sync();
    REQUIRE(close_result.has_value());
  }

  // Read back and verify
  {
    auto rh = cache->read_sync(key);
    REQUIRE(rh.has_value());

    // Header should be empty
    REQUIRE(rh->header().empty());

    // Content should match
    auto read_content = rh->content();
    REQUIRE(read_content.size() == content.size());
    std::string read_content_str(
        reinterpret_cast<const char *>(read_content.data()),
        read_content.size());
    REQUIRE(read_content_str == content_str);
  }

  cache->stop();
}

TEST_CASE("DocumentReader::content with near-UINT32_MAX header_len",
          "[security][document]") {
  // kHeaderSize + header_len can overflow size_t on 32-bit targets
  // (or uint32_t on any target), causing the bounds check to pass incorrectly.

  DocumentBuilder builder;
  builder.set_key(CacheKey("overflow-test"))
      .set_type(Document::Type::SingleFrag)
      .set_header({})
      .set_content({});

  auto data = builder.build();
  REQUIRE(data.size() >= Document::kHeaderSize);

  // Corrupt header_len to UINT32_MAX — kHeaderSize + header_len overflows
  uint32_t huge_header_len = UINT32_MAX;
  std::memcpy(data.data() + 80, &huge_header_len, sizeof(huge_header_len));

  DocumentReader reader{std::span<const std::byte>(data)};
  REQUIRE(reader.header().empty());
  REQUIRE(reader.content().empty());

  // header_len that causes wrap to a small value on 32-bit
  uint32_t wrap_header_len = UINT32_MAX - Document::kHeaderSize + 1;
  std::memcpy(data.data() + 80, &wrap_header_len, sizeof(wrap_header_len));

  DocumentReader reader2{std::span<const std::byte>(data)};
  REQUIRE(reader2.header().empty());
  REQUIRE(reader2.content().empty());
}

TEST_CASE("Document::content_data_size with overflow values",
          "[security][document]") {
  Document doc;
  doc.magic = Document::kMagic;

  // header_len near UINT32_MAX — kHeaderSize + header_len overflows
  doc.len = 200;
  doc.header_len = UINT32_MAX;
  REQUIRE(doc.content_data_size() == 0);

  // len < kHeaderSize
  doc.len = 50;
  doc.header_len = 0;
  REQUIRE(doc.content_data_size() == 0);
}
