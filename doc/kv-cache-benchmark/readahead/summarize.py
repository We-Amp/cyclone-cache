#!/usr/bin/env python3

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Summarise the cold reps: median first-touch / restart GB/s per store & size.
import glob
import json
import statistics
import sys
from collections import defaultdict

d = sys.argv[1]
vals = defaultdict(list)  # (store, bs, phase) -> [gbps]
p50 = defaultdict(list)
for f in sorted(glob.glob(f"{d}/cold-*.jsonl")):
    tag = f.split("/cold-")[1].rsplit("-", 1)[0]
    for line in open(f):
        r = json.loads(line)
        ph = r.get("phase")
        if ph not in ("get_first_touch", "restart"):
            continue
        bs = r["block_size"]
        vals[(tag, bs, ph)].append(r["gb_per_s"])
        p50[(tag, bs, ph)].append(r.get("p50_us"))
sizes = sorted({k[1] for k in vals})
for ph in ("get_first_touch", "restart"):
    print(f"## {ph}: median GB/s of 3 [runs]; after/base; lmdb/after")
    for bs in sizes:
        row = []
        med = {}
        for t in ("base", "after", "lmdb"):
            v = vals.get((t, bs, ph), [])
            med[t] = statistics.median(v) if v else float("nan")
            row.append(f"{t} {med[t]:.2f} {[round(x, 2) for x in v]}")
        print(f"{bs // 1024:>6} KiB  " + "  ".join(row) +
              f"  after/base {med['after'] / med['base']:.2f}x"
              f"  lmdb/after {med['lmdb'] / med['after']:.2f}x")
