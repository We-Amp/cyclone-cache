// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "cyclone/alternate.hpp"
#include "cyclone/key.hpp"

namespace cyclone {

// Document header format (132-byte fixed header, followed by variable data)
struct Document {
  static constexpr uint32_t kMagic = 0x5F129B14;
  static constexpr uint8_t kVersionMajor =
      7;  // Version 6: hit_count/next_alternate_offset/last_access laid out
          // naturally aligned so the in-place header RMW sites can store them
          // via std::atomic_ref (kept in lockstep with
          // VolumeHeader::kFormatVersionMajor; pre-v6 volumes auto-reset)
          // Version 7: alternate chains are depth-bounded at write time; the
          // bump exists to leave pre-bound chains (over-deep, possibly cyclic)
          // behind rather than repair them.  Lockstep with
          // VolumeHeader::kFormatVersionMajor is not ceremony: DocumentReader
          // gates on this field, so any stray v6 document a v7 binary reaches
          // reads as a MISS instead of a misparse.
  static constexpr uint8_t kVersionMinor = 0;

  // Maximum alternates per cache key (bounds chain traversal).  Alias of the
  // public kMaxAlternatesPerKey: the declared API constant is the
  // single source of truth, so the bound documented for consumers is
  // provably the one enforced at the write path.
  static constexpr uint8_t kMaxAlternates = kMaxAlternatesPerKey;

  // Maximum chain nodes to traverse before giving up.  With duplicate
  // alternate IDs (e.g. concurrent nginx writes) the physical chain can
  // be longer than kMaxAlternates, so we allow 2× headroom.
  static constexpr size_t kMaxChainTraversalDepth =
      static_cast<size_t>(kMaxAlternates) * 2;

  enum class Type : uint8_t {
    Empty = 0,
    FirstFrag = 1,
    MiddleFrag = 2,
    LastFrag = 3,
    SingleFrag = 4  // Complete document in one fragment
  };

  enum Flags : uint8_t {
    kFlagNone = 0,
    kFlagCompressed = 1 << 0,
    kFlagPinned = 1 << 1,
    kFlagPrecious = 1 << 2
  };

  uint32_t magic = kMagic;
  uint32_t len = 0;        // Fragment length (including header)
  uint64_t total_len = 0;  // Total document size

  std::array<std::byte, CacheKey::kDigestSize> first_key{};  // Primary key
  std::array<std::byte, CacheKey::kDigestSize>
      fragment_key{};  // This fragment's key

  uint32_t header_len = 0;
  Type doc_type = Type::Empty;
  uint8_t version_major = kVersionMajor;
  uint8_t version_minor = kVersionMinor;
  uint8_t flags = kFlagNone;

  uint32_t sync_serial = 0;
  uint32_t write_serial = 0;
  uint32_t pin_until = 0;  // Unix timestamp
  uint32_t checksum = 0;   // CRC32

  uint32_t frag_offset = 0;  // Offset within total document
  uint32_t hit_count = 0;    // Persistent hit counter (was: reserved)

  // Alternate chain fields (v2)
  uint64_t next_alternate_offset = 0;  // Offset to next alternate, 0 = none
  uint8_t alternate_id = 0;            // AlternateId enum value (0 = Original)
  uint8_t reserved2[3] = {};           // Alignment padding
  int64_t last_access = 0;             // Unix timestamp in milliseconds

  // Header size calculation:
  // magic(4) + len(4) + total_len(8) + first_key(32) + fragment_key(32) +
  // header_len(4) + doc_type(1) + version_major(1) + version_minor(1) +
  // flags(1) + sync_serial(4) + write_serial(4) + pin_until(4) + checksum(4) +
  // frag_offset(4) + hit_count(4) + next_alternate_offset(8) +
  // last_access(8) + alternate_id(1) + reserved2(3) = 132 bytes
  static constexpr size_t kHeaderSize = 132;

