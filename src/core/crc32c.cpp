// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "crc32c.hpp"

#include <array>
#include <bit>
#include <cstring>

// --- Platform selection ----------------------------------------------------
//
// CRC-32C is the polynomial both mainstream server architectures implement in
// hardware, which is the whole reason the on-disk format moved to it at v8:
//
//   * x86-64, SSE4.2: the `crc32` instruction (_mm_crc32_u64/_mm_crc32_u8).
//     The project's stock flags do NOT include -msse4.2 -- downstream
//     consumers (Bazel, vendored copies) must keep building with unchanged
//     flags -- so on GCC/Clang only this one function carries
//     __attribute__((target("sse4.2"))) and the path is selected at run time
//     from __builtin_cpu_supports("sse4.2").  MSVC needs neither: its
//     intrinsics are always available, and the runtime probe is __cpuid leaf
//     1, ECX bit 20.
//   * ARMv8: the `crc32cb/crc32cw/crc32cx` instruction group.  Two flavours,
//     as before: when the toolchain already targets a CPU with the extension
//     (__ARM_FEATURE_CRC32 -- every Apple silicon build, Windows on ARM, any
//     -march=armv8-a+crc build) we use the <arm_acle.h> intrinsics directly;
//     on a baseline armv8-a Linux build the ACLE header does not expose them,
//     so the instructions are emitted with inline asm under
//     `.arch_extension crc` (the trick the kernel and zlib-ng use) and the
//     path is selected from AT_HWCAP & HWCAP_CRC32.  Either way everything
//     stays in one translation unit built with the project's stock flags.
//
// Everything else runs the portable slice-by-16 table path.

#if defined(__x86_64__) || defined(_M_X64)
#define CYCLONE_CRC32C_X86 1
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
#define CYCLONE_CRC32C_ARM 1
#endif

#if defined(CYCLONE_CRC32C_X86)
#include <nmmintrin.h>
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
// MSVC has no per-function target attribute; the intrinsics are always
// emittable and the runtime probe below decides whether they are used.
#define CYCLONE_CRC32C_SSE42_TARGET
#else
#define CYCLONE_CRC32C_SSE42_TARGET __attribute__((target("sse4.2")))
#endif
#endif

#if defined(CYCLONE_CRC32C_ARM)
#if defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#define CYCLONE_CRC32C_ARM_ALWAYS 1
#elif defined(__linux__) && defined(__GNUC__)
#include <sys/auxv.h>
#define CYCLONE_CRC32C_ARM_RUNTIME 1
#ifndef HWCAP_CRC32
#define HWCAP_CRC32 (1U << 7)
#endif
#endif
#endif

#if defined(CYCLONE_CRC32C_ARM_ALWAYS) || defined(CYCLONE_CRC32C_ARM_RUNTIME)
#define CYCLONE_CRC32C_ARM_HW 1
#endif

