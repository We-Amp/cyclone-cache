// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include <catch2/catch_test_macros.hpp>

#include "../../src/core/document.hpp"
#include "../../src/core/volume.hpp"

using namespace cyclone;

TEST_CASE("Document magic number", "[document]") {
  Document doc;
  REQUIRE(doc.magic == Document::kMagic);
  REQUIRE(doc.is_valid());
}

TEST_CASE("Document serialization round-trip", "[document]") {
  CacheKey key("test-key");
  std::vector<std::byte> header = {std::byte{0x01}, std::byte{0x02},
                                   std::byte{0x03}};
  std::vector<std::byte> content(100);
  for (size_t i = 0; i < content.size(); ++i) {
    content[i] = std::byte(i & 0xFF);
  }

  auto data = DocumentBuilder()
                  .set_key(key)
                  .set_header(header)
                  .set_content(content)
                  .set_type(Document::Type::SingleFrag)
                  .build();

  REQUIRE(data.size() >=
          Document::kHeaderSize + header.size() + content.size());

  DocumentReader reader(data);
  REQUIRE(reader.is_valid());
  REQUIRE(reader.document().doc_type == Document::Type::SingleFrag);
  REQUIRE(reader.header().size() == header.size());
  REQUIRE(reader.content().size() == content.size());
}

TEST_CASE("Document checksum", "[document]") {
  std::vector<std::byte> data = {std::byte{0x00}, std::byte{0x11},
                                 std::byte{0x22}, std::byte{0x33}};

  uint32_t checksum1 = Document::compute_checksum(data);
  uint32_t checksum2 = Document::compute_checksum(data);

  REQUIRE(checksum1 == checksum2);
  REQUIRE(checksum1 != 0);

  data[0] = std::byte{0xFF};
  uint32_t checksum3 = Document::compute_checksum(data);
  REQUIRE(checksum1 != checksum3);
}

TEST_CASE("DocumentBuilder with different types", "[document]") {
  CacheKey key("test");

  auto single = DocumentBuilder()
                    .set_key(key)
                    .set_type(Document::Type::SingleFrag)
                    .enable_checksum(false)
                    .build();

  DocumentReader single_reader(single);
  REQUIRE(single_reader.document().is_complete());
  REQUIRE(single_reader.document().is_first());

  auto first = DocumentBuilder()
                   .set_key(key)
                   .set_type(Document::Type::FirstFrag)
                   .enable_checksum(false)
                   .build();

  DocumentReader first_reader(first);
  REQUIRE_FALSE(first_reader.document().is_complete());
  REQUIRE(first_reader.document().is_first());

  auto last = DocumentBuilder()
                  .set_key(key)
                  .set_type(Document::Type::LastFrag)
                  .enable_checksum(false)
                  .build();

  DocumentReader last_reader(last);
  REQUIRE(last_reader.document().is_complete());
  REQUIRE_FALSE(last_reader.document().is_first());
}

TEST_CASE("Document header size is 132 bytes", "[document][alternate]") {
  REQUIRE(Document::kHeaderSize == 132);
  // Deliberate tripwire: a version bump is a cross-release decision (cold
  // cache for every consumer), never an incidental edit.  Version 7 leaves
  // pre-depth-bound alternate chains behind; it must stay in lockstep with
  // VolumeHeader::kFormatVersionMajor.
  REQUIRE(Document::kVersionMajor == 7);
}

TEST_CASE("Document alternate chain fields default values",
          "[document][alternate]") {
  Document doc;
  REQUIRE(doc.alternate_id == 0);
  REQUIRE(doc.next_alternate_offset == 0);
  REQUIRE(doc.hit_count == 0);
  REQUIRE(doc.last_access == 0);
}

