// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Small cold reads (issue #29): the write-then-first-read pattern (PageSpeed
// writes an optimized alternate and serves it on the next request).  Puts N
// documents; after each put, reads back a document written K puts earlier,
// so every read is the first, CRC-pending read of a recently written,
// resident document.  Prints the read rate and the readahead counters.
//
//   write_then_read DIR SIZE N K [cold_readahead_min_bytes]
//
// Build (from a configured build tree):
//   c++ -O2 -std=c++23 -I include -I src write_then_read.cpp \
//     build/libcyclone-cache.a -lpthread -o write_then_read
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "cyclone/cache.hpp"
#include "cyclone/config.hpp"
#include "cyclone/key.hpp"

using namespace cyclone;

int main(int argc, char** argv) {
  if (argc < 5) {
    std::fprintf(stderr, "usage: %s DIR SIZE N K [cold_min]\n", argv[0]);
    return 2;
  }
  const std::string dir = argv[1];
  const size_t size = std::strtoull(argv[2], nullptr, 10);
  const size_t n = std::strtoull(argv[3], nullptr, 10);
  const size_t lag = std::strtoull(argv[4], nullptr, 10);
  CacheConfig cfg;
  cfg.ram_cache_size = 0;
  cfg.enable_hit_tracking = false;
  cfg.optimization_config.enabled = false;
  if (argc > 5) {
    cfg.cold_readahead_min_bytes = std::strtoull(argv[5], nullptr, 10);
  }
  auto cache = Cache::create(cfg);
  if (!cache) return 1;
  VolumeConfig vc;
  vc.path = dir;
  vc.size = size_t{1} << 30;
  if (!(*cache)->add_volume(vc) || !(*cache)->start()) return 1;

  std::vector<std::byte> buf(size, std::byte{0x5a});
  std::vector<CacheKey> keys;
  keys.reserve(n);
  double read_us = 0;
  size_t reads = 0;
  for (size_t i = 0; i < n; ++i) {
    keys.emplace_back("wtr-" + std::to_string(i));
    auto wh = (*cache)->write_sync(keys.back(), size);
    if (!wh || !wh->write_sync(buf) || !wh->close_sync()) return 1;
    if (i >= lag) {
      const auto t0 = std::chrono::steady_clock::now();
      auto rh = (*cache)->read_sync(keys[i - lag]);
      if (!rh) return 1;
      read_us += std::chrono::duration<double, std::micro>(
                     std::chrono::steady_clock::now() - t0)
                     .count();
      ++reads;
    }
  }
  const auto st = (*cache)->stats();
  std::printf(
      "size %zu n %zu lag %zu: %.2f us/read  cold %llu seq %llu "
      "recent_skips %llu\n",
      size, n, lag, read_us / static_cast<double>(reads),
      static_cast<unsigned long long>(st.cold_readahead_hints),
      static_cast<unsigned long long>(st.sequential_readahead_hints),
      static_cast<unsigned long long>(st.recent_write_hint_skips));
  (*cache)->stop();
  return 0;
}
