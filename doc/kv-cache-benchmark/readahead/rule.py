#!/usr/bin/env python3

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Throwaway: make the advise_willneed chunk rule env-configurable
#   RA_SMALL (64K)  RA_BIG (512K)  RA_HEAD (1M)  RA_TAIL (0)
# chunk = RA_SMALL while advised < RA_HEAD or remaining <= RA_TAIL, else RA_BIG.
import sys

p = sys.argv[1] + "/src/io/mapped_file.cpp"
s = open(p).read()
old = """    constexpr size_t kHeadChunkBytes = size_t{64} * 1024;
    constexpr size_t kHeadBytes = size_t{1024} * 1024;
    constexpr size_t kTailChunkBytes = size_t{512} * 1024;
    size_t advised = 0;
    while (advise_length > 0) {
      const size_t limit =
          advised < kHeadBytes ? kHeadChunkBytes : kTailChunkBytes;"""
new = """    auto env = [](const char* n, size_t d) -> size_t {
      const char* v = getenv(n);
      return v ? strtoull(v, nullptr, 10) : d;
    };
    static const size_t kHeadChunkBytes = env("RA_SMALL", 64 * 1024);
    static const size_t kHeadBytes = env("RA_HEAD", 1024 * 1024);
    static const size_t kTailChunkBytes = env("RA_BIG", 512 * 1024);
    static const size_t kTailBytes = env("RA_TAIL", 0);
    size_t advised = 0;
    while (advise_length > 0) {
      const size_t limit =
          (advised < kHeadBytes || advise_length <= kTailBytes)
              ? kHeadChunkBytes : kTailChunkBytes;"""
assert s.count(old) == 1
s = s.replace(old, new)
s = s.replace("#include <cerrno>\n", "#include <cerrno>\n#include <cstdlib>\n", 1)
open(p, "w").write(s)
print("patched")
