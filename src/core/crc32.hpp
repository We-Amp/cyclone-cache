// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef CYCLONE_CORE_CRC32_HPP
#define CYCLONE_CORE_CRC32_HPP

#include <cstddef>
#include <cstdint>
#include <span>

namespace cyclone {

// CRC-32/ISO-HDLC -- the "IEEE" / zlib CRC32 that Cyclone documents have
// always carried on disk.  Convention (unchanged, and load-bearing: it is
// baked into every cache file ever written):
//
//   width      32
//   poly       0x04C11DB7, reflected form 0xEDB88320
//   init       0xFFFFFFFF
//   refin      true   (bytes enter least-significant bit first)
//   refout     true
//   xorout     0xFFFFFFFF
//   check      crc32("123456789") == 0xCBF43926
//   crc32("")  == 0x00000000
//
// The running state handed to and returned by crc32_update() is the
// zlib-style *external* value: start at 0, and the value returned after the
// last chunk is the final checksum.  The init/xorout complement is applied
// inside the function, so chunking is transparent:
//
//   crc32_update(crc32_update(0, a), b) == crc32(a ++ b)

// Incremental CRC32.  `state` is 0 for a fresh checksum.
uint32_t crc32_update(uint32_t state, std::span<const std::byte> data) noexcept;

// One-shot CRC32 over `data`.
uint32_t crc32(std::span<const std::byte> data) noexcept;

// --- Implementation probes -------------------------------------------------
//
// Exposed for the micro-benchmark and for tests that want to pin a specific
// implementation rather than whatever runtime dispatch selected.  All of
// these produce bit-identical results; only their speed differs.

// Portable slice-by-16 table path.  Always available.
uint32_t crc32_update_portable(uint32_t state,
                               std::span<const std::byte> data) noexcept;

// ARMv8 CRC32-instruction path.  Falls back to the portable path verbatim on
// targets without it, so callers never need to guard the call.
uint32_t crc32_update_hardware(uint32_t state,
                               std::span<const std::byte> data) noexcept;

// True when crc32_update_hardware() really is a hardware path on this CPU.
bool crc32_has_hardware() noexcept;

// Name of the implementation runtime dispatch settled on ("slice-by-16" or
// "armv8-crc32").
const char* crc32_impl_name() noexcept;

}  // namespace cyclone

#endif  // CYCLONE_CORE_CRC32_HPP
