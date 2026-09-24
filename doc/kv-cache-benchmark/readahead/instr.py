#!/usr/bin/env python3

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Throwaway timing instrumentation for Volume::read_sync (never committed).
# Buckets: probe..key-match, hint syscall(s), CRC pass, borrow..handle.
import sys

p = sys.argv[1] + "/src/core/volume.cpp"
s = open(p).read()

prelude = r'''
#include <cstdio>
#include <cstdlib>
namespace cyclone_instr {
inline uint64_t now_ns() {
  timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}
inline std::atomic<uint64_t> g[6];
inline void dump_now() {
  uint64_t n = g[5].load(); if (!n) return;
  std::fprintf(stderr, "INSTR n=%llu probe_us=%.1f hint_us=%.1f crc_us=%.1f tail_us=%.1f total_us=%.1f\n",
    (unsigned long long)n, g[0]/1e3/n, g[1]/1e3/n, g[2]/1e3/n, g[3]/1e3/n, g[4]/1e3/n);
}
}
'''
anchor = "std::expected<ReadHandle, CacheError> Volume::read_sync(const CacheKey& key) {\n"
assert anchor in s
s = s.replace(anchor, prelude + anchor + "  const uint64_t _i0 = cyclone_instr::now_ns(); uint64_t _i1 = 0, _i2 = 0, _i3 = 0;\n", 1)

hint = "          maybe_advise_readahead(doc_offset,\n                                 mapped->first("
assert s.count(hint) == 1
s = s.replace(hint, "          _i1 = cyclone_instr::now_ns();\n" + hint, 1)

crc = "          // Verify checksum to detect corruption or torn reads.\n          // Skip if this {offset, checksum} pair"
assert s.count(crc) == 1
s = s.replace(crc, "          _i2 = cyclone_instr::now_ns();\n" + crc, 1)

lease = "          // Lease protocol + register the borrow (count+1) and stamp the\n"
assert s.count(lease) >= 1  # first one is read_sync
s = s.replace(lease, "          _i3 = cyclone_instr::now_ns();\n" + lease, 1)

found = "          result = ReadHandle(impl);\n          found = true;\n"
assert s.count(found) == 1
s = s.replace(found, found + r'''          { const uint64_t _i4 = cyclone_instr::now_ns();
            cyclone_instr::g[0] += _i1 - _i0; cyclone_instr::g[1] += _i2 - _i1;
            cyclone_instr::g[2] += _i3 - _i2; cyclone_instr::g[3] += _i4 - _i3;
            cyclone_instr::g[4] += _i4 - _i0;
            static const uint64_t _stop = std::getenv("INSTR_N") ? std::strtoull(std::getenv("INSTR_N"), nullptr, 10) : 4098;
            if (cyclone_instr::g[5].fetch_add(1) + 1 == _stop) cyclone_instr::dump_now(); }
''', 1)
open(p, "w").write(s)
print("patched")
