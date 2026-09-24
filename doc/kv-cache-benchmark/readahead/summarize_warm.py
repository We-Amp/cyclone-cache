#!/usr/bin/env python3

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Median per cell over warm-<tag>-<rep>.jsonl files: tags given on argv.
import glob
import json
import statistics
import sys
from collections import defaultdict

d, prefix, tags = sys.argv[1], sys.argv[2], sys.argv[3:]
vals = defaultdict(list)
for t in tags:
    for f in sorted(glob.glob(f"{d}/{prefix}-{t}-*.jsonl")):
        for line in open(f):
            r = json.loads(line)
            if "block_size" not in r:
                continue
            ph, mode, th = r["phase"], r.get("mode", "-"), r.get("threads", 1)
            metric = ("ops_per_s" if ph in ("get_warm", "multiprocess_read")
                      and mode == "view" else "gb_per_s")
            vals[(r["block_size"], ph, mode, th, t)].append(r[metric])
cells = sorted({k[:4] for k in vals})
for c in cells:
    meds = []
    out = []
    for t in tags:
        v = vals.get(c + (t,), [])
        m = statistics.median(v) if v else float("nan")
        meds.append(m)
        out.append(f"{t} {m:,.2f} {[round(x, 2) for x in v]}")
    ratio = meds[-1] / meds[0] if meds[0] else float("nan")
    print(f"{c[0] // 1024}K {c[1]:17} {c[2]:4} T={c[3]}  " + "  ".join(out) +
          f"  ratio {ratio:.3f}")
