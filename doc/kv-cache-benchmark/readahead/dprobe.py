#!/usr/bin/env python3

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Raw O_DIRECT read probe on a volume file, page cache bypassed.
# usage: dprobe.py FILE STRIPE0_START STRIPE_BYTES NSTRIPES BLOCK N
# Each test reads from its own third of the first 120 MiB of the stripes
# (written data only, no hole reads), so no test re-reads another's range.
import mmap
import os
import random
import sys
import time

path = sys.argv[1]
base, stripe, nstr, blk, n = (int(x) for x in sys.argv[2:7])
fd = os.open(path, os.O_RDONLY | os.O_DIRECT)
buf = mmap.mmap(-1, blk)
third = 36 << 20


def run(name, offs):
    t = time.perf_counter()
    for o in offs:
        assert os.preadv(fd, [buf], o) == blk
    dt = time.perf_counter() - t
    print(f"{name:26s} {len(offs)} x {blk // 1024} KiB: "
          f"{len(offs) * blk / dt / 1e9:.2f} GB/s, "
          f"{dt / len(offs) * 1e6:.0f} us/read", flush=True)


run("sequential, one stripe",
    [base + i * blk for i in range(min(n, third // blk))])
rng = random.Random(1)
cur = [0] * nstr
offs = []
for _ in range(n):
    s = rng.randrange(nstr)
    if cur[s] * blk >= third:
        continue
    offs.append(base + s * stripe + third + cur[s] * blk)
    cur[s] += 1
run("16-way interleaved", offs)
rng = random.Random(2)
run("random within stripes",
    [base + rng.randrange(nstr) * stripe + 2 * third +
     rng.randrange(third // blk) * blk for _ in range(n)])
