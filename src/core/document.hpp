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
      9;  // Version 6: hit_count/next_alternate_offset/last_access laid out
          // naturally aligned so the in-place header RMW sites can store them
          // via std::atomic_ref (kept in lockstep with
          // VolumeHeader::kFormatVersionMajor; pre-v6 volumes auto-reset)
          // Version 7: alternate chains are depth-bounded at write time; the
          // bump exists to leave pre-bound chains (over-deep, possibly cyclic)
          // behind rather than repair them.  Lockstep with
          // VolumeHeader::kFormatVersionMajor is not ceremony: DocumentReader
          // gates on this field, so any stray v6 document a v7 binary reaches
          // reads as a MISS instead of a misparse.
          // Version 8: the checksum below is CRC-32C (was CRC-32/ISO-HDLC).
          // Pre-v8 documents would fail verification, and the fingerprinted
          // filename (which mixes the format major) makes them invisible
          // anyway -- consumers take a cold cache, never a misparse.
          // Version 9: the checksum also covers the fixed header: every
          // field that never changes after the write (see
          // checksum_header_seed).  A v8 checksum covered header bytes +
          // content only, so v8 documents would fail verification; the
          // same fingerprinted-filename separation applies.
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

  uint32_t fill_serial = 0;  // see kFillSerialOffset
  uint32_t write_serial = 0;
  uint32_t pin_until = 0;  // Unix timestamp
  uint32_t checksum = 0;   // CRC-32C (v8+); CRC-32/ISO-HDLC before v8

  uint32_t frag_offset = 0;  // Offset within total document
  uint32_t hit_count = 0;    // Persistent hit counter (was: reserved)

  // Alternate chain fields (v2)
  uint64_t next_alternate_offset = 0;  // Link to the next alternate, 0 = none
                                       // (offset + serial, see make_link)
  uint8_t alternate_id = 0;            // AlternateId enum value (0 = Original)
  uint8_t reserved2[3] = {};           // Alignment padding
  int64_t last_access = 0;             // Unix timestamp in milliseconds

  // Header size calculation:
  // magic(4) + len(4) + total_len(8) + first_key(32) + fragment_key(32) +
  // header_len(4) + doc_type(1) + version_major(1) + version_minor(1) +
  // flags(1) + fill_serial(4) + write_serial(4) + pin_until(4) + checksum(4) +
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
  // fill_serial carries the serial of the fill that wrote the document (the
  // write-lock acquisition it was written under; see WriteSlot::fill_serial
  // in volume.hpp), and the directory entry that publishes it carries the
  // same value, as does every chain link made to it (make_link below).  A
  // read uses a document only when its serial agrees with the entry or the
  // link it was reached through, so the late bytes of a writer whose lock
  // was taken over in the middle of its fill are never served in place of
  // the document the next holder published at the same offset, neither
  // through its entry nor through a later alternate's link to it.  Stamped
  // after the checksum is computed, so it sits outside it.  (Named
  // sync_serial and always 0 before.)
  static constexpr size_t kFillSerialOffset = 88;
  // write_serial carries the stripe PASS the document was written in (wrap
  // retention's pass stamp, mod 2^32; see Volume::patch_pass_stamp).  It
  // was always 0 before, and it sits outside the checksum.
  static constexpr size_t kWriteSerialOffset = 92;
  static constexpr size_t kChecksumOffset = 100;
  static constexpr size_t kHitCountOffset = 108;
  static constexpr size_t kNextAlternateOffsetPos = 112;
  static constexpr size_t kLastAccessOffset = 120;
  static constexpr size_t kAlternateIdOffset = 128;

  // A chain link (next_alternate_offset) carries the successor's
  // stripe-relative offset in its low 48 bits and the successor's
  // fill_serial in its top 16 bits, so a hop can tell the document the link
  // was made to from other bytes at the same offset (see kFillSerialOffset).
  // Offsets are 40 bits (DirEntry), so the top bits were always zero; a link
  // of 0 is still "no successor".  The field is outside the checksum and
  // stored atomically, so a repoint writes offset and serial at once.
  static constexpr unsigned kLinkSerialShift = 48;
  static constexpr uint64_t kLinkOffsetMask =
      (uint64_t{1} << kLinkSerialShift) - 1;
  [[nodiscard]] static constexpr uint64_t make_link(uint64_t rel_offset,
                                                    uint16_t serial) noexcept {
    return rel_offset == 0 ? 0
                           : (rel_offset & kLinkOffsetMask) |
                                 (uint64_t{serial} << kLinkSerialShift);
  }
  [[nodiscard]] static constexpr uint64_t link_offset(uint64_t link) noexcept {
    return link & kLinkOffsetMask;
  }
  [[nodiscard]] static constexpr uint16_t link_serial(uint64_t link) noexcept {
    return static_cast<uint16_t>(link >> kLinkSerialShift);
  }
  // Is this document the one `link` was made to?
  [[nodiscard]] bool bound_by_link(uint64_t link) const noexcept {
    return fill_serial == link_serial(link);
  }
  // The link to this document, at `rel_offset`.
  [[nodiscard]] uint64_t link_to(uint64_t rel_offset) const noexcept {
    return make_link(rel_offset, static_cast<uint16_t>(fill_serial));
  }

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

  // The shape every document a volume writes has: one complete fragment
  // whose total_len is exactly its content size.  len, total_len, header_len
  // and doc_type sit in the fixed header, outside the checksum, so a read
  // that serves a document checks this identity as well as the CRC: a
  // document whose fields disagree reads as corrupt (a miss), whatever its
  // payload's checksum says.
  [[nodiscard]] bool has_volume_shape() const {
    return doc_type == Type::SingleFrag && frag_offset == 0 &&
           len >= kHeaderSize && header_len <= len - kHeaderSize &&
           total_len == uint64_t{len} - kHeaderSize - header_len;
  }

  static uint32_t compute_checksum(std::span<const std::byte> data);

  // The document checksum (v9) is the CRC-32C over this document's
  // canonical fixed header followed by its payload (header bytes +
  // content).  The canonical header is the 132-byte wire header with the
  // fields the cache stamps after the checksum is computed or updates in
  // place set to zero: fill_serial and write_serial (the fill and pass
  // stamps), the checksum itself, hit_count, next_alternate_offset and
  // last_access.  Every other field -- magic, the lengths, both keys,
  // header_len, doc_type, the version, flags, pin_until, frag_offset,
  // alternate_id and the reserved bytes -- is covered, so a change to any
  // of them fails verification.
  // Returns the CRC-32C state after the canonical header, to be continued
  // over the payload with crc32c_update().
  [[nodiscard]] uint32_t checksum_header_seed() const;
  // `payload` is header bytes + content (DocumentReader::payload()).
  [[nodiscard]] bool verify_checksum(std::span<const std::byte> payload) const;

  std::span<std::byte> serialize(std::vector<std::byte> &buffer) const;
  // The wire header into exactly kHeaderSize bytes at `out`.
  void serialize_to(std::byte *out) const;
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

  // The document's head only: the kHeaderSize header followed by the header
  // bytes, with the checksum computed over header bytes + `content`.  For a
  // writer that writes `content` from its own buffer right behind the head
  // (two writes, or a gathered one) instead of copying it into one
  // contiguous document: head ++ content is byte-identical to build() after
  // set_content(content).  Content given to set_content() is ignored.  Empty
  // on overflow, like build().  `extra` reserves that many more bytes of
  // capacity behind the head, for a caller that appends to it.
  [[nodiscard]] std::vector<std::byte> build_head(
      std::span<const std::byte> content, size_t extra = 0) const;

 private:
  // The head of a document whose content is `content`, with `extra` more
  // bytes of capacity reserved behind it (build() appends the content).
  [[nodiscard]] std::vector<std::byte> serialize_head(
      std::span<const std::byte> content, size_t extra) const;

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

// Tag for a reader over bytes that live in a shared file mapping.
struct SharedMappingTag {};
inline constexpr SharedMappingTag kSharedMapping{};

class DocumentReader {
 public:
  explicit DocumentReader(std::span<const std::byte> data);
  // Bytes inside a shared mapping: documents start 8-aligned there, which the
  // atomic loads of the live-mutated header fields rely on (a misaligned base
  // would silently fall back to a racy plain read).  A misaligned base yields
  // an invalid reader in every build.
  DocumentReader(SharedMappingTag, std::span<const std::byte> data);

  [[nodiscard]] bool is_valid() const { return _valid; }
  // is_valid() plus Document::has_volume_shape(): what every read path that
  // serves or enumerates a stored document requires.
  [[nodiscard]] bool is_servable() const {
    return _valid && _doc.has_volume_shape();
  }
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
