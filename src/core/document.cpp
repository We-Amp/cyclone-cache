// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "document.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>

#include "crc32c.hpp"

namespace cyclone {

// The document checksum is the CRC-32C (Castagnoli) over header_data +
// content (everything after the 132-byte on-disk header).  The polynomial,
// init, reflection and xorout live in crc32c.hpp and are part of the on-disk
// format -- see the convention block there before touching any of it, and
// note that changing it requires a kVersionMajor bump (v8 is the CRC-32C
// one; v7 and earlier carried CRC-32/ISO-HDLC).

uint32_t Document::compute_checksum(std::span<const std::byte> data) {
  return crc32c(data);
}

bool Document::verify_checksum(std::span<const std::byte> content) const {
  return checksum == crc32c(content);
}

// Field descriptor for table-driven serialization.
// sizeof(Document) > kHeaderSize due to compiler padding, so we cannot use
// a single memcpy.  Instead we describe each field's offset-in-struct and
// size, then loop over the table.
namespace {
struct FieldDesc {
  size_t struct_offset;  // offsetof(Document, field)
  size_t size;           // sizeof(field)
};

// The wire layout is the concatenation of these fields in order.
// Adding or reordering fields requires updating this table AND kHeaderSize.
constexpr FieldDesc kDocumentFields[] = {
    {offsetof(Document, magic), sizeof(Document::magic)},
    {offsetof(Document, len), sizeof(Document::len)},
    {offsetof(Document, total_len), sizeof(Document::total_len)},
    {offsetof(Document, first_key), sizeof(Document::first_key)},
    {offsetof(Document, fragment_key), sizeof(Document::fragment_key)},
    {offsetof(Document, header_len), sizeof(Document::header_len)},
    {offsetof(Document, doc_type), sizeof(Document::doc_type)},
    {offsetof(Document, version_major), sizeof(Document::version_major)},
    {offsetof(Document, version_minor), sizeof(Document::version_minor)},
    {offsetof(Document, flags), sizeof(Document::flags)},
    {offsetof(Document, sync_serial), sizeof(Document::sync_serial)},
    {offsetof(Document, write_serial), sizeof(Document::write_serial)},
    {offsetof(Document, pin_until), sizeof(Document::pin_until)},
    {offsetof(Document, checksum), sizeof(Document::checksum)},
    {offsetof(Document, frag_offset), sizeof(Document::frag_offset)},
    {offsetof(Document, hit_count), sizeof(Document::hit_count)},
    {offsetof(Document, next_alternate_offset),
     sizeof(Document::next_alternate_offset)},
    {offsetof(Document, last_access), sizeof(Document::last_access)},
    {offsetof(Document, alternate_id), sizeof(Document::alternate_id)},
    {offsetof(Document, reserved2), sizeof(Document::reserved2)},
};

// Verify at compile time that the field table covers exactly kHeaderSize bytes.
consteval size_t sum_field_sizes() {
  size_t total = 0;
  for (const auto &f : kDocumentFields) total += f.size;
  return total;
}
static_assert(sum_field_sizes() == Document::kHeaderSize,
              "Field table must cover exactly kHeaderSize bytes");

// Verify that named wire-offset constants match the field table positions.
// This catches silent misalignment if fields are added or reordered.
consteval size_t wire_offset_of(size_t field_index) {
  size_t offset = 0;
  for (size_t i = 0; i < field_index; ++i) offset += kDocumentFields[i].size;
  return offset;
}
// version_major is field index 7, checksum is 13, hit_count is 15,
// next_alternate_offset is 16, last_access is 17, alternate_id is 18 (v6)
static_assert(wire_offset_of(7) == Document::kVersionMajorOffset,
              "kVersionMajorOffset out of sync with field table");
static_assert(wire_offset_of(11) == Document::kWriteSerialOffset,
              "kWriteSerialOffset out of sync with field table");
static_assert(wire_offset_of(13) == Document::kChecksumOffset,
              "kChecksumOffset out of sync with field table");
static_assert(wire_offset_of(15) == Document::kHitCountOffset,
              "kHitCountOffset out of sync with field table");
static_assert(wire_offset_of(16) == Document::kNextAlternateOffsetPos,
              "kNextAlternateOffsetPos out of sync with field table");
static_assert(wire_offset_of(17) == Document::kLastAccessOffset,
              "kLastAccessOffset out of sync with field table");
static_assert(wire_offset_of(18) == Document::kAlternateIdOffset,
              "kAlternateIdOffset out of sync with field table");
// The three in-place-mutated fields must be naturally aligned so the RMW
// sites can store them via std::atomic_ref on the shared mapping (UB on
// ARM64 otherwise); this holds only when doc starts are 8-aligned too (see
// allocate_write_slot's v6 padding).
static_assert(Document::kHitCountOffset % 4 == 0,
              "hit_count must be 4-aligned for atomic_ref");
static_assert(Document::kNextAlternateOffsetPos % 8 == 0,
              "next_alternate_offset must be 8-aligned for atomic_ref");
static_assert(Document::kLastAccessOffset % 8 == 0,
              "last_access must be 8-aligned for atomic_ref");
}  // namespace

std::span<std::byte> Document::serialize(std::vector<std::byte> &buffer) const {
  size_t old_size = buffer.size();
  buffer.resize(old_size + kHeaderSize);

  const auto *src = reinterpret_cast<const std::byte *>(this);
  std::byte *dst = buffer.data() + old_size;

  for (const auto &f : kDocumentFields) {
    std::memcpy(dst, src + f.struct_offset, f.size);
    dst += f.size;
  }

  return {buffer.data() + old_size, kHeaderSize};
}

Document Document::deserialize(std::span<const std::byte> data) {
  Document doc;

  if (data.size() < kHeaderSize) {
    doc.magic = 0;
    return doc;
  }

  auto *dst = reinterpret_cast<std::byte *>(&doc);
  const std::byte *base = data.data();

  // The three live-mutated fields (hit_count, next_alternate_offset,
  // last_access) can be written in place on a shared mapping by a concurrent
  // RMW writer (update_hit_count_sync / remove_alternate_sync's chain
  // repoint) via std::atomic_ref.  Reading them with a plain memcpy while
  // such a store is in flight is a data race (UB/TSan).  On the shared
  // mapping v6 keeps all three naturally aligned (doc starts 8-aligned;
  // offsets 108/112/120), so we load them atomically.  Private deserialize
  // buffers (tests, round-trips) may be under-aligned but are never
  // concurrently mutated, so an unaligned base safely falls back to memcpy.
  const bool atomic_ok = reinterpret_cast<std::uintptr_t>(base) % 8 == 0;
  std::size_t wire_off = 0;
  for (const auto &f : kDocumentFields) {
    std::byte *fsrc = const_cast<std::byte *>(base) + wire_off;
    if (atomic_ok && wire_off == kHitCountOffset) {
      uint32_t v =
          std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t *>(fsrc))
              .load(std::memory_order_acquire);
      std::memcpy(dst + f.struct_offset, &v, sizeof(v));
    } else if (atomic_ok && wire_off == kNextAlternateOffsetPos) {
      uint64_t v =
          std::atomic_ref<uint64_t>(*reinterpret_cast<uint64_t *>(fsrc))
              .load(std::memory_order_acquire);
      std::memcpy(dst + f.struct_offset, &v, sizeof(v));
    } else if (atomic_ok && wire_off == kLastAccessOffset) {
      int64_t v = std::atomic_ref<int64_t>(*reinterpret_cast<int64_t *>(fsrc))
                      .load(std::memory_order_acquire);
      std::memcpy(dst + f.struct_offset, &v, sizeof(v));
    } else {
      std::memcpy(dst + f.struct_offset, fsrc, f.size);
    }
    wire_off += f.size;
  }

