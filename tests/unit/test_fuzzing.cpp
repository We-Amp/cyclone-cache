// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <random>

#include "core/directory.hpp"
#include "core/document.hpp"
#include "core/volume.hpp"
#include "cyclone/cache.hpp"
#include "cyclone/key.hpp"
#include "support/temp_cache.hpp"

using namespace cyclone;

namespace {

// Generate random bytes
void fill_random(std::byte *buffer, size_t len, uint32_t seed) {
  std::mt19937 rng(seed);
  for (size_t i = 0; i < len; ++i) {
    buffer[i] = static_cast<std::byte>(rng() & 0xFF);
  }
}

}  // namespace

// =============================================================================
// Document Deserialization Fuzzing
// =============================================================================

TEST_CASE("DocumentReader handles completely random bytes",
          "[fuzzing][document]") {
  // Test with multiple random seeds
  for (uint32_t seed = 0; seed < 100; ++seed) {
    std::vector<std::byte> random_data(256);
    fill_random(random_data.data(), random_data.size(), seed);

    // Should not crash
    DocumentReader reader{std::span<const std::byte>(random_data)};

    // Most random data won't have valid magic
    // Just verify no crash and can query methods
    (void)reader.is_valid();
    if (reader.is_valid()) {
      (void)reader.header();
      (void)reader.content();
      (void)reader.document();
    }
  }
}

TEST_CASE("DocumentReader handles valid magic with random rest",
          "[fuzzing][document]") {
  for (uint32_t seed = 0; seed < 50; ++seed) {
    std::vector<std::byte> data(Document::kHeaderSize + 100);
    fill_random(data.data(), data.size(), seed);

    // Set valid magic AND a valid format version: DocumentReader gates on
    // BOTH (a foreign version must read as a miss, not be misparsed -- see
    // DocumentReader's constructor).  A random version_major byte would fail
    // that gate 255 times out of 256 and this case would stop fuzzing
    // anything.  EVERY OTHER FIELD stays random, which is the point: the
    // reader must survive garbage len/total_len/header_len.
    uint32_t magic = Document::kMagic;
    std::memcpy(data.data(), &magic, sizeof(magic));
    auto version = static_cast<std::byte>(Document::kVersionMajor);
    data[Document::kVersionMajorOffset] = version;

    DocumentReader reader{std::span<const std::byte>(data)};

    // Should not crash - is_valid will be true, but data may be nonsense
    REQUIRE(reader.is_valid());

    // These should not crash even with garbage data
    auto header = reader.header();
    auto content = reader.content();
    (void)header;
    (void)content;

    // Access document fields - should not crash
    const Document &doc = reader.document();
    (void)doc.len;
    (void)doc.total_len;
    (void)doc.header_len;
    (void)doc.content_data_size();
  }
}

TEST_CASE("DocumentReader handles overflow length values",
          "[fuzzing][document]") {
  CacheKey key("test");
  std::vector<std::byte> content = {std::byte{1}, std::byte{2}};

  auto data = DocumentBuilder()
                  .set_key(key)
                  .set_content(std::span<const std::byte>(content))
                  .build();

  // Test various overflow scenarios
  struct TestCase {
    size_t offset;
    uint32_t value;
  };

  // Offsets: len(4), total_len(8), header_len at 80
  TestCase cases[] = {
      {4, UINT32_MAX},                           // len = MAX
      {4, 0},                                    // len = 0
      {80, UINT32_MAX},                          // header_len = MAX
      {80, static_cast<uint32_t>(data.size())},  // header_len = entire doc
  };

  for (const auto &tc : cases) {
    std::vector<std::byte> corrupted = data;
    std::memcpy(corrupted.data() + tc.offset, &tc.value, sizeof(tc.value));

    DocumentReader reader{std::span<const std::byte>(corrupted)};
    // Should not crash
    if (reader.is_valid()) {
      auto h = reader.header();
      auto c = reader.content();
      // Sizes should be bounded
      REQUIRE(h.size() <= corrupted.size());
      REQUIRE(c.size() <= corrupted.size());
    }
  }
}

TEST_CASE("DocumentReader handles huge next_alternate_offset",
          "[fuzzing][document]") {
  CacheKey key("test");
  std::vector<std::byte> content = {std::byte{1}};

  auto data = DocumentBuilder()
                  .set_key(key)
                  .set_content(std::span<const std::byte>(content))
                  .set_next_alternate_offset(UINT64_MAX)
                  .build();

  DocumentReader reader{std::span<const std::byte>(data)};
  REQUIRE(reader.is_valid());

  const Document &doc = reader.document();
  REQUIRE(doc.next_alternate_offset == UINT64_MAX);
  // Should not crash or cause issues
}

