// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <random>
#include <span>
#include <string_view>
#include <vector>

#include "../../src/core/crc32c.hpp"
#include "../../src/core/document.hpp"

using namespace cyclone;

namespace {

// REFERENCE ORACLE -- an independent byte-at-a-time table CRC-32C, written
// from the polynomial rather than derived from anything in src/core.  It
// defines the on-disk checksum of format v8, so every fast path (slice-by-16,
// SSE4.2, ARMv8, one-chain and 3-way interleaved) must agree with it
// bit-for-bit.  Do not "simplify" it and do not replace it with a call into
// crc32c.hpp: the whole point is that it is an independent implementation.
constexpr uint32_t make_crc_table_entry(uint32_t n) {
  uint32_t c = n;
  for (int k = 0; k < 8; k++) {
    if ((c & 1) != 0u) {
      c = 0x82f63b78 ^ (c >> 1);  // reflected 0x1EDC6F41 (Castagnoli)
    } else {
      c = c >> 1;
    }
  }
  return c;
}

constexpr std::array<uint32_t, 256> make_crc_table() {
  std::array<uint32_t, 256> table{};
  for (uint32_t n = 0; n < 256; n++) {
    table[n] = make_crc_table_entry(n);
  }
  return table;
}

constexpr auto kCrcTable = make_crc_table();

uint32_t reference_crc32c(std::span<const std::byte> data) {
  uint32_t crc = 0xFFFFFFFF;
  for (std::byte b : data) {
    crc = kCrcTable[(crc ^ static_cast<uint8_t>(b)) & 0xFF] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFF;
}

std::span<const std::byte> as_bytes(std::string_view s) {
  return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

std::vector<std::byte> random_bytes(size_t n, uint32_t seed) {
  std::mt19937 rng(seed);
  std::vector<std::byte> v(n);
  for (size_t i = 0; i < n; ++i) {
    v[i] = static_cast<std::byte>(rng() & 0xFFu);
  }
  return v;
}

std::vector<std::byte> repeated(size_t n, uint8_t value) {
  return {n, static_cast<std::byte>(value)};
}

// Every implementation the build offers, checked against the oracle.
void check_all_impls(std::span<const std::byte> data) {
  const uint32_t expected = reference_crc32c(data);
  REQUIRE(crc32c(data) == expected);
  REQUIRE(crc32c_update(0, data) == expected);
  REQUIRE(crc32c_update_portable(0, data) == expected);
  REQUIRE(crc32c_update_hardware(0, data) == expected);
  REQUIRE(crc32c_update_hardware_1way(0, data) == expected);
  REQUIRE(Document::compute_checksum(data) == expected);
}

}  // namespace

TEST_CASE("CRC-32C known-answer vectors", "[crc32c][document]") {
  // The convention baked into the v8 on-disk format: reflected Castagnoli
  // polynomial 0x82F63B78, init 0xFFFFFFFF, xorout 0xFFFFFFFF.
  REQUIRE(reference_crc32c({}) == 0x00000000u);
  REQUIRE(crc32c({}) == 0x00000000u);

  // The CRC catalogue's check value.
  REQUIRE(reference_crc32c(as_bytes("123456789")) == 0xE3069283u);
  REQUIRE(crc32c(as_bytes("123456789")) == 0xE3069283u);

  // RFC 3720 appendix B.4 (iSCSI CRC-32C test vectors).
  REQUIRE(crc32c(repeated(32, 0x00)) == 0x8A9136AAu);
  REQUIRE(crc32c(repeated(32, 0xFF)) == 0x62A8AB43u);
  {
    std::vector<std::byte> ascending(32);
    std::vector<std::byte> descending(32);
    for (size_t i = 0; i < 32; ++i) {
      ascending[i] = static_cast<std::byte>(i);
      descending[i] = static_cast<std::byte>(31 - i);
    }
    REQUIRE(crc32c(ascending) == 0x46DD794Eu);
    REQUIRE(crc32c(descending) == 0x113FDB5Cu);
    check_all_impls(ascending);
    check_all_impls(descending);
  }

  // ...and the same values through every path that exists on this build.
  check_all_impls({});
  check_all_impls(as_bytes("123456789"));
  check_all_impls(repeated(32, 0x00));
  check_all_impls(repeated(32, 0xFF));
  check_all_impls(as_bytes("The quick brown fox jumps over the lazy dog"));
}

TEST_CASE("CRC-32C matches the byte-wise oracle for 0..300 byte inputs",
          "[crc32c][document]") {
  const auto buf = random_bytes(300, 0xC0FFEEu);
  for (size_t len = 0; len <= buf.size(); ++len) {
    INFO("len=" << len);
    check_all_impls(std::span<const std::byte>(buf).first(len));
  }
}

TEST_CASE(
    "CRC-32C matches the byte-wise oracle across the interleave "
    "block boundaries",
    "[crc32c][document]") {
  // The hardware paths switch strategy at 3*kCrc32cLongBlock and
  // 3*kCrc32cShortBlock; the combine step is only exercised at or above those
  // sizes, so probe each boundary from both sides, at every misalignment.
  const size_t s3 = 3 * kCrc32cShortBlock;
  const size_t l3 = 3 * kCrc32cLongBlock;
  const std::vector<size_t> lengths = {
      s3 - 1,      s3,          s3 + 1,
      2 * s3 + 17, l3 - 1,      l3,
      l3 + 1,      2 * l3 + 17, size_t{8} * 1024 * 1024};

  const size_t max_len = *std::max_element(lengths.begin(), lengths.end());
  const auto buf = random_bytes(max_len + 8, 0x5EEDu);

  for (size_t len : lengths) {
    for (size_t misalign = 0; misalign < 8; ++misalign) {
      INFO("len=" << len << " misalign=" << misalign);
      check_all_impls(std::span<const std::byte>(buf).subspan(misalign, len));
    }
  }
}

TEST_CASE("CRC-32C handles unaligned heads and tails", "[crc32c][document]") {
  // Slice both ends so the fast loops see every combination of unaligned
  // start address and ragged tail length.
  const auto buf = random_bytes(4096 + 32, 0xA11A5u);
  for (size_t head = 0; head < 16; ++head) {
    for (size_t tail :
         {size_t{0}, size_t{1}, size_t{3}, size_t{7}, size_t{13}, size_t{16}}) {
      const size_t len = buf.size() - head - tail;
      INFO("head=" << head << " tail=" << tail);
      check_all_impls(std::span<const std::byte>(buf).subspan(head, len));
    }
  }
}

TEST_CASE("CRC-32C incremental equals one-shot", "[crc32c][document]") {
  const auto buf = random_bytes(70000, 0x1234u);
  const uint32_t expected = reference_crc32c(buf);

  for (size_t split :
       {size_t{0}, size_t{1}, size_t{7}, size_t{8}, size_t{63}, size_t{768},
        size_t{4096}, size_t{24576}, size_t{65535}, buf.size()}) {
    INFO("split=" << split);
    const std::span<const std::byte> all(buf);
    uint32_t state = crc32c_update(0, all.first(split));
    state = crc32c_update(state, all.subspan(split));
    REQUIRE(state == expected);

    // Not just whatever dispatch picked: each implementation must carry the
    // running state across a chunk boundary on its own.
    uint32_t portable = crc32c_update_portable(0, all.first(split));
    portable = crc32c_update_portable(portable, all.subspan(split));
    REQUIRE(portable == expected);

    uint32_t hw = crc32c_update_hardware(0, all.first(split));
    hw = crc32c_update_hardware(hw, all.subspan(split));
    REQUIRE(hw == expected);

    uint32_t hw1 = crc32c_update_hardware_1way(0, all.first(split));
    hw1 = crc32c_update_hardware_1way(hw1, all.subspan(split));
    REQUIRE(hw1 == expected);
  }

  // Random split points, so the chunk boundaries do not all land on the
  // sizes a human would have picked.
  std::mt19937 rng(0xBEEFu);
  for (int trial = 0; trial < 64; ++trial) {
    const size_t split = rng() % (buf.size() + 1);
    INFO("random split=" << split);
    const std::span<const std::byte> all(buf);
    uint32_t state = crc32c_update(0, all.first(split));
    state = crc32c_update(state, all.subspan(split));
    REQUIRE(state == expected);
  }

  // Many small chunks of an awkward size.
  uint32_t state = 0;
  size_t pos = 0;
  const std::span<const std::byte> all(buf);
  while (pos < buf.size()) {
    const size_t n = std::min<size_t>(37, buf.size() - pos);
    state = crc32c_update(state, all.subspan(pos, n));
    pos += n;
  }
  REQUIRE(state == expected);
}

TEST_CASE("CRC-32C runtime dispatch reports a sane implementation",
          "[crc32c]") {
  const std::string_view name = crc32c_impl_name();
  REQUIRE((name == "slice-by-16" || name == "sse4.2-crc32c" ||
           name == "armv8-crc32c"));
  if (crc32c_has_hardware()) {
    REQUIRE(name != "slice-by-16");
  } else {
    REQUIRE(name == "slice-by-16");
  }
}
