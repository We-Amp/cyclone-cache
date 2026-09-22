// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef CYCLONE_CORE_CRC32C_HPP
#define CYCLONE_CORE_CRC32C_HPP

#include <cstddef>
#include <cstdint>
#include <span>

namespace cyclone {

// CRC-32C / Castagnoli -- the checksum Cyclone documents carry on disk from
// on-disk format v8 onward.  Convention (frozen, and load-bearing: it is baked
// into every cache file a v8 binary writes):
//
//   width        32
//   poly         0x1EDC6F41, reflected form 0x82F63B78
//   init         0xFFFFFFFF
//   refin        true   (bytes enter least-significant bit first)
//   refout       true
//   xorout       0xFFFFFFFF
//   check        crc32c("123456789") == 0xE3069283
//   crc32c("")   == 0x00000000
//
// Chosen over the CRC-32/ISO-HDLC of v7 and earlier because both mainstream
// server architectures implement THIS polynomial in hardware -- x86-64 SSE4.2
// (`crc32`) and ARMv8 (`crc32c*`) -- while ISO-HDLC has hardware only on
// ARMv8.  The document checksum is re-verified on every cold read, so on x86
// it was the cold-read bandwidth ceiling.
//
// The running state handed to and returned by crc32c_update() is the
// zlib-style *external* value: start at 0, and the value returned after the
// last chunk is the final checksum.  The init/xorout complement is applied
// inside the function, so chunking is transparent:
//
//   crc32c_update(crc32c_update(0, a), b) == crc32c(a ++ b)

// Incremental CRC-32C.  `state` is 0 for a fresh checksum.
uint32_t crc32c_update(uint32_t state,
                       std::span<const std::byte> data) noexcept;

// One-shot CRC-32C over `data`.
uint32_t crc32c(std::span<const std::byte> data) noexcept;

// Block sizes of the hardware paths' 3-way interleave (see crc32c.cpp).  Only
// the tests need these, to land inputs exactly on the block boundaries.
inline constexpr size_t kCrc32cLongBlock = 8192;
inline constexpr size_t kCrc32cShortBlock = 256;

// --- Implementation probes -------------------------------------------------
//
// Exposed for the micro-benchmark and for tests that want to pin a specific
// implementation rather than whatever runtime dispatch selected.  All of
// these produce bit-identical results; only their speed differs.

// Portable slice-by-16 table path.  Always available.
uint32_t crc32c_update_portable(uint32_t state,
                                std::span<const std::byte> data) noexcept;

// CRC32C-instruction path (SSE4.2 on x86-64, `crc32c*` on ARMv8), with the
// 3-way interleave that hides the instruction's latency.  Falls back to the
// portable path verbatim on targets without it, so callers never need to
// guard the call.
uint32_t crc32c_update_hardware(uint32_t state,
                                std::span<const std::byte> data) noexcept;

// The same instructions over one dependent chain instead of three.
// Benchmark-only: it exists so the interleave's win is measured rather than
// assumed, and so the tests can check that the two agree.  Falls back to the
// portable path on targets without the instructions.
uint32_t crc32c_update_hardware_1way(uint32_t state,
                                     std::span<const std::byte> data) noexcept;

// True when the crc32c_update_hardware* entry points really are hardware
// paths on this CPU.
bool crc32c_has_hardware() noexcept;

// Name of the implementation runtime dispatch settled on ("slice-by-16",
// "sse4.2-crc32c" or "armv8-crc32c").
const char* crc32c_impl_name() noexcept;

}  // namespace cyclone

#endif  // CYCLONE_CORE_CRC32C_HPP