TEST_CASE("DocumentReader handles invalid doc_type values",
          "[fuzzing][document]") {
  CacheKey key("test");
  std::vector<std::byte> content = {std::byte{1}};

  auto data = DocumentBuilder()
                  .set_key(key)
                  .set_content(std::span<const std::byte>(content))
                  .build();

  // doc_type is at offset 84 (after header_len at 80)
  for (uint8_t invalid_type : {5, 10, 50, 100, 200, 255}) {
    std::vector<std::byte> corrupted = data;
    corrupted[84] = static_cast<std::byte>(invalid_type);

    DocumentReader reader{std::span<const std::byte>(corrupted)};
    // Should not crash
    if (reader.is_valid()) {
      const Document &doc = reader.document();
      // Methods should handle unknown types gracefully
      (void)doc.is_complete();
      (void)doc.is_first();
    }
  }
}

TEST_CASE("DocumentReader handles document truncated at every offset",
          "[fuzzing][document]") {
  CacheKey key("test");
  std::string content_str = "Some test content data";
  std::vector<std::byte> content(content_str.size());
  std::memcpy(content.data(), content_str.data(), content_str.size());

  auto full_data = DocumentBuilder()
                       .set_key(key)
                       .set_content(std::span<const std::byte>(content))
                       .build();

  // Try truncating at every byte offset
  for (size_t truncate_at = 0; truncate_at <= full_data.size(); ++truncate_at) {
    std::vector<std::byte> truncated(full_data.begin(),
                                     full_data.begin() + truncate_at);

    DocumentReader reader{std::span<const std::byte>(truncated)};
    // Should not crash
    (void)reader.is_valid();
    if (reader.is_valid()) {
      auto h = reader.header();
      auto c = reader.content();
      REQUIRE(h.size() <= truncated.size());
      REQUIRE(c.size() <= truncated.size());
    }
  }
}

// =============================================================================
// Directory Entry Fuzzing
// =============================================================================

TEST_CASE("DirEntry handles random bit patterns", "[fuzzing][directory]") {
  for (uint32_t seed = 0; seed < 100; ++seed) {
    DirEntry entry;
    // Directly fill the entry with random data
    fill_random(reinterpret_cast<std::byte *>(&entry), sizeof(entry), seed);

    // All accessors should work without crash
    (void)entry.offset();
    (void)entry.tag();
    (void)entry.big();
    (void)entry.size();
    (void)entry.phase();
    (void)entry.head();
    (void)entry.pinned();
    (void)entry.next();
    (void)entry.is_empty();
    (void)entry.approx_size();
  }
}

TEST_CASE("DirEntry handles maximum 40-bit offset", "[fuzzing][directory]") {
  DirEntry entry;

  // Test various large offset values
  uint64_t test_offsets[] = {
      (1ULL << 40) - 1,  // Max 40-bit
      (1ULL << 40),      // Just over 40-bit (should truncate)
      (1ULL << 41),      // Way over
      UINT64_MAX,        // Maximum
      0,                 // Zero
      1,                 // One
  };

  for (uint64_t offset : test_offsets) {
    entry.set_offset(offset);
    uint64_t read_back = entry.offset();
    // Should be bounded to 40 bits
    REQUIRE(read_back <= ((1ULL << 40) - 1));
  }
}

TEST_CASE("DirEntry handles all big/size combinations",
          "[fuzzing][directory]") {
  DirEntry entry;

  // big is 2 bits (0-3), size is 6 bits (0-63)
  for (uint8_t big = 0; big <= 3; ++big) {
    for (uint8_t size = 0; size <= 63; ++size) {
      entry.set_big(big);
      entry.set_size(size);

      REQUIRE(entry.big() == big);
      REQUIRE(entry.size() == size);

      // approx_size should not overflow
      uint64_t approx = entry.approx_size();
      REQUIRE(approx > 0);
      REQUIRE(approx <= 64ULL * 512 * 8);  // Max theoretical value
    }
  }
}