  // Field offsets within the header (eliminate magic numbers)
  // These are used for direct field updates without full document rewrite
  // version_major is field index 7 in the wire table (see document.cpp):
  // magic(4)+len(4)+total_len(8)+first_key(32)+fragment_key(32)+header_len(4)
  // +doc_type(1) = 85.  Named so the version gate can be exercised on-disk
  // without a magic number; kept honest by a static_assert in document.cpp.
  static constexpr size_t kVersionMajorOffset = 85;
  static constexpr size_t kChecksumOffset = 100;
  static constexpr size_t kHitCountOffset = 108;
  static constexpr size_t kNextAlternateOffsetPos = 112;
  static constexpr size_t kLastAccessOffset = 120;
  static constexpr size_t kAlternateIdOffset = 128;

  [[nodiscard]] bool is_valid() const { return magic == kMagic; }
  [[nodiscard]] bool is_version_compatible() const {
    return version_major == kVersionMajor;
  }
  [[nodiscard]] bool is_valid_and_compatible() const {
    return is_valid() && is_version_compatible();
  }
  [[nodiscard]] bool is_complete() const {
    return doc_type == Type::SingleFrag || doc_type == Type::LastFrag;
  }
  [[nodiscard]] bool is_first() const {
    return doc_type == Type::FirstFrag || doc_type == Type::SingleFrag;
  }

  [[nodiscard]] size_t header_data_size() const { return header_len; }
  [[nodiscard]] size_t content_data_size() const {
    // Guard against underflow and overflow from corrupted data.
    // Check each subtraction independently to avoid overflow when
    // kHeaderSize + header_len wraps on 32-bit targets.
    if (len < kHeaderSize || header_len > len - kHeaderSize) {
      return 0;
    }
    return len - kHeaderSize - header_len;
  }

  static uint32_t compute_checksum(std::span<const std::byte> data);
  [[nodiscard]] bool verify_checksum(std::span<const std::byte> content) const;

  std::span<std::byte> serialize(std::vector<std::byte> &buffer) const;
  static Document deserialize(std::span<const std::byte> data);
};

static_assert(sizeof(Document) >= Document::kHeaderSize,
              "Document struct must be at least kHeaderSize bytes");

class DocumentBuilder {
 public:
  DocumentBuilder();

  DocumentBuilder &set_key(const CacheKey &key);
  DocumentBuilder &set_header(std::span<const std::byte> header);
  DocumentBuilder &set_content(std::span<const std::byte> content);
  DocumentBuilder &set_type(Document::Type type);
  DocumentBuilder &set_flags(uint8_t flags);
  DocumentBuilder &set_total_length(uint64_t total_len);
  DocumentBuilder &set_frag_offset(uint32_t offset);
  DocumentBuilder &enable_checksum(bool enable);

  // Alternate chain support (v2)
  DocumentBuilder &set_alternate_id(uint8_t id);
  DocumentBuilder &set_next_alternate_offset(uint64_t offset);
  DocumentBuilder &set_hit_count(uint32_t count);
  DocumentBuilder &set_last_access(int64_t timestamp_ms);

  [[nodiscard]] std::vector<std::byte> build() const;

 private:
  CacheKey _key;
  std::vector<std::byte> _header;
  std::vector<std::byte> _content;
  Document::Type _type = Document::Type::SingleFrag;
  uint8_t _flags = Document::kFlagNone;
  uint64_t _total_len = 0;
  uint32_t _frag_offset = 0;
  bool _enable_checksum = true;
  uint8_t _alternate_id = 0;
  uint64_t _next_alternate_offset = 0;
  uint32_t _hit_count = 0;
  int64_t _last_access = 0;
};

class DocumentReader {
 public:
  explicit DocumentReader(std::span<const std::byte> data);

  [[nodiscard]] bool is_valid() const { return _valid; }
  [[nodiscard]] const Document &document() const { return _doc; }

  [[nodiscard]] std::span<const std::byte> header() const;
  [[nodiscard]] std::span<const std::byte> content() const;

  // Returns header_data + content (the region covered by the checksum)
  [[nodiscard]] std::span<const std::byte> payload() const;

  [[nodiscard]] CacheKey first_key() const;
  [[nodiscard]] CacheKey fragment_key() const;

 private:
  Document _doc;
  std::span<const std::byte> _data;
  bool _valid = false;
};

}  // namespace cyclone