  return doc;
}

// DocumentBuilder

DocumentBuilder::DocumentBuilder() = default;

DocumentBuilder &DocumentBuilder::set_key(const CacheKey &key) {
  _key = key;
  return *this;
}

DocumentBuilder &DocumentBuilder::set_header(
    std::span<const std::byte> header) {
  _header.assign(header.begin(), header.end());
  return *this;
}

DocumentBuilder &DocumentBuilder::set_content(
    std::span<const std::byte> content) {
  _content.assign(content.begin(), content.end());
  return *this;
}

DocumentBuilder &DocumentBuilder::set_type(Document::Type type) {
  _type = type;
  return *this;
}

DocumentBuilder &DocumentBuilder::set_flags(uint8_t flags) {
  _flags = flags;
  return *this;
}

DocumentBuilder &DocumentBuilder::set_total_length(uint64_t total_len) {
  _total_len = total_len;
  return *this;
}

DocumentBuilder &DocumentBuilder::set_frag_offset(uint32_t offset) {
  _frag_offset = offset;
  return *this;
}

DocumentBuilder &DocumentBuilder::enable_checksum(bool enable) {
  _enable_checksum = enable;
  return *this;
}

DocumentBuilder &DocumentBuilder::set_alternate_id(uint8_t id) {
  _alternate_id = id;
  return *this;
}

DocumentBuilder &DocumentBuilder::set_next_alternate_offset(uint64_t offset) {
  _next_alternate_offset = offset;
  return *this;
}

DocumentBuilder &DocumentBuilder::set_hit_count(uint32_t count) {
  _hit_count = count;
  return *this;
}

DocumentBuilder &DocumentBuilder::set_last_access(int64_t timestamp_ms) {
  _last_access = timestamp_ms;
  return *this;
}

std::vector<std::byte> DocumentBuilder::build() const {
  auto result = serialize_head(_content, _content.size());
  if (result.empty()) {
    return result;
  }
  result.insert(result.end(), _content.begin(), _content.end());
  return result;
}

