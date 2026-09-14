// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/document.hpp"
#include "core/volume.hpp"
#include "cyclone/cache.hpp"
#include "cyclone/key.hpp"

using namespace cyclone;

namespace {

std::string create_temp_file(size_t size_mb = 10, bool multi_process = false,
                             size_t stripe_size = 0) {
  static std::atomic<int> counter{0};
  const std::string base =
      (std::filesystem::temp_directory_path() /
       ("cyclone_version_test_" + std::to_string(counter++) + ".dat"))
          .string();
  // Resolve the SAME structural-fingerprint filename the cache will open (see
  // fingerprint_cache_path), so the raw header inspection / corruption below
  // targets the file the cache actually uses.  fingerprint_cache_path is
  // idempotent, so the later add_volume() keeps this exact name.
  const std::string path = fingerprint_cache_path(base, size_mb * 1024 * 1024,
                                                  stripe_size, multi_process);
  std::remove(path.c_str());

  FILE *f = std::fopen(path.c_str(), "wb");
  if (f != nullptr) {
    std::fseek(f, size_mb * 1024 * 1024 - 1, SEEK_SET);
    std::fputc(0, f);
    std::fclose(f);
  }
  return path;
}

void cleanup_temp_file(const std::string &path) { std::remove(path.c_str()); }

// Write raw bytes to a file at given offset
bool write_bytes_at(const std::string &path, size_t offset, const void *data,
                    size_t len) {
  FILE *f = std::fopen(path.c_str(), "r+b");
  if (f == nullptr) {
    return false;
  }
  std::fseek(f, offset, SEEK_SET);
  size_t written = std::fwrite(data, 1, len, f);
  std::fclose(f);
  return written == len;
}

}  // namespace

// =============================================================================
// VolumeHeader Tests
// =============================================================================

TEST_CASE("VolumeHeader constants are correct", "[version]") {
  REQUIRE(VolumeHeader::kMagic == 0x43594C4E);
  REQUIRE(VolumeHeader::kSize == 64);
  // Deliberate tripwire (see the twin assertion on Document::kVersionMajor):
  // Version 7 leaves pre-depth-bound alternate chains behind, and the two
  // constants move in lockstep.
  REQUIRE(VolumeHeader::kFormatVersionMajor == 7);
  REQUIRE(VolumeHeader::kFormatVersionMinor == 0);
}

TEST_CASE("VolumeHeader serialize/deserialize round-trip", "[version]") {
  VolumeHeader original;
  original.magic = VolumeHeader::kMagic;
  original.format_version_major = 2;
  original.format_version_minor = 1;
  original.creation_time = 1234567890;
  original.volume_size = static_cast<uint64_t>(1024 * 1024 * 100);

  std::byte buffer[VolumeHeader::kSize];
  original.serialize(buffer);

  VolumeHeader restored = VolumeHeader::deserialize(buffer);

  REQUIRE(restored.magic == original.magic);
  REQUIRE(restored.format_version_major == original.format_version_major);
  REQUIRE(restored.format_version_minor == original.format_version_minor);
  REQUIRE(restored.creation_time == original.creation_time);
  REQUIRE(restored.volume_size == original.volume_size);
}

TEST_CASE("VolumeHeader is_valid checks magic", "[version]") {
  VolumeHeader header;

  header.magic = VolumeHeader::kMagic;
  REQUIRE(header.is_valid());

  header.magic = 0;
  REQUIRE_FALSE(header.is_valid());

  header.magic = 0xDEADBEEF;
  REQUIRE_FALSE(header.is_valid());
}

