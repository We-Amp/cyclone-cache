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

#include "../../src/core/crc32.hpp"
#include "../../src/core/document.hpp"

using namespace cyclone;

namespace {

// REFERENCE ORACLE -- a verbatim copy of the byte-at-a-time table CRC32 that
// lived in src/core/document.cpp before the slice-by-16 / ARMv8 rewrite.  It
// defines the on-disk checksum, so every fast path must agree with it
// bit-for-bit or previously written cache files stop verifying.  Do not
// "simplify" it and do not replace it with a call into crc32.hpp: the whole
// point is that it is an independent implementation.
constexpr uint32_t make_crc_table_entry(uint32_t n) {
  uint32_t c = n;
  for (int k = 0; k < 8; k++) {
    if ((c & 1) != 0u) {
      c = 0xedb88320 ^ (c >> 1);
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

uint32_t reference_crc32(std::span<const std::byte> data) {
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

// Every implementation the build offers, checked against the oracle.
void check_all_impls(std::span<const std::byte> data) {
  const uint32_t expected = reference_crc32(data);
  REQUIRE(crc32(data) == expected);
  REQUIRE(crc32_update(0, data) == expected);
  REQUIRE(crc32_update_portable(0, data) == expected);
  REQUIRE(crc32_update_hardware(0, data) == expected);
  REQUIRE(Document::compute_checksum(data) == expected);
}

}  // namespace

TEST_CASE("CRC32 known-answer vectors", "[crc32][document]") {
  // The convention baked into the on-disk format: reflected IEEE polynomial
  // 0xEDB88320, init 0xFFFFFFFF, xorout 0xFFFFFFFF (CRC-32/ISO-HDLC).
  REQUIRE(reference_crc32({}) == 0x00000000u);
  REQUIRE(crc32({}) == 0x00000000u);

  REQUIRE(reference_crc32(as_bytes("123456789")) == 0xCBF43926u);
  REQUIRE(crc32(as_bytes("123456789")) == 0xCBF43926u);

  REQUIRE(crc32(as_bytes("a")) == 0xE8B7BE43u);
  REQUIRE(crc32(as_bytes("abc")) == 0x352441C2u);
  REQUIRE(crc32(as_bytes("The quick brown fox jumps over the lazy dog")) ==
          0x414FA339u);

  // ...and the same values through every path that exists on this build.
  check_all_impls({});
  check_all_impls(as_bytes("123456789"));
  check_all_impls(as_bytes("The quick brown fox jumps over the lazy dog"));
}

TEST_CASE("CRC32 matches the old byte-wise code for 0..64 byte inputs",
          "[crc32][document]") {
  const auto buf = random_bytes(64, 0xC0FFEEu);
  for (size_t len = 0; len <= buf.size(); ++len) {
    INFO("len=" << len);
    check_all_impls(std::span<const std::byte>(buf).first(len));
  }
}

TEST_CASE("CRC32 matches the old byte-wise code on a 1 MiB buffer",
          "[crc32][document]") {
  const auto buf = random_bytes(1024 * 1024, 0x5EEDu);
  check_all_impls(buf);
}

TEST_CASE("CRC32 handles unaligned heads and tails", "[crc32][document]") {
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

TEST_CASE("CRC32 incremental equals one-shot", "[crc32][document]") {
  const auto buf = random_bytes(70000, 0x1234u);
  const uint32_t expected = reference_crc32(buf);

  for (size_t split : {size_t{0}, size_t{1}, size_t{7}, size_t{8}, size_t{63},
                       size_t{4096}, size_t{65535}, buf.size()}) {
    INFO("split=" << split);
    const std::span<const std::byte> all(buf);
    uint32_t state = crc32_update(0, all.first(split));
    state = crc32_update(state, all.subspan(split));
    REQUIRE(state == expected);
  }

  // Many small chunks of an awkward size.
  uint32_t state = 0;
  size_t pos = 0;
  const std::span<const std::byte> all(buf);
  while (pos < buf.size()) {
    const size_t n = std::min<size_t>(37, buf.size() - pos);
    state = crc32_update(state, all.subspan(pos, n));
    pos += n;
  }
  REQUIRE(state == expected);
}

TEST_CASE("CRC32 runtime dispatch reports a sane implementation", "[crc32]") {
  const std::string_view name = crc32_impl_name();
  REQUIRE((name == "slice-by-16" || name == "armv8-crc32"));
  if (crc32_has_hardware()) {
    REQUIRE(name == "armv8-crc32");
  } else {
    REQUIRE(name == "slice-by-16");
  }
}
