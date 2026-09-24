#!/usr/bin/env python3

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Median first-touch / restart GB/s per chunk rule and block size.
import glob
import json
import statistics
import sys
from collections import defaultdict

d = sys.argv[1]
vals = defaultdict(list)
for f in sorted(glob.glob(f"{d}/*.jsonl")):
    tag = f.split("/")[-1].rsplit("-", 1)[0]
    for line in open(f):
        r = json.loads(line)
        if r.get("phase") in ("get_first_touch", "restart"):
            vals[(tag, r["block_size"], r["phase"])].append(r["gb_per_s"])
tags = sorted({k[0] for k in vals})
sizes = sorted({k[1] for k in vals})
for ph in ("get_first_touch", "restart"):
    print(f"## {ph}: median GB/s [runs]")
    for bs in sizes:
        cells = []
        for t in tags:
            v = vals.get((t, bs, ph), [])
            m = statistics.median(v) if v else float("nan")
            cells.append(f"{t} {m:.2f} {[round(x, 2) for x in v]}")
        print(f"{bs // 1024:>6} KiB  " + "  ".join(cells))