TEST_CASE("Document alternate chain fields round-trip",
          "[document][alternate]") {
  CacheKey key("test-alternate");

  constexpr uint8_t test_alternate_id = 5;
  constexpr uint64_t test_next_offset = 0x123456789ABCULL;
  constexpr uint32_t test_hit_count = 12345;
  constexpr int64_t test_last_access = 1700000000000LL;

  auto data = DocumentBuilder()
                  .set_key(key)
                  .set_type(Document::Type::SingleFrag)
                  .set_alternate_id(test_alternate_id)
                  .set_next_alternate_offset(test_next_offset)
                  .set_hit_count(test_hit_count)
                  .set_last_access(test_last_access)
                  .enable_checksum(false)
                  .build();

  REQUIRE(data.size() >= Document::kHeaderSize);

  DocumentReader reader(data);
  REQUIRE(reader.is_valid());

  const Document &doc = reader.document();
  REQUIRE(doc.alternate_id == test_alternate_id);
  REQUIRE(doc.next_alternate_offset == test_next_offset);
  REQUIRE(doc.hit_count == test_hit_count);
  REQUIRE(doc.last_access == test_last_access);
}

TEST_CASE("Document max alternates constant", "[document][alternate]") {
  REQUIRE(Document::kMaxAlternates == 64);
}

TEST_CASE("DocumentBuilder overflow guard", "[document][security]") {
  CacheKey key("overflow-test");

  SECTION("Normal build succeeds") {
    std::vector<std::byte> header(100, std::byte{0x01});
    std::vector<std::byte> content(200, std::byte{0x02});

    auto data = DocumentBuilder()
                    .set_key(key)
                    .set_header(header)
                    .set_content(content)
                    .set_type(Document::Type::SingleFrag)
                    .build();

    REQUIRE(!data.empty());
    REQUIRE(data.size() == Document::kHeaderSize + 100 + 200);
  }

  SECTION("Empty build returns valid minimal document") {
    auto data = DocumentBuilder()
                    .set_key(key)
                    .set_type(Document::Type::SingleFrag)
                    .build();

    REQUIRE(!data.empty());
    REQUIRE(data.size() == Document::kHeaderSize);
  }
}

// =============================================================================
// Binary layout tests — lock down the on-disk format
// =============================================================================

TEST_CASE("Document serialize/deserialize preserves all fields exactly",
          "[document][layout]") {
  Document doc;
  doc.magic = Document::kMagic;
  doc.len = 0x12345678;
  doc.total_len = 0xAABBCCDDEEFF0011ULL;
  for (size_t i = 0; i < doc.first_key.size(); ++i)
    doc.first_key[i] = std::byte(i);
  for (size_t i = 0; i < doc.fragment_key.size(); ++i)
    doc.fragment_key[i] = std::byte(0xFF - i);
  doc.header_len = 42;
  doc.doc_type = Document::Type::SingleFrag;
  doc.version_major = 3;
  doc.version_minor = 7;
  doc.flags = Document::kFlagCompressed | Document::kFlagPinned;
  doc.sync_serial = 0x11223344;
  doc.write_serial = 0x55667788;
  doc.pin_until = 0xDEADBEEF;
  doc.checksum = 0xCAFEBABE;
  doc.frag_offset = 0xF00D;
  doc.hit_count = 999;
  doc.next_alternate_offset = 0x1122334455667788ULL;
  doc.alternate_id = 5;
  doc.reserved2[0] = 0xAA;
  doc.reserved2[1] = 0xBB;
  doc.reserved2[2] = 0xCC;
  doc.last_access = -12345;

  std::vector<std::byte> buffer;
  doc.serialize(buffer);
  REQUIRE(buffer.size() == Document::kHeaderSize);

  Document restored = Document::deserialize(std::span<const std::byte>(buffer));

  REQUIRE(restored.magic == doc.magic);
  REQUIRE(restored.len == doc.len);
  REQUIRE(restored.total_len == doc.total_len);
  REQUIRE(restored.first_key == doc.first_key);
  REQUIRE(restored.fragment_key == doc.fragment_key);
  REQUIRE(restored.header_len == doc.header_len);
  REQUIRE(restored.doc_type == doc.doc_type);
  REQUIRE(restored.version_major == doc.version_major);
  REQUIRE(restored.version_minor == doc.version_minor);
  REQUIRE(restored.flags == doc.flags);
  REQUIRE(restored.sync_serial == doc.sync_serial);
  REQUIRE(restored.write_serial == doc.write_serial);
  REQUIRE(restored.pin_until == doc.pin_until);
  REQUIRE(restored.checksum == doc.checksum);
  REQUIRE(restored.frag_offset == doc.frag_offset);
  REQUIRE(restored.hit_count == doc.hit_count);
  REQUIRE(restored.next_alternate_offset == doc.next_alternate_offset);
  REQUIRE(restored.alternate_id == doc.alternate_id);
  REQUIRE(restored.reserved2[0] == doc.reserved2[0]);
  REQUIRE(restored.reserved2[1] == doc.reserved2[1]);
  REQUIRE(restored.reserved2[2] == doc.reserved2[2]);
  REQUIRE(restored.last_access == doc.last_access);
}

