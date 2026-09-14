// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// libFuzzer target: document / alternate deserialization gauntlet.
//
// Attack surface: the on-disk document header (Document::deserialize /
// DocumentReader) and the compact 10-byte directory entry (DirEntry) that a
// reader consumes straight from mmap'd bytes.  These are the structures a
// lock-free reader parses out of a possibly-torn, possibly-adversarial cache
// file with no prior trust.
//
// Invariant under test: adversarial bytes must yield a clean parse result
// (valid/invalid) with NO crash, NO undefined behaviour, and every span the
// reader hands back must stay strictly inside the input buffer.  A corrupt
// length field must clamp the returned view, never license an over-read.
// AddressSanitizer enforces the memory-safety half; the explicit bounds
// checks below enforce the "no garbage served past the buffer" half.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "core/directory.hpp"
#include "core/document.hpp"

using namespace cyclone;

namespace {

bool within(std::span<const std::byte> view, const std::byte *base,
            size_t size) {
  if (view.empty()) {
    return true;
  }
  return view.data() >= base && view.data() + view.size() <= base + size;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  const auto *base = reinterpret_cast<const std::byte *>(data);
  std::span<const std::byte> bytes(base, size);

  // --- Document header + payload parsing --------------------------------
  DocumentReader reader(bytes);
  if (reader.is_valid()) {
    auto header = reader.header();
    auto content = reader.content();
    auto payload = reader.payload();

    // Every parsed view must sit inside the input buffer.  A violation here
    // is a real over-read bug, not a fuzzer artifact.
    if (!within(header, base, size) || !within(content, base, size) ||
        !within(payload, base, size)) {
      __builtin_trap();
    }

    const Document &doc = reader.document();
    (void)doc.header_data_size();
    (void)doc.content_data_size();
    (void)doc.is_complete();
    (void)doc.is_first();
    (void)doc.is_valid_and_compatible();
    (void)reader.first_key();
    (void)reader.fragment_key();
    // Checksum verification walks the payload span; must never over-read.
    (void)doc.verify_checksum(payload);
  }

  // Struct-level deserialize is defined for any size (short input -> magic 0).
  Document raw = Document::deserialize(bytes);
  (void)raw.content_data_size();
  (void)raw.is_valid();

  // --- DirEntry bit-pattern accessors -----------------------------------
  // Slide a 10-byte window across the input; every accessor must be total
  // over arbitrary bit patterns (offset is 40-bit clamped, etc.).
  for (size_t off = 0; off + DirEntry::kSize <= size; off += DirEntry::kSize) {
    DirEntry e;
    std::memcpy(e._w, data + off, DirEntry::kSize);
    (void)e.offset();
    (void)e.tag();
    (void)e.big();
    (void)e.size();
    (void)e.phase();
    (void)e.head();
    (void)e.pinned();
    (void)e.next();
    (void)e.is_empty();
    (void)e.approx_size();
  }

  return 0;
}