TEST_CASE("VolumeHeader is_compatible checks version", "[version]") {
  VolumeHeader header;
  header.magic = VolumeHeader::kMagic;

  // Same major version is compatible
  header.format_version_major = VolumeHeader::kFormatVersionMajor;
  header.format_version_minor = 0;
  REQUIRE(header.is_compatible());

  // Different minor version with same major is compatible
  header.format_version_minor = 99;
  REQUIRE(header.is_compatible());

  // Different major version is incompatible
  header.format_version_major = VolumeHeader::kFormatVersionMajor + 1;
  REQUIRE_FALSE(header.is_compatible());

  header.format_version_major = VolumeHeader::kFormatVersionMajor - 1;
  REQUIRE_FALSE(header.is_compatible());

  // Invalid magic makes it incompatible regardless of version
  header.magic = 0;
  header.format_version_major = VolumeHeader::kFormatVersionMajor;
  REQUIRE_FALSE(header.is_compatible());
}

// =============================================================================
// Cache Version Compatibility Tests
// =============================================================================

TEST_CASE("Fresh cache creates valid header", "[version][integration]") {
  std::string cache_path = create_temp_file(10);

  {
    CacheConfig config;
    auto cache = Cache::create(config);
    REQUIRE(cache.has_value());

    VolumeConfig vol_config;
    vol_config.path = cache_path;
    vol_config.size = static_cast<size_t>(10 * 1024 * 1024);

    REQUIRE((*cache)->add_volume(vol_config).has_value());
    REQUIRE((*cache)->start().has_value());

    // Write something to ensure cache is active
    CacheKey key("test-key");
    std::string content = "test content";
    std::vector<std::byte> data(content.size());
    std::memcpy(data.data(), content.data(), content.size());

    auto wh = (*cache)->write_sync(key, data.size());
    REQUIRE(wh.has_value());
    wh->write_sync(std::span<const std::byte>(data));
    wh->close_sync();

    (*cache)->stop();
  }

  // Verify header was written
  FILE *f = std::fopen(cache_path.c_str(), "rb");
  REQUIRE(f != nullptr);

  std::byte buffer[VolumeHeader::kSize];
  size_t read = std::fread(buffer, 1, VolumeHeader::kSize, f);
  std::fclose(f);

  REQUIRE(read == VolumeHeader::kSize);

  VolumeHeader header = VolumeHeader::deserialize(buffer);
  REQUIRE(header.is_valid());
  REQUIRE(header.is_compatible());
  REQUIRE(header.format_version_major == VolumeHeader::kFormatVersionMajor);
  REQUIRE(header.volume_size == static_cast<uint64_t>(10 * 1024 * 1024));

  cleanup_temp_file(cache_path);
}

TEST_CASE("Compatible cache preserves data", "[version][integration]") {
  std::string cache_path = create_temp_file(10);
  CacheKey key("persistent-key");
  std::string expected_content = "This should persist across restarts";

  // Write data
  {
    CacheConfig config;
    auto cache = Cache::create(config);
    REQUIRE(cache.has_value());

    VolumeConfig vol_config;
    vol_config.path = cache_path;
    vol_config.size = static_cast<size_t>(10 * 1024 * 1024);

    REQUIRE((*cache)->add_volume(vol_config).has_value());
    REQUIRE((*cache)->start().has_value());

    std::vector<std::byte> data(expected_content.size());
    std::memcpy(data.data(), expected_content.data(), expected_content.size());

    auto wh = (*cache)->write_sync(key, data.size());
    REQUIRE(wh.has_value());
    wh->write_sync(std::span<const std::byte>(data));
    REQUIRE(wh->close_sync().has_value());

    (*cache)->stop();
  }

  // Reopen and verify data exists
  {
    CacheConfig config;
    auto cache = Cache::create(config);
    REQUIRE(cache.has_value());

    VolumeConfig vol_config;
    vol_config.path = cache_path;
    vol_config.size = static_cast<size_t>(10 * 1024 * 1024);

    REQUIRE((*cache)->add_volume(vol_config).has_value());
    REQUIRE((*cache)->start().has_value());

    // Note: Directory is in-memory and not persisted, so data won't be findable
    // This test verifies that the cache starts successfully with compatible
    // header
    REQUIRE((*cache)->is_running());

    (*cache)->stop();
  }

  cleanup_temp_file(cache_path);
}