TEST_CASE("Document binary layout matches expected offsets",
          "[document][layout]") {
  // Verify that known field offsets produce the right bytes.
  // This catches any accidental reordering or padding changes.
  Document doc;
  doc.magic = Document::kMagic;
  doc.len = 0;
  doc.hit_count = 0x42424242;
  doc.last_access = 0x0102030405060708LL;

  std::vector<std::byte> buffer;
  doc.serialize(buffer);

  // magic at offset 0
  uint32_t magic_read;
  std::memcpy(&magic_read, buffer.data(), 4);
  REQUIRE(magic_read == Document::kMagic);

  // hit_count at kHitCountOffset (108)
  uint32_t hit_read;
  std::memcpy(&hit_read, buffer.data() + Document::kHitCountOffset, 4);
  REQUIRE(hit_read == 0x42424242);

  // checksum at kChecksumOffset (100)
  uint32_t cksum_read;
  std::memcpy(&cksum_read, buffer.data() + Document::kChecksumOffset, 4);
  REQUIRE(cksum_read == doc.checksum);

  // last_access at kLastAccessOffset (120 in v6)
  int64_t access_read;
  std::memcpy(&access_read, buffer.data() + Document::kLastAccessOffset, 8);
  REQUIRE(access_read == 0x0102030405060708LL);
}

TEST_CASE("VolumeHeader serialize/deserialize preserves all fields",
          "[document][layout]") {
  VolumeHeader hdr;
  hdr.magic = VolumeHeader::kMagic;
  hdr.format_version_major = 3;
  hdr.format_version_minor = 1;
  hdr.creation_time = 0x1122334455667788ULL;
  hdr.volume_size = 0xAABBCCDDEEFF0011ULL;
  hdr.directory_buckets = 16384;
  hdr.mmap_directory = 1;

  std::byte buffer[VolumeHeader::kSize];
  hdr.serialize(buffer);

  VolumeHeader restored = VolumeHeader::deserialize(buffer);

  REQUIRE(restored.magic == hdr.magic);
  REQUIRE(restored.format_version_major == hdr.format_version_major);
  REQUIRE(restored.format_version_minor == hdr.format_version_minor);
  REQUIRE(restored.creation_time == hdr.creation_time);
  REQUIRE(restored.volume_size == hdr.volume_size);
  REQUIRE(restored.directory_buckets == hdr.directory_buckets);
  REQUIRE(restored.mmap_directory == hdr.mmap_directory);
}

TEST_CASE("Document alternate fields with zero next offset",
          "[document][alternate]") {
  CacheKey key("test-no-chain");

  auto data = DocumentBuilder()
                  .set_key(key)
                  .set_type(Document::Type::SingleFrag)
                  .set_alternate_id(1)
                  .set_next_alternate_offset(0)
                  .enable_checksum(false)
                  .build();

  DocumentReader reader(data);
  REQUIRE(reader.is_valid());
  REQUIRE(reader.document().next_alternate_offset == 0);
}
