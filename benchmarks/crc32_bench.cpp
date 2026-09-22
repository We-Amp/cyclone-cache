// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Micro-benchmark for the document checksum (src/core/crc32.*).
//
// Every first read of a document, and every read after a restart or from a
// second process, re-verifies the whole payload, so this number is a hard
// ceiling on cold read bandwidth.  Usage:
//
//   ./build-rel/crc32_bench [--seconds 0.3]

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <span>
#include <string>
#include <vector>

#include "core/crc32.hpp"

namespace {

using Clock = std::chrono::steady_clock;

// The byte-at-a-time table CRC32 Cyclone shipped before slice-by-16, kept here
// as the "before" row so the speedup is measured on one machine at one time.
constexpr uint32_t make_crc_table_entry(uint32_t n) {
  uint32_t c = n;
  for (int k = 0; k < 8; k++) {
    c = ((c & 1) != 0u) ? (0xedb88320 ^ (c >> 1)) : (c >> 1);
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

uint32_t crc32_bytewise(uint32_t state, std::span<const std::byte> data) {
  uint32_t crc = ~state;
  for (std::byte b : data) {
    crc = kCrcTable[(crc ^ static_cast<uint8_t>(b)) & 0xFF] ^ (crc >> 8);
  }
  return ~crc;
}

using Fn = uint32_t (*)(uint32_t, std::span<const std::byte>);

uint32_t call_portable(uint32_t s, std::span<const std::byte> d) {
  return cyclone::crc32_update_portable(s, d);
}

uint32_t call_hardware(uint32_t s, std::span<const std::byte> d) {
  return cyclone::crc32_update_hardware(s, d);
}

uint32_t call_dispatch(uint32_t s, std::span<const std::byte> d) {
  return cyclone::crc32_update(s, d);
}

struct Impl {
  const char* name;
  Fn fn;
};

double run(Fn fn, std::span<const std::byte> buf, double seconds,
           uint32_t* sink) {
  // Warm up and settle on an iteration count that keeps the timing loop
  // above the clock's resolution.
  uint32_t acc = 0;
  size_t iters = 1;
  for (;;) {
    const auto t0 = Clock::now();
    for (size_t i = 0; i < iters; ++i) {
      acc ^= fn(0, buf);
    }
    const double elapsed =
        std::chrono::duration<double>(Clock::now() - t0).count();
    if (elapsed >= seconds) {
      *sink ^= acc;
      return static_cast<double>(iters) * static_cast<double>(buf.size()) /
             elapsed / 1e9;
    }
    const size_t next =
        elapsed > 0 ? static_cast<size_t>(static_cast<double>(iters) *
                                          std::max(2.0, seconds / elapsed))
                    : iters * 8;
    iters = std::max(iters + 1, next);
  }
}

}  // namespace

int main(int argc, char** argv) {
  double seconds = 0.3;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--seconds" && i + 1 < argc) {
      seconds = std::atof(argv[++i]);
    } else {
      std::fprintf(stderr, "usage: %s [--seconds N]\n", argv[0]);
      return 2;
    }
  }

  std::vector<Impl> impls;
  impls.push_back({"bytewise (old)", &crc32_bytewise});
  impls.push_back({"slice-by-16", &call_portable});
  if (cyclone::crc32_has_hardware()) {
    impls.push_back({"armv8-crc32", &call_hardware});
  }
  impls.push_back({"dispatch", &call_dispatch});

  const size_t sizes[] = {4096, 64 * 1024, 2 * 1024 * 1024};

  std::printf("crc32 micro-benchmark  (dispatch selected: %s)\n\n",
              cyclone::crc32_impl_name());
  std::printf("%-16s %12s %12s %12s\n", "impl", "4 KiB", "64 KiB", "2 MiB");
  std::printf("%-16s %12s %12s %12s\n", "", "GB/s", "GB/s", "GB/s");

  std::mt19937 rng(12345);
  std::vector<std::byte> buf(sizes[2]);
  for (auto& b : buf) {
    b = static_cast<std::byte>(rng() & 0xFFu);
  }

  // Sanity: every implementation must agree before any number is printed.
  const uint32_t want = crc32_bytewise(0, buf);
  for (const auto& impl : impls) {
    if (impl.fn(0, buf) != want) {
      std::fprintf(stderr, "MISMATCH in %s\n", impl.name);
      return 1;
    }
  }

  uint32_t sink = 0;
  for (const auto& impl : impls) {
    std::printf("%-16s", impl.name);
    for (size_t size : sizes) {
      const double gbps = run(
          impl.fn, std::span<const std::byte>(buf).first(size), seconds, &sink);
      std::printf(" %12.2f", gbps);
      std::fflush(stdout);
    }
    std::printf("\n");
  }
  std::printf("\nchecksum sink: %08x\n", sink);
  return 0;
}