TEST_CASE("Incompatible major version triggers reset with auto_reset=true",
          "[version][integration]") {
  std::string cache_path = create_temp_file(10);

  // Create cache with current version
  {
    CacheConfig config;
    auto cache = Cache::create(config);
    REQUIRE(cache.has_value());

    VolumeConfig vol_config;
    vol_config.path = cache_path;
    vol_config.size = static_cast<size_t>(10 * 1024 * 1024);

    REQUIRE((*cache)->add_volume(vol_config).has_value());
    REQUIRE((*cache)->start().has_value());
    (*cache)->stop();
  }

  // Corrupt header to have different major version
  VolumeHeader old_header;
  old_header.magic = VolumeHeader::kMagic;
  old_header.format_version_major =
      VolumeHeader::kFormatVersionMajor + 1;  // Future version
  old_header.format_version_minor = 0;
  old_header.creation_time = 12345;
  old_header.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);

  std::byte buffer[VolumeHeader::kSize];
  old_header.serialize(buffer);
  REQUIRE(write_bytes_at(cache_path, 0, buffer, VolumeHeader::kSize));

  // Reopen - should reset automatically
  {
    CacheConfig config;
    auto cache = Cache::create(config);
    REQUIRE(cache.has_value());

    VolumeConfig vol_config;
    vol_config.path = cache_path;
    vol_config.size = static_cast<size_t>(10 * 1024 * 1024);
    vol_config.auto_reset_on_incompatible = true;

    REQUIRE((*cache)->add_volume(vol_config).has_value());
    REQUIRE((*cache)->start().has_value());  // Should succeed after reset
    REQUIRE((*cache)->is_running());

    (*cache)->stop();
  }

  // Verify header was updated to current version
  FILE *f = std::fopen(cache_path.c_str(), "rb");
  REQUIRE(f != nullptr);
  size_t read = std::fread(buffer, 1, VolumeHeader::kSize, f);
  std::fclose(f);
  REQUIRE(read == VolumeHeader::kSize);

  VolumeHeader new_header = VolumeHeader::deserialize(buffer);
  REQUIRE(new_header.is_valid());
  REQUIRE(new_header.is_compatible());
  REQUIRE(new_header.format_version_major == VolumeHeader::kFormatVersionMajor);

  cleanup_temp_file(cache_path);
}

TEST_CASE("Incompatible version returns error with auto_reset=false",
          "[version][integration]") {
  std::string cache_path = create_temp_file(10);

  // Create cache with current version
  {
    CacheConfig config;
    auto cache = Cache::create(config);
    REQUIRE(cache.has_value());

    VolumeConfig vol_config;
    vol_config.path = cache_path;
    vol_config.size = static_cast<size_t>(10 * 1024 * 1024);

    REQUIRE((*cache)->add_volume(vol_config).has_value());
    REQUIRE((*cache)->start().has_value());
    (*cache)->stop();
  }

  // Corrupt header to have different major version
  VolumeHeader old_header;
  old_header.magic = VolumeHeader::kMagic;
  old_header.format_version_major = 1;  // Old version
  old_header.format_version_minor = 0;
  old_header.creation_time = 12345;
  old_header.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);

  std::byte buffer[VolumeHeader::kSize];
  old_header.serialize(buffer);
  REQUIRE(write_bytes_at(cache_path, 0, buffer, VolumeHeader::kSize));

  // Reopen with auto_reset=false - should fail
  {
    CacheConfig config;
    auto cache = Cache::create(config);
    REQUIRE(cache.has_value());

    VolumeConfig vol_config;
    vol_config.path = cache_path;
    vol_config.size = static_cast<size_t>(10 * 1024 * 1024);
    vol_config.auto_reset_on_incompatible = false;

    REQUIRE((*cache)->add_volume(vol_config).has_value());

    auto start_result = (*cache)->start();
    REQUIRE_FALSE(start_result.has_value());
    REQUIRE(start_result.error() == CacheError::IncompatibleVersion);
  }

  cleanup_temp_file(cache_path);
}