namespace cyclone {

namespace {

// Reflected form of the Castagnoli polynomial 0x1EDC6F41.
constexpr uint32_t kPolyReflected = 0x82F63B78U;

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

// --- GF(2) "advance by N zero bytes" operators -----------------------------
//
// The hardware paths run three independent CRC registers over three adjacent
// blocks of L bytes each, because the crc32 instruction has ~3 cycles of
// latency at 1/cycle throughput: one dependent chain leaves two thirds of the
// issue slots idle.  Stitching the three back together uses the standard
// linearity argument.  Writing R(r, D) for "register r advanced over data D"
// (no init/xorout -- that is applied only at the API boundary):
//
//   R(r, D) = Z_|D|(r) ^ R(0, D)
//
// where Z_n is the linear map "advance the register over n zero bytes".  So
// with crc0 = R(r, A), crc1 = R(0, B), crc2 = R(0, C) for equal-length
// A, B, C, the CRC of A++B++C from r is Z_L(Z_L(crc0) ^ crc1) ^ crc2.
//
// Z_n is represented as a 32x32 GF(2) matrix (one column per register bit)
// and applied a byte at a time through a 4x256 table, exactly as in Mark
// Adler's crc32c.c.  Both the matrices and the tables are generated at
// compile time from the polynomial -- nothing here is pasted magic.

using Gf2Matrix = std::array<uint32_t, 32>;

// Apply a matrix to a register value: XOR the columns the set bits select.
constexpr uint32_t gf2_apply(const Gf2Matrix& mat, uint32_t vec) {
  uint32_t sum = 0;
  for (size_t n = 0; n < 32 && vec != 0; ++n, vec >>= 1) {
    if ((vec & 1U) != 0U) {
      sum ^= mat[n];
    }
  }
  return sum;
}

// Composition "apply b, then a".  All the matrices here are powers of one
// generator, so they commute and the order is immaterial.
constexpr Gf2Matrix gf2_compose(const Gf2Matrix& a, const Gf2Matrix& b) {
  Gf2Matrix out{};
  for (size_t n = 0; n < 32; ++n) {
    out[n] = gf2_apply(a, b[n]);
  }
  return out;
}

// Advance the reflected register over ONE zero bit:
//   r' = (r >> 1) ^ (poly if r & 1)
// Column 0 (register bit 0 set) is therefore the polynomial; column n > 0 is
// 1 << (n - 1).
constexpr Gf2Matrix zero_bit_operator() {
  Gf2Matrix m{};
  m[0] = kPolyReflected;
  uint32_t row = 1;
  for (size_t n = 1; n < 32; ++n) {
    m[n] = row;
    row <<= 1;
  }
  return m;
}

// Z_bytes, by binary exponentiation over composition.
constexpr Gf2Matrix zero_bytes_operator(size_t bytes) {
  Gf2Matrix result{};
  for (size_t n = 0; n < 32; ++n) {
    result[n] = uint32_t{1} << n;  // identity
  }
  Gf2Matrix base = zero_bit_operator();
  for (size_t bits = bytes * 8; bits != 0; bits >>= 1) {
    if ((bits & 1U) != 0U) {
      result = gf2_compose(base, result);
    }
    base = gf2_compose(base, base);
  }
  return result;
}

using ShiftTable = std::array<std::array<uint32_t, 256>, 4>;

constexpr ShiftTable make_shift_table(size_t bytes) {
  const Gf2Matrix op = zero_bytes_operator(bytes);
  ShiftTable t{};
  for (uint32_t n = 0; n < 256; ++n) {
    t[0][n] = gf2_apply(op, n);
    t[1][n] = gf2_apply(op, n << 8);
    t[2][n] = gf2_apply(op, n << 16);
    t[3][n] = gf2_apply(op, n << 24);
  }
  return t;
}

// Two block sizes, as in crc32c.c: the long block amortizes the combine over
// 24 KiB, the short one keeps the interleave paying off down to ~768 bytes.
constexpr size_t kLong = kCrc32cLongBlock;
constexpr size_t kShort = kCrc32cShortBlock;

constexpr ShiftTable kShiftLong = make_shift_table(kLong);
constexpr ShiftTable kShiftShort = make_shift_table(kShort);

inline uint32_t shift_crc(const ShiftTable& t, uint32_t crc) noexcept {
  return t[0][crc & 0xFFU] ^ t[1][(crc >> 8) & 0xFFU] ^
         t[2][(crc >> 16) & 0xFFU] ^ t[3][(crc >> 24) & 0xFFU];
}

// All the *_impl functions below take and return the INTERNAL (complemented)
// CRC register; crc32c_update() applies the 0xFFFFFFFF init/xorout around
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

// --- x86-64 SSE4.2 ---------------------------------------------------------

#if defined(CYCLONE_CRC32C_X86)

bool sse42_usable() noexcept {
#if defined(_MSC_VER) && !defined(__clang__)
  int regs[4] = {0, 0, 0, 0};
  __cpuid(regs, 1);
  return (static_cast<unsigned int>(regs[2]) & (1U << 20)) != 0U;
#else
  return __builtin_cpu_supports("sse4.2") != 0;
#endif
}

// The interleave and the tails live inside the target-attributed functions on
// purpose: GCC will not inline a plain helper's `crc32` instruction into an
// sse4.2 caller (and would refuse to compile the helper at all), so the loops
// cannot be factored out behind the attribute boundary.

CYCLONE_CRC32C_SSE42_TARGET uint32_t sse42_tail(uint32_t crc, const uint8_t* p,
                                                size_t len) noexcept {
  while (len >= 8) {
    crc = static_cast<uint32_t>(_mm_crc32_u64(crc, load_le64(p)));
    p += 8;
    len -= 8;
  }
  while (len != 0) {
    crc = _mm_crc32_u8(crc, *p++);
    --len;
  }
  return crc;
}

CYCLONE_CRC32C_SSE42_TARGET uint32_t sse42_1way_impl(uint32_t crc,
                                                     const uint8_t* p,
                                                     size_t len) noexcept {
  while (len != 0 && (reinterpret_cast<uintptr_t>(p) & 7U) != 0U) {
    crc = _mm_crc32_u8(crc, *p++);
    --len;
  }
  return sse42_tail(crc, p, len);
}

CYCLONE_CRC32C_SSE42_TARGET uint32_t sse42_3way_impl(uint32_t crc,
                                                     const uint8_t* p,
                                                     size_t len) noexcept {
  while (len != 0 && (reinterpret_cast<uintptr_t>(p) & 7U) != 0U) {
    crc = _mm_crc32_u8(crc, *p++);
    --len;
  }
  while (len >= 3 * kLong) {
    uint32_t c0 = crc;
    uint32_t c1 = 0;
    uint32_t c2 = 0;
    const uint8_t* q = p;
    for (size_t i = 0; i < kLong / 8; ++i) {
      c0 = static_cast<uint32_t>(_mm_crc32_u64(c0, load_le64(q)));
      c1 = static_cast<uint32_t>(_mm_crc32_u64(c1, load_le64(q + kLong)));
      c2 = static_cast<uint32_t>(_mm_crc32_u64(c2, load_le64(q + 2 * kLong)));
      q += 8;
    }
    crc = shift_crc(kShiftLong, c0) ^ c1;
    crc = shift_crc(kShiftLong, crc) ^ c2;
    p += 3 * kLong;
    len -= 3 * kLong;
  }
  while (len >= 3 * kShort) {
    uint32_t c0 = crc;
    uint32_t c1 = 0;
    uint32_t c2 = 0;
    const uint8_t* q = p;
    for (size_t i = 0; i < kShort / 8; ++i) {
      c0 = static_cast<uint32_t>(_mm_crc32_u64(c0, load_le64(q)));
      c1 = static_cast<uint32_t>(_mm_crc32_u64(c1, load_le64(q + kShort)));
      c2 = static_cast<uint32_t>(_mm_crc32_u64(c2, load_le64(q + 2 * kShort)));
      q += 8;
    }
    crc = shift_crc(kShiftShort, c0) ^ c1;
    crc = shift_crc(kShiftShort, crc) ^ c2;
    p += 3 * kShort;
    len -= 3 * kShort;
  }
  return sse42_tail(crc, p, len);
}

#endif  // CYCLONE_CRC32C_X86

// --- ARMv8 crc32c* ---------------------------------------------------------

#if defined(CYCLONE_CRC32C_ARM_HW)

#if defined(CYCLONE_CRC32C_ARM_ALWAYS)
inline uint32_t hw_crc32cb(uint32_t crc, uint8_t v) noexcept {
  return __crc32cb(crc, v);
}
inline uint32_t hw_crc32cd(uint32_t crc, uint64_t v) noexcept {
  return __crc32cd(crc, v);
}
#else
// Baseline armv8-a: same instructions, spelled so the assembler will accept
// them without the extension in -march.
inline uint32_t hw_crc32cb(uint32_t crc, uint8_t v) noexcept {
  __asm__(".arch_extension crc\ncrc32cb %w0, %w0, %w1"
          : "+r"(crc)
          : "r"(static_cast<uint32_t>(v)));
  return crc;
}
inline uint32_t hw_crc32cd(uint32_t crc, uint64_t v) noexcept {
  __asm__(".arch_extension crc\ncrc32cx %w0, %w0, %x1" : "+r"(crc) : "r"(v));
  return crc;
}
#endif

uint32_t arm_tail(uint32_t crc, const uint8_t* p, size_t len) noexcept {
  while (len >= 8) {
    crc = hw_crc32cd(crc, load_le64(p));
    p += 8;
    len -= 8;
  }
  while (len != 0) {
    crc = hw_crc32cb(crc, *p++);
    --len;
  }
  return crc;
}

uint32_t arm_1way_impl(uint32_t crc, const uint8_t* p, size_t len) noexcept {
  while (len != 0 && (reinterpret_cast<uintptr_t>(p) & 7U) != 0U) {
    crc = hw_crc32cb(crc, *p++);
    --len;
  }
  return arm_tail(crc, p, len);
}

uint32_t arm_3way_impl(uint32_t crc, const uint8_t* p, size_t len) noexcept {
  while (len != 0 && (reinterpret_cast<uintptr_t>(p) & 7U) != 0U) {
    crc = hw_crc32cb(crc, *p++);
    --len;
  }
  while (len >= 3 * kLong) {
    uint32_t c0 = crc;
    uint32_t c1 = 0;
    uint32_t c2 = 0;
    const uint8_t* q = p;
    for (size_t i = 0; i < kLong / 8; ++i) {
      c0 = hw_crc32cd(c0, load_le64(q));
      c1 = hw_crc32cd(c1, load_le64(q + kLong));
      c2 = hw_crc32cd(c2, load_le64(q + 2 * kLong));
      q += 8;
    }
    crc = shift_crc(kShiftLong, c0) ^ c1;
    crc = shift_crc(kShiftLong, crc) ^ c2;
    p += 3 * kLong;
    len -= 3 * kLong;
  }
  while (len >= 3 * kShort) {
    uint32_t c0 = crc;
    uint32_t c1 = 0;
    uint32_t c2 = 0;
    const uint8_t* q = p;
    for (size_t i = 0; i < kShort / 8; ++i) {
      c0 = hw_crc32cd(c0, load_le64(q));
      c1 = hw_crc32cd(c1, load_le64(q + kShort));
      c2 = hw_crc32cd(c2, load_le64(q + 2 * kShort));
      q += 8;
    }
    crc = shift_crc(kShiftShort, c0) ^ c1;
    crc = shift_crc(kShiftShort, crc) ^ c2;
    p += 3 * kShort;
    len -= 3 * kShort;
  }
  return arm_tail(crc, p, len);
}

bool arm_crc32c_usable() noexcept {
#if defined(CYCLONE_CRC32C_ARM_ALWAYS)
  return true;
#else
  return (getauxval(AT_HWCAP) & HWCAP_CRC32) != 0;
#endif
}

#endif  // CYCLONE_CRC32C_ARM_HW

using Crc32cImpl = uint32_t (*)(uint32_t, const uint8_t*, size_t) noexcept;

bool hardware_usable() noexcept {
#if defined(CYCLONE_CRC32C_X86)
  return sse42_usable();
#elif defined(CYCLONE_CRC32C_ARM_HW)
  return arm_crc32c_usable();
#else
  return false;
#endif
}

// The interleaved variant is what dispatch picks on both architectures.  On
// an Apple M5 it is 2.4-2.8x the single-chain path (12.0 -> 34.0 GB/s over
// 2 MiB, 12.5 -> 28.9 GB/s over 4 KiB), and it is never slower: an input
// below 3*kShort takes the same single-chain tail either way.  Compare the
// two with `crc32c_bench`, which times both rows.
Crc32cImpl hardware_impl() noexcept {
#if defined(CYCLONE_CRC32C_X86)
  return &sse42_3way_impl;
#elif defined(CYCLONE_CRC32C_ARM_HW)
  return &arm_3way_impl;
#else
  return &slice_by_16_impl;
#endif
}

Crc32cImpl hardware_impl_1way() noexcept {
#if defined(CYCLONE_CRC32C_X86)
  return &sse42_1way_impl;
#elif defined(CYCLONE_CRC32C_ARM_HW)
  return &arm_1way_impl;
#else
  return &slice_by_16_impl;
#endif
}

Crc32cImpl select_impl() noexcept {
  if (hardware_usable()) {
    return hardware_impl();
  }
  return &slice_by_16_impl;
}

// Resolved once, before main(); the hot loop below sees a plain indirect call
// and no feature branches.
const Crc32cImpl kCrc32cImpl = select_impl();

inline uint32_t run(Crc32cImpl impl, uint32_t state,
                    std::span<const std::byte> data) noexcept {
  const uint32_t crc =
      impl(~state, reinterpret_cast<const uint8_t*>(data.data()), data.size());
  return ~crc;
}

}  // namespace

uint32_t crc32c_update(uint32_t state,
                       std::span<const std::byte> data) noexcept {
  return run(kCrc32cImpl, state, data);
}

uint32_t crc32c(std::span<const std::byte> data) noexcept {
  return crc32c_update(0, data);
}

uint32_t crc32c_update_portable(uint32_t state,
                                std::span<const std::byte> data) noexcept {
  return run(&slice_by_16_impl, state, data);
}

uint32_t crc32c_update_hardware(uint32_t state,
                                std::span<const std::byte> data) noexcept {
  return run(hardware_usable() ? hardware_impl() : &slice_by_16_impl, state,
             data);
}

uint32_t crc32c_update_hardware_1way(uint32_t state,
                                     std::span<const std::byte> data) noexcept {
  return run(hardware_usable() ? hardware_impl_1way() : &slice_by_16_impl,
             state, data);
}

bool crc32c_has_hardware() noexcept { return hardware_usable(); }

const char* crc32c_impl_name() noexcept {
  if (kCrc32cImpl == &slice_by_16_impl) {
    return "slice-by-16";
  }
#if defined(CYCLONE_CRC32C_X86)
  return "sse4.2-crc32c";
#else
  return "armv8-crc32c";
#endif
}

}  // namespace cyclone