TEST_CASE("DirEntry set_approx_size handles edge values",
          "[fuzzing][directory]") {
  DirEntry entry;

  uint64_t test_sizes[] = {
      0,
      1,
      511,
      512,
      513,
      1024,
      static_cast<uint64_t>(32 * 1024),
      static_cast<uint64_t>(64 * 1024),
      static_cast<uint64_t>(128 * 1024),
      static_cast<uint64_t>(256 * 1024),
      static_cast<uint64_t>(1024 * 1024),
      UINT64_MAX,
  };

  for (uint64_t size : test_sizes) {
    entry.set_approx_size(size);
    // Should not crash, result should be reasonable
    uint64_t approx = entry.approx_size();
    if (size > 0) {
      REQUIRE(approx >=
              std::min(size, uint64_t{512}));  // At least minimum block or size
    }
  }
}

// =============================================================================
// VolumeHeader Fuzzing
// =============================================================================

TEST_CASE("VolumeHeader handles random bytes", "[fuzzing][volume]") {
  for (uint32_t seed = 0; seed < 100; ++seed) {
    std::byte buffer[VolumeHeader::kSize];
    fill_random(buffer, VolumeHeader::kSize, seed);

    VolumeHeader header = VolumeHeader::deserialize(buffer);

    // Should not crash
    (void)header.is_valid();
    (void)header.is_compatible();
    (void)header.magic;
    (void)header.format_version_major;
    (void)header.format_version_minor;
    (void)header.creation_time;
    (void)header.volume_size;
  }
}

TEST_CASE("VolumeHeader handles truncated buffer", "[fuzzing][volume]") {
  // Note: deserialize requires kSize bytes, but let's test what happens
  // with valid header then serialized to smaller buffer (shouldn't happen in
  // practice)
  VolumeHeader header;
  header.magic = VolumeHeader::kMagic;

  std::byte buffer[VolumeHeader::kSize];
  header.serialize(buffer);

  // Deserialize should work
  VolumeHeader restored = VolumeHeader::deserialize(buffer);
  REQUIRE(restored.is_valid());
}

TEST_CASE("VolumeHeader handles version boundary values", "[fuzzing][volume]") {
  VolumeHeader header;
  header.magic = VolumeHeader::kMagic;

  uint16_t test_versions[] = {0, 1, 2, 3, 100, 255, 256, 65535};

  for (uint16_t major : test_versions) {
    for (uint16_t minor : test_versions) {
      header.format_version_major = major;
      header.format_version_minor = minor;

      std::byte buffer[VolumeHeader::kSize];
      header.serialize(buffer);

      VolumeHeader restored = VolumeHeader::deserialize(buffer);
      REQUIRE(restored.format_version_major == major);
      REQUIRE(restored.format_version_minor == minor);

      // is_compatible should not crash
      (void)restored.is_compatible();
    }
  }
}

// =============================================================================
// Cache Operation Fuzzing
// =============================================================================

TEST_CASE("Cache handles keys with all byte values",
          "[fuzzing][cache][integration]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheConfig config;
  auto cache = Cache::create(config);
  REQUIRE(cache.has_value());

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(20 * 1024 * 1024);

  REQUIRE((*cache)->add_volume(vol_config).has_value());
  REQUIRE((*cache)->start().has_value());

  // Create keys with various byte patterns
  for (int i = 0; i < 256; i += 17) {
    std::vector<std::byte> key_data(32);
    for (auto &b : key_data) {
      b = static_cast<std::byte>(i);
    }

    CacheKey key{std::span<const std::byte>(key_data)};
    std::string content = "Content for byte " + std::to_string(i);
    std::vector<std::byte> data(content.size());
    std::memcpy(data.data(), content.data(), content.size());

    auto wh = (*cache)->write_sync(key, data.size());
    if (wh.has_value()) {
      wh->write_sync(std::span<const std::byte>(data));
      wh->close_sync();
    }
  }

  (*cache)->stop();
}

TEST_CASE("Cache handles zero-length content",
          "[fuzzing][cache][integration]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheConfig config;
  auto cache = Cache::create(config);
  REQUIRE(cache.has_value());

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(10 * 1024 * 1024);

  REQUIRE((*cache)->add_volume(vol_config).has_value());
  REQUIRE((*cache)->start().has_value());

  CacheKey key("zero-length-key");
  std::vector<std::byte> empty;

  auto wh = (*cache)->write_sync(key, 0);
  if (wh.has_value()) {
    wh->write_sync(std::span<const std::byte>(empty));
    auto result = wh->close_sync();
    // May succeed or fail - should not crash
    (void)result;
  }

  (*cache)->stop();
}