TEST_CASE("Missing/invalid magic triggers reset", "[version][integration]") {
  std::string cache_path = create_temp_file(10);

  // Write garbage to header area
  uint32_t garbage = 0xDEADBEEF;
  REQUIRE(write_bytes_at(cache_path, 0, &garbage, sizeof(garbage)));

  // Open cache - should reset and work
  {
    CacheConfig config;
    auto cache = Cache::create(config);
    REQUIRE(cache.has_value());

    VolumeConfig vol_config;
    vol_config.path = cache_path;
    vol_config.size = static_cast<size_t>(10 * 1024 * 1024);

    REQUIRE((*cache)->add_volume(vol_config).has_value());
    REQUIRE((*cache)->start().has_value());
    REQUIRE((*cache)->is_running());

    (*cache)->stop();
  }

  // Verify valid header now exists
  FILE *f = std::fopen(cache_path.c_str(), "rb");
  REQUIRE(f != nullptr);
  std::byte buffer[VolumeHeader::kSize];
  std::fread(buffer, 1, VolumeHeader::kSize, f);
  std::fclose(f);

  VolumeHeader header = VolumeHeader::deserialize(buffer);
  REQUIRE(header.is_valid());
  REQUIRE(header.is_compatible());

  cleanup_temp_file(cache_path);
}

TEST_CASE("Zero-filled file triggers reset", "[version][integration]") {
  std::string cache_path = create_temp_file(10);
  // File is already zero-filled by create_temp_file

  {
    CacheConfig config;
    auto cache = Cache::create(config);
    REQUIRE(cache.has_value());

    VolumeConfig vol_config;
    vol_config.path = cache_path;
    vol_config.size = static_cast<size_t>(10 * 1024 * 1024);

    REQUIRE((*cache)->add_volume(vol_config).has_value());
    REQUIRE((*cache)->start().has_value());
    REQUIRE((*cache)->is_running());

    (*cache)->stop();
  }

  cleanup_temp_file(cache_path);
}

// =============================================================================
// Document Version Compatibility Tests
// =============================================================================

TEST_CASE("Document version compatibility check", "[version][document]") {
  Document doc;
  doc.magic = Document::kMagic;

  // Current version is compatible
  doc.version_major = Document::kVersionMajor;
  doc.version_minor = Document::kVersionMinor;
  REQUIRE(doc.is_version_compatible());
  REQUIRE(doc.is_valid_and_compatible());

  // Different minor version with same major is compatible
  doc.version_minor = 99;
  REQUIRE(doc.is_version_compatible());

  // Different major version is incompatible
  doc.version_major = Document::kVersionMajor + 1;
  REQUIRE_FALSE(doc.is_version_compatible());
  REQUIRE_FALSE(doc.is_valid_and_compatible());

  doc.version_major = Document::kVersionMajor - 1;
  REQUIRE_FALSE(doc.is_version_compatible());

  // Invalid magic with correct version - is_valid_and_compatible returns false
  doc.magic = 0;
  doc.version_major = Document::kVersionMajor;
  REQUIRE_FALSE(doc.is_valid_and_compatible());
}

TEST_CASE("Document version constants match volume header", "[version]") {
  // Document and VolumeHeader should use the same major version
  REQUIRE(Document::kVersionMajor == VolumeHeader::kFormatVersionMajor);
}

