// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "crc32.hpp"

#include <array>
#include <bit>
#include <cstring>

// --- Platform selection ----------------------------------------------------
//
// ARMv8 has a CRC32 instruction group for exactly this polynomial (the
// "IEEE"/zlib CRC32; `crc32b/h/w/x`, NOT the `crc32cb/...` CRC-32C group), so
// aarch64 gets a hardware path.  Two flavours:
//
//   * The toolchain already targets a CPU with the extension
//     (__ARM_FEATURE_CRC32 -- every Apple silicon build, Windows on ARM, and
//     any -march=armv8-a+crc build).  We use the <arm_acle.h> intrinsics
//     directly; no runtime probe is needed.
//   * A baseline armv8-a Linux build.  The intrinsics are not exposed by the
//     ACLE header in that case, so the three instructions are emitted with
//     inline asm under `.arch_extension crc` (the same trick the kernel and
//     zlib-ng use), and the path is selected at run time from
//     AT_HWCAP & HWCAP_CRC32.  Emitting them this way keeps everything in one
//     translation unit compiled with the project's stock flags, so no
//     consumer build (Bazel, vendored copies) needs per-file flag handling.
//
// x86-64 deliberately has NO hardware path: SSE4.2's `crc32` instruction
// computes CRC-32C (Castagnoli, poly 0x1EDC6F41), a DIFFERENT checksum.
// Switching Cyclone to CRC-32C would change the on-disk format and invalidate
// every existing cache file, so it is not done here.  Future option, if the
// format is ever revved: either adopt CRC-32C and use SSE4.2/`crc32c` +
// ARMv8 `crc32c*`, or keep this polynomial and add a PCLMULQDQ folding path
// (carry-less multiply works for any polynomial and would reach >20 GB/s).
// Until then x86-64 runs the portable slice-by-16 table path.

#if defined(__aarch64__) || defined(_M_ARM64)
#define CYCLONE_CRC32_ARM 1
#endif

#if defined(CYCLONE_CRC32_ARM)
#if defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#define CYCLONE_CRC32_ARM_ALWAYS 1
#elif defined(__linux__) && defined(__GNUC__)
#include <sys/auxv.h>
#define CYCLONE_CRC32_ARM_RUNTIME 1
#ifndef HWCAP_CRC32
#define HWCAP_CRC32 (1U << 7)
#endif
#endif
#endif