TEST_CASE("Cache handles interleaved random operations",
          "[fuzzing][cache][integration]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheConfig config;
  auto cache = Cache::create(config);
  REQUIRE(cache.has_value());

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(30 * 1024 * 1024);

  REQUIRE((*cache)->add_volume(vol_config).has_value());
  REQUIRE((*cache)->start().has_value());

  std::mt19937 rng(42);
  std::uniform_int_distribution<int> op_dist(0, 3);
  std::uniform_int_distribution<int> key_dist(0, 99);

  for (int i = 0; i < 500; ++i) {
    int op = op_dist(rng);
    int idx = key_dist(rng);
    CacheKey key("fuzz-key-" + std::to_string(idx));

    switch (op) {
      case 0: {  // Write
        std::string content = "Content " + std::to_string(i);
        std::vector<std::byte> data(content.size());
        std::memcpy(data.data(), content.data(), content.size());

        auto wh = (*cache)->write_sync(key, data.size());
        if (wh.has_value()) {
          wh->write_sync(std::span<const std::byte>(data));
          wh->close_sync();
        }
        break;
      }
      case 1: {  // Read
        auto rh = (*cache)->read_sync(key);
        if (rh.has_value()) {
          auto content = rh->content();
          (void)content;
        }
        break;
      }
      case 2: {  // Exists
        auto result = (*cache)->exists_sync(key);
        (void)result;
        break;
      }
      case 3: {  // Remove
        auto result = (*cache)->remove_sync(key);
        (void)result;
        break;
      }
      default:
        break;
    }
  }

  (*cache)->stop();
}

TEST_CASE("Cache handles rapid create/destroy cycles with random data",
          "[fuzzing][cache][integration]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  std::mt19937 rng(123);

  for (int cycle = 0; cycle < 10; ++cycle) {
    CacheConfig config;
    auto cache = Cache::create(config);
    REQUIRE(cache.has_value());

    VolumeConfig vol_config;
    vol_config.path = cache_path;
    vol_config.size = static_cast<size_t>(10 * 1024 * 1024);

    REQUIRE((*cache)->add_volume(vol_config).has_value());
    REQUIRE((*cache)->start().has_value());

    // Random operations
    for (int i = 0; i < 20; ++i) {
      CacheKey key("cycle-" + std::to_string(cycle) + "-" + std::to_string(i));

      std::vector<std::byte> data(rng() % 1000 + 1);
      fill_random(data.data(), data.size(), rng());

      auto wh = (*cache)->write_sync(key, data.size());
      if (wh.has_value()) {
        wh->write_sync(std::span<const std::byte>(data));
        wh->close_sync();
      }
    }

    (*cache)->stop();
  }
}

// =============================================================================
// CacheKey Fuzzing
// =============================================================================

TEST_CASE("CacheKey handles all possible single-byte keys", "[fuzzing][key]") {
  for (int b = 0; b < 256; ++b) {
    std::byte data[1] = {static_cast<std::byte>(b)};
    CacheKey key{std::span<const std::byte>(data, 1)};

    // Should not crash
    REQUIRE_FALSE(key.is_zero());
    (void)key.to_hex();
    (void)key.segment_hash();
    (void)key.bucket_hash();
    (void)key.tag();
  }
}

TEST_CASE("CacheKey from_hex handles invalid input", "[fuzzing][key]") {
  // Various invalid hex strings - should not crash
  std::string invalid_inputs[] = {
      "",
      "g",
      "GH",
      "not-hex-at-all",
      "0123456789abcdef",     // Too short
      std::string(128, 'f'),  // Too long
      std::string(64, 'g'),   // Right length, wrong chars
      std::string(63, 'a'),   // 63 chars - one short of valid
  };

  for (const auto &input : invalid_inputs) {
    CacheKey key = CacheKey::from_hex(input);
    // Should not crash - may return zero key or partial result
    (void)key.is_zero();
  }
}

TEST_CASE("CacheKey handles maximum length input", "[fuzzing][key]") {
  // Very large input data
  std::vector<std::byte> large_data(static_cast<size_t>(1024 * 1024));  // 1MB
  fill_random(large_data.data(), large_data.size(), 42);

  CacheKey key{std::span<const std::byte>(large_data)};

  // Should work - SHA-256 handles any size input
  REQUIRE_FALSE(key.is_zero());
  REQUIRE(key.to_hex().length() == 64);
}