// =============================================================================
// The READ path must gate on the document's format version
// =============================================================================
// Volume::open() REFUSES to open under a live peer running an incompatible
// format (CacheError::ResetRefusedLivePeer; the coexist machinery was removed)
// -- resetting would wipe the cache out from under draining nginx/Apache
// workers.  A new-format binary can nonetheless find itself reading old-format
// documents: when the reset gate is degraded (a lock-less filesystem), or
// transiently before an owed reset has run (see the DocumentReader rationale
// in src/core/document.cpp).
//
// DocumentReader used to accept a document on its MAGIC ALONE
// (is_valid()).  Magic is version-independent, so an old-format record would be
// MISPARSED through the new layout and served as if it were ours -- silently
// wrong bytes.  is_valid_and_compatible() is the gate; a foreign version must
// read as a MISS.  This test pins that defense-in-depth.
//
// Non-vacuity, both load-bearing:
//   * MULTI-PROCESS (mmap directory).  A single-process volume rebuilds an
//     empty in-memory Directory on every open, so nothing would survive the
//     reopen and every key would miss for the wrong reason.
//   * ram_cache_size = 0.  A RAM hit would never touch the document at all.
// The reopened volume is asserted to still hold all N directory entries BEFORE
// the reads, so a miss can only come from the version gate.
TEST_CASE("Old-format documents miss instead of being misparsed",
          "[version][document][integration]") {
  const std::string path =
      create_temp_file(64, /*multi_process=*/true);  // 64MB
  constexpr int kNumKeys = 40;
  const std::vector<std::byte> data(256, std::byte{0x5A});

  auto make_cache = [&]() {
    CacheConfig config;
    config.set_multi_process(0, 1);  // persistent (mmap) directory
    config.set_ram_cache_size(0);    // or a RAM hit masks the disk read
    return config;
  };

  // Phase 1: write documents with the CURRENT format version.
  {
    auto cache = Cache::create(make_cache());
    REQUIRE(cache.has_value());
    (*cache)->add_volume(path, static_cast<size_t>(64) * 1024 * 1024);
    REQUIRE((*cache)->start().has_value());

    for (int i = 0; i < kNumKeys; ++i) {
      CacheKey k("oldfmt-key-" + std::to_string(i));
      auto wh = (*cache)->write_sync(k, data.size());
      REQUIRE(wh.has_value());
      REQUIRE(wh->write_sync(data).has_value());
      REQUIRE(wh->close_sync().has_value());
    }
    REQUIRE((*cache)->stats().current_entries == kNumKeys);
    (*cache)->stop();
  }

  // Phase 2: age every document on disk to a FOREIGN major version, exactly as
  // a peer running an older format would have written them.  Scan for document
  // headers (magic + a current version_major at the named wire offset) and bump
  // the version byte.  The document checksum covers only the bytes AFTER the
  // 132-byte header, so this does not disturb it -- which is the whole point:
  // the record stays perfectly well-formed and CRC-clean, and ONLY the version
  // says it is not ours.  Nothing but a version check can reject it.
  size_t aged = 0;
  {
    std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
    REQUIRE(f.is_open());
    f.seekg(0, std::ios::end);
    const auto file_size = static_cast<size_t>(f.tellg());
    f.seekg(0, std::ios::beg);

    std::vector<char> buf(file_size);
    f.read(buf.data(), static_cast<std::streamsize>(file_size));
    REQUIRE(static_cast<size_t>(f.gcount()) == file_size);

    const uint32_t magic = Document::kMagic;
    for (size_t off = 0; off + Document::kHeaderSize <= file_size; ++off) {
      if (std::memcmp(buf.data() + off, &magic, sizeof(magic)) != 0) {
        continue;
      }
      char *ver = buf.data() + off + Document::kVersionMajorOffset;
      if (static_cast<uint8_t>(*ver) != Document::kVersionMajor) {
        continue;  // not a document header we wrote
      }
      const auto foreign = static_cast<char>(Document::kVersionMajor + 1);
      f.clear();
      f.seekp(static_cast<std::streamoff>(off + Document::kVersionMajorOffset));
      f.write(&foreign, 1);
      ++aged;
    }
    f.close();
  }
  INFO("document headers aged to a foreign version: " << aged);
  REQUIRE(aged >= kNumKeys);  // at least one header per document

  // Phase 3: reopen with the SAME config.  The VOLUME header is untouched, so
  // there is no reset: the directory still points at all N documents.  Every
  // read must MISS -- never return a valid-looking document.
  {
    auto cache = Cache::create(make_cache());
    REQUIRE(cache.has_value());
    (*cache)->add_volume(path, static_cast<size_t>(64) * 1024 * 1024);
    REQUIRE((*cache)->start().has_value());

    // The volume was NOT wiped: the entries are all still there, so a miss
    // below can only be the version gate rejecting the document.
    INFO("the directory must still hold the entries, or this test is vacuous");
    REQUIRE((*cache)->stats().current_entries == kNumKeys);

    int served = 0;
    for (int i = 0; i < kNumKeys; ++i) {
      CacheKey k("oldfmt-key-" + std::to_string(i));
      auto result = (*cache)->read_sync(k);
      if (result.has_value()) {
        ++served;  // a foreign-format record was served as if it were ours
      }
    }
    INFO("old-format documents served instead of missed: " << served << " / "
                                                           << kNumKeys);
    CHECK(served == 0);

    (*cache)->stop();
  }

  cleanup_temp_file(path);
}