namespace cyclone {

namespace {

// Reflected form of the IEEE 802.3 polynomial 0x04C11DB7.
constexpr uint32_t kPolyReflected = 0xEDB88320U;

constexpr size_t kSliceRows = 16;

using SliceTables = std::array<std::array<uint32_t, 256>, kSliceRows>;

// Slice-by-16 tables (16 KiB of rodata).  Row 0 is the classic
// byte-at-a-time table; row s is "row s-1 advanced by one more zero byte",
// which is what lets the main loop consume sixteen input bytes per iteration
// with sixteen independent, dependency-free lookups.
constexpr SliceTables make_slice_tables() {
  SliceTables t{};
  for (uint32_t n = 0; n < 256; ++n) {
    uint32_t c = n;
    for (int k = 0; k < 8; ++k) {
      c = ((c & 1U) != 0U) ? (kPolyReflected ^ (c >> 1)) : (c >> 1);
    }
    t[0][n] = c;
  }
  for (uint32_t n = 0; n < 256; ++n) {
    for (size_t s = 1; s < kSliceRows; ++s) {
      const uint32_t prev = t[s - 1][n];
      t[s][n] = (prev >> 8) ^ t[0][prev & 0xFFU];
    }
  }
  return t;
}

constexpr SliceTables kSlice = make_slice_tables();

constexpr uint32_t bswap32(uint32_t v) noexcept {
  return ((v & 0x000000FFU) << 24) | ((v & 0x0000FF00U) << 8) |
         ((v >> 8) & 0x0000FF00U) | ((v >> 24) & 0x000000FFU);
}

constexpr uint64_t bswap64(uint64_t v) noexcept {
  return (static_cast<uint64_t>(bswap32(static_cast<uint32_t>(v))) << 32) |
         bswap32(static_cast<uint32_t>(v >> 32));
}

// Unaligned little-endian loads.  The CRC consumes bytes low-end first, so
// the words must be read little-endian on every host.
inline uint32_t load_le32(const uint8_t* p) noexcept {
  uint32_t v = 0;
  std::memcpy(&v, p, sizeof(v));
  if constexpr (std::endian::native == std::endian::big) {
    v = bswap32(v);
  }
  return v;
}

inline uint64_t load_le64(const uint8_t* p) noexcept {
  uint64_t v = 0;
  std::memcpy(&v, p, sizeof(v));
  if constexpr (std::endian::native == std::endian::big) {
    v = bswap64(v);
  }
  return v;
}

inline uint32_t crc_byte(uint32_t crc, uint8_t b) noexcept {
  return kSlice[0][(crc ^ b) & 0xFFU] ^ (crc >> 8);
}

// All the *_impl functions below take and return the INTERNAL (complemented)
// CRC register; crc32_update() applies the 0xFFFFFFFF init/xorout around
// them.

uint32_t slice_by_16_impl(uint32_t crc, const uint8_t* p, size_t len) noexcept {
  // Unaligned head: step to an 8-byte boundary so the main loop's word loads
  // are aligned (correctness never depends on this -- the loads go through
  // memcpy -- but it is measurably faster).
  while (len != 0 && (reinterpret_cast<uintptr_t>(p) & 7U) != 0U) {
    crc = crc_byte(crc, *p++);
    --len;
  }
  while (len >= 16) {
    const uint32_t w0 = load_le32(p) ^ crc;
    const uint32_t w1 = load_le32(p + 4);
    const uint32_t w2 = load_le32(p + 8);
    const uint32_t w3 = load_le32(p + 12);
    crc = kSlice[15][w0 & 0xFFU] ^ kSlice[14][(w0 >> 8) & 0xFFU] ^
          kSlice[13][(w0 >> 16) & 0xFFU] ^ kSlice[12][(w0 >> 24) & 0xFFU] ^
          kSlice[11][w1 & 0xFFU] ^ kSlice[10][(w1 >> 8) & 0xFFU] ^
          kSlice[9][(w1 >> 16) & 0xFFU] ^ kSlice[8][(w1 >> 24) & 0xFFU] ^
          kSlice[7][w2 & 0xFFU] ^ kSlice[6][(w2 >> 8) & 0xFFU] ^
          kSlice[5][(w2 >> 16) & 0xFFU] ^ kSlice[4][(w2 >> 24) & 0xFFU] ^
          kSlice[3][w3 & 0xFFU] ^ kSlice[2][(w3 >> 8) & 0xFFU] ^
          kSlice[1][(w3 >> 16) & 0xFFU] ^ kSlice[0][(w3 >> 24) & 0xFFU];
    p += 16;
    len -= 16;
  }
  if (len >= 8) {
    const uint32_t lo = load_le32(p) ^ crc;
    const uint32_t hi = load_le32(p + 4);
    crc = kSlice[7][lo & 0xFFU] ^ kSlice[6][(lo >> 8) & 0xFFU] ^
          kSlice[5][(lo >> 16) & 0xFFU] ^ kSlice[4][(lo >> 24) & 0xFFU] ^
          kSlice[3][hi & 0xFFU] ^ kSlice[2][(hi >> 8) & 0xFFU] ^
          kSlice[1][(hi >> 16) & 0xFFU] ^ kSlice[0][(hi >> 24) & 0xFFU];
    p += 8;
    len -= 8;
  }
  // Unaligned tail.
  while (len != 0) {
    crc = crc_byte(crc, *p++);
    --len;
  }
  return crc;
}

#if defined(CYCLONE_CRC32_ARM_ALWAYS) || defined(CYCLONE_CRC32_ARM_RUNTIME)

#if defined(CYCLONE_CRC32_ARM_ALWAYS)
inline uint32_t hw_crc32b(uint32_t crc, uint8_t v) noexcept {
  return __crc32b(crc, v);
}
inline uint32_t hw_crc32w(uint32_t crc, uint32_t v) noexcept {
  return __crc32w(crc, v);
}
inline uint32_t hw_crc32d(uint32_t crc, uint64_t v) noexcept {
  return __crc32d(crc, v);
}
#else
// Baseline armv8-a: same three instructions, spelled so the assembler will
// accept them without the extension in -march.
inline uint32_t hw_crc32b(uint32_t crc, uint8_t v) noexcept {
  __asm__(".arch_extension crc\ncrc32b %w0, %w0, %w1"
          : "+r"(crc)
          : "r"(static_cast<uint32_t>(v)));
  return crc;
}
inline uint32_t hw_crc32w(uint32_t crc, uint32_t v) noexcept {
  __asm__(".arch_extension crc\ncrc32w %w0, %w0, %w1" : "+r"(crc) : "r"(v));
  return crc;
}
inline uint32_t hw_crc32d(uint32_t crc, uint64_t v) noexcept {
  __asm__(".arch_extension crc\ncrc32x %w0, %w0, %x1" : "+r"(crc) : "r"(v));
  return crc;
}
#endif

uint32_t arm_crc32_impl(uint32_t crc, const uint8_t* p, size_t len) noexcept {
  while (len != 0 && (reinterpret_cast<uintptr_t>(p) & 7U) != 0U) {
    crc = hw_crc32b(crc, *p++);
    --len;
  }
  // The instruction is fully serial on the CRC register, so the unroll only
  // hides load and loop overhead; it is still enough to run at many GB/s.
  while (len >= 64) {
    for (int i = 0; i < 8; ++i) {
      crc = hw_crc32d(crc, load_le64(p));
      p += 8;
    }
    len -= 64;
  }
  while (len >= 8) {
    crc = hw_crc32d(crc, load_le64(p));
    p += 8;
    len -= 8;
  }
  if (len >= 4) {
    crc = hw_crc32w(crc, load_le32(p));
    p += 4;
    len -= 4;
  }
  while (len != 0) {
    crc = hw_crc32b(crc, *p++);
    --len;
  }
  return crc;
}

bool arm_crc32_usable() noexcept {
#if defined(CYCLONE_CRC32_ARM_ALWAYS)
  return true;
#else
  return (getauxval(AT_HWCAP) & HWCAP_CRC32) != 0;
#endif
}

#endif  // ARM hardware path compiled in

using Crc32Impl = uint32_t (*)(uint32_t, const uint8_t*, size_t) noexcept;

Crc32Impl select_impl() noexcept {
#if defined(CYCLONE_CRC32_ARM_ALWAYS) || defined(CYCLONE_CRC32_ARM_RUNTIME)
  if (arm_crc32_usable()) {
    return &arm_crc32_impl;
  }
#endif
  return &slice_by_16_impl;
}

// Resolved once, before main(); the hot loop below sees a plain indirect call
// and no feature branches.
const Crc32Impl kCrc32Impl = select_impl();

inline uint32_t run(Crc32Impl impl, uint32_t state,
                    std::span<const std::byte> data) noexcept {
  const uint32_t crc =
      impl(~state, reinterpret_cast<const uint8_t*>(data.data()), data.size());
  return ~crc;
}

}  // namespace

uint32_t crc32_update(uint32_t state,
                      std::span<const std::byte> data) noexcept {
  return run(kCrc32Impl, state, data);
}

uint32_t crc32(std::span<const std::byte> data) noexcept {
  return crc32_update(0, data);
}

uint32_t crc32_update_portable(uint32_t state,
                               std::span<const std::byte> data) noexcept {
  return run(&slice_by_16_impl, state, data);
}

uint32_t crc32_update_hardware(uint32_t state,
                               std::span<const std::byte> data) noexcept {
#if defined(CYCLONE_CRC32_ARM_ALWAYS) || defined(CYCLONE_CRC32_ARM_RUNTIME)
  if (arm_crc32_usable()) {
    return run(&arm_crc32_impl, state, data);
  }
#endif
  return run(&slice_by_16_impl, state, data);
}

bool crc32_has_hardware() noexcept {
#if defined(CYCLONE_CRC32_ARM_ALWAYS) || defined(CYCLONE_CRC32_ARM_RUNTIME)
  return arm_crc32_usable();
#else
  return false;
#endif
}

const char* crc32_impl_name() noexcept {
  return kCrc32Impl == &slice_by_16_impl ? "slice-by-16" : "armv8-crc32";
}

}  // namespace cyclone