std::vector<std::byte> DocumentBuilder::build_head(
    std::span<const std::byte> content, size_t extra) const {
  return serialize_head(content, extra);
}

std::vector<std::byte> DocumentBuilder::serialize_head(
    std::span<const std::byte> content, size_t extra) const {
  // Check for 32-bit overflow before casting
  size_t total_size = Document::kHeaderSize + _header.size() + content.size();
  if (total_size > std::numeric_limits<uint32_t>::max()) {
    return {};  // Overflow: document too large for 32-bit len field
  }
  if (_header.size() > std::numeric_limits<uint32_t>::max()) {
    return {};  // Overflow: header too large for 32-bit header_len field
  }

  Document doc;
  doc.len = static_cast<uint32_t>(total_size);
  doc.total_len = _total_len > 0 ? _total_len : content.size();
  doc.header_len = static_cast<uint32_t>(_header.size());
  doc.doc_type = _type;
  doc.flags = _flags;
  doc.frag_offset = _frag_offset;

  // Alternate chain fields (v2)
  doc.alternate_id = _alternate_id;
  doc.next_alternate_offset = _next_alternate_offset;
  doc.hit_count = _hit_count;
  doc.last_access = _last_access;

  auto digest = _key.digest();
  std::memcpy(doc.first_key.data(), digest.data(), digest.size());
  std::memcpy(doc.fragment_key.data(), digest.data(), digest.size());

  std::vector<std::byte> result;
  result.reserve(Document::kHeaderSize + _header.size() + extra);

  doc.serialize(result);
  result.insert(result.end(), _header.begin(), _header.end());

  if (_enable_checksum) {
    // Header bytes, then content: crc32c_update chains, so this is the CRC
    // of the contiguous payload without the payload ever being contiguous.
    const uint32_t crc =
        crc32c_update(crc32c(std::span<const std::byte>(_header)), content);
    std::byte *checksum_location = result.data() + Document::kChecksumOffset;
    std::memcpy(checksum_location, &crc, sizeof(crc));
  }

  return result;
}

// DocumentReader

DocumentReader::DocumentReader(std::span<const std::byte> data) : _data(data) {
  if (data.size() >= Document::kHeaderSize) {
    _doc = Document::deserialize(data);
    // Gate on the document's FORMAT VERSION, not just its magic.
    //
    // A NEW-format binary can still find itself reading OLD-format documents:
    // when the reset gate is degraded/ungated (a lock-less filesystem), or
    // transiently before an owed reset has run.  With a magic-only check it
    // would MISPARSE them -- serving
    // wrong bytes -- instead of missing them.  A foreign version must read as a
    // MISS.  (Under a live peer Volume::open() REFUSES rather than reset; this
    // gate is the defense-in-depth for the paths where a foreign-format record
    // is nonetheless reached.)
    //
    // Every read, chain hop, alternate walk, removal and hit-count writeback
    // funnels through DocumentReader / map_document, so this one check covers
    // them all.  Cost is a uint8_t compare on a header already in L1.
    _valid = _doc.is_valid_and_compatible();
  }
}

std::span<const std::byte> DocumentReader::header() const {
  if (!_valid || _data.size() < Document::kHeaderSize ||
      _doc.header_len > _data.size() - Document::kHeaderSize) {
    return {};
  }
  return _data.subspan(Document::kHeaderSize, _doc.header_len);
}

std::span<const std::byte> DocumentReader::content() const {
  if (!_valid) {
    return {};
  }
  // Validate each step independently to avoid overflow when
  // kHeaderSize + header_len wraps on 32-bit targets.
  if (_doc.len < Document::kHeaderSize ||
      _doc.header_len > _doc.len - Document::kHeaderSize) {
    return {};
  }
  size_t content_start = Document::kHeaderSize + _doc.header_len;
  if (content_start > _data.size()) {
    return {};
  }
  size_t content_len = _doc.len - Document::kHeaderSize - _doc.header_len;
  if (content_start + content_len > _data.size()) {
    content_len = _data.size() - content_start;
  }
  return _data.subspan(content_start, content_len);
}

std::span<const std::byte> DocumentReader::payload() const {
  if (!_valid || _data.size() <= Document::kHeaderSize) {
    return {};
  }
  size_t payload_len =
      _doc.len > Document::kHeaderSize ? _doc.len - Document::kHeaderSize : 0;
  if (Document::kHeaderSize + payload_len > _data.size()) {
    payload_len = _data.size() - Document::kHeaderSize;
  }
  return _data.subspan(Document::kHeaderSize, payload_len);
}

CacheKey DocumentReader::first_key() const {
  return CacheKey::from_digest(
      std::span<const std::byte, CacheKey::kDigestSize>(_doc.first_key));
}

CacheKey DocumentReader::fragment_key() const {
  return CacheKey::from_digest(
      std::span<const std::byte, CacheKey::kDigestSize>(_doc.fragment_key));
}

}  // namespace cyclone