// =============================================================================
// Edge Cases
// =============================================================================

TEST_CASE("Truncated header file triggers reset", "[version][edge]") {
  std::string cache_path =
      (std::filesystem::temp_directory_path() / "truncated_header_test.dat")
          .string();

  // Create a file smaller than header size
  {
    FILE *f = std::fopen(cache_path.c_str(), "wb");
    REQUIRE(f != nullptr);
    std::byte small_data[32] = {};
    std::fwrite(small_data, 1, sizeof(small_data), f);
    std::fclose(f);
  }

  // Open should resize and reset
  {
    CacheConfig config;
    auto cache = Cache::create(config);
    REQUIRE(cache.has_value());

    VolumeConfig vol_config;
    vol_config.path = cache_path;
    vol_config.size = static_cast<size_t>(10 * 1024 * 1024);

    REQUIRE((*cache)->add_volume(vol_config).has_value());
    REQUIRE((*cache)->start().has_value());
    REQUIRE((*cache)->is_running());

    (*cache)->stop();
  }

  cleanup_temp_file(cache_path);
}

TEST_CASE("Version 0.x is treated as incompatible", "[version][edge]") {
  std::string cache_path = create_temp_file(10);

  // Write header with version 0.x
  VolumeHeader old_header;
  old_header.magic = VolumeHeader::kMagic;
  old_header.format_version_major = 0;
  old_header.format_version_minor = 1;
  old_header.creation_time = 12345;
  old_header.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);

  std::byte buffer[VolumeHeader::kSize];
  old_header.serialize(buffer);
  REQUIRE(write_bytes_at(cache_path, 0, buffer, VolumeHeader::kSize));

  // Should reset
  {
    CacheConfig config;
    auto cache = Cache::create(config);
    REQUIRE(cache.has_value());

    VolumeConfig vol_config;
    vol_config.path = cache_path;
    vol_config.size = static_cast<size_t>(10 * 1024 * 1024);

    REQUIRE((*cache)->add_volume(vol_config).has_value());
    REQUIRE((*cache)->start().has_value());

    (*cache)->stop();
  }

  // Verify reset to current version
  FILE *f = std::fopen(cache_path.c_str(), "rb");
  std::fread(buffer, 1, VolumeHeader::kSize, f);
  std::fclose(f);

  VolumeHeader new_header = VolumeHeader::deserialize(buffer);
  REQUIRE(new_header.format_version_major == VolumeHeader::kFormatVersionMajor);

  cleanup_temp_file(cache_path);
}

// =============================================================================
// Phase 5A: VolumeHeader Serialize/Deserialize Tests
// =============================================================================

TEST_CASE("VolumeHeader serialize deserialize roundtrip", "[version]") {
  VolumeHeader original;
  original.magic = VolumeHeader::kMagic;
  original.format_version_major = 5;
  original.format_version_minor = 42;
  original.creation_time = 1700000000;
  original.volume_size = 256ULL * 1024 * 1024;
  original.directory_buckets = 8192;
  original.mmap_directory = 1;

  std::byte buffer[VolumeHeader::kSize];
  original.serialize(buffer);

  VolumeHeader restored = VolumeHeader::deserialize(buffer);

  REQUIRE(restored.magic == original.magic);
  REQUIRE(restored.format_version_major == original.format_version_major);
  REQUIRE(restored.format_version_minor == original.format_version_minor);
  REQUIRE(restored.creation_time == original.creation_time);
  REQUIRE(restored.volume_size == original.volume_size);
  REQUIRE(restored.directory_buckets == original.directory_buckets);
  REQUIRE(restored.mmap_directory == original.mmap_directory);
}

TEST_CASE("VolumeHeader deserialize from truncated buffer",
          "[version][security]") {
  // Serialize a valid header first
  VolumeHeader original;
  original.magic = VolumeHeader::kMagic;
  original.format_version_major = VolumeHeader::kFormatVersionMajor;
  original.format_version_minor = VolumeHeader::kFormatVersionMinor;
  original.creation_time = 1234567890;
  original.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);

  std::byte full_buffer[VolumeHeader::kSize];
  original.serialize(full_buffer);

  // VolumeHeader::deserialize() reads from a raw pointer; the caller
  // (Volume::read_header) is responsible for ensuring the buffer is at least
  // kSize bytes. We verify that the Volume-level check rejects truncated reads
  // by writing a truncated file and attempting to open it.
  std::string cache_path = create_temp_file(10);

  // Write only a partial header (less than kSize bytes)
  {
    FILE *f = std::fopen(cache_path.c_str(), "r+b");
    REQUIRE(f != nullptr);
    // Write only 20 bytes of the header (less than kSize=64)
    std::fwrite(full_buffer, 1, 20, f);
    // Zero out the rest of the header area
    std::byte zeros[VolumeHeader::kSize - 20] = {};
    std::fwrite(zeros, 1, sizeof(zeros), f);
    std::fclose(f);
  }

  // The file has garbage in the header area (partial valid header + zeros).
  // Opening should trigger a reset (the deserialized header will have invalid
  // fields).
  {
    CacheConfig config;
    auto cache = Cache::create(config);
    REQUIRE(cache.has_value());

    VolumeConfig vol_config;
    vol_config.path = cache_path;
    vol_config.size = static_cast<size_t>(10 * 1024 * 1024);

    REQUIRE((*cache)->add_volume(vol_config).has_value());
    // Should succeed by resetting (auto_reset_on_incompatible defaults to true)
    REQUIRE((*cache)->start().has_value());
    (*cache)->stop();
  }

  cleanup_temp_file(cache_path);
}

TEST_CASE("VolumeHeader invalid magic number", "[version][security]") {
  // Serialize a valid header
  VolumeHeader original;
  original.magic = VolumeHeader::kMagic;
  original.format_version_major = VolumeHeader::kFormatVersionMajor;
  original.format_version_minor = VolumeHeader::kFormatVersionMinor;
  original.creation_time = 1234567890;
  original.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);

  std::byte buffer[VolumeHeader::kSize];
  original.serialize(buffer);

  // Corrupt the first 4 bytes (magic)
  uint32_t bad_magic = 0xBADCAFE0;
  std::memcpy(buffer, &bad_magic, sizeof(bad_magic));

  // Deserialize and verify is_valid() returns false
  VolumeHeader corrupted = VolumeHeader::deserialize(buffer);
  REQUIRE_FALSE(corrupted.is_valid());

  // Other fields should still have been deserialized
  REQUIRE(corrupted.format_version_major == original.format_version_major);
  REQUIRE(corrupted.creation_time == original.creation_time);
  REQUIRE(corrupted.volume_size == original.volume_size);
}
