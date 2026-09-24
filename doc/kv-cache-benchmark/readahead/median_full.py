#!/usr/bin/env python3

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Median over several full kv_bench sweeps per tree, cell by cell.
# usage: median_full.py DIR   (reads full-base*.jsonl and full-after*.jsonl)
import glob
import json
import statistics
import sys
from collections import defaultdict

d = sys.argv[1]
vals = defaultdict(list)
for tree in ("base", "after"):
    for f in sorted(glob.glob(f"{d}/full-{tree}*.jsonl")):
        for line in open(f):
            r = json.loads(line)
            if "block_size" not in r:
                continue
            ph, mode, th = r["phase"], r.get("mode", "-"), r.get("threads", 1)
            view = ph in ("get_warm", "multiprocess_read") and mode == "view"
            metric = "ops_per_s" if view else "gb_per_s"
            vals[(r["block_size"], ph, mode, th, tree)].append(r[metric])
cells = sorted({k[:4] for k in vals})
print(f"{'block':>7} {'phase':17} {'mode':5} {'T':>2} {'base':>11} "
      f"{'after':>11} ratio  runs(base | after)")
for c in cells:
    b = vals.get(c + ("base",), [])
    a = vals.get(c + ("after",), [])
    if not a or not b:
        continue
    mb, ma = statistics.median(b), statistics.median(a)
    unit = "gets/s" if c[1] in ("get_warm", "multiprocess_read") and \
        c[2] == "view" else "GB/s"
    print(f"{c[0] // 1024:>6}K {c[1]:17} {c[2]:5} {c[3]:>2} {mb:>11.2f} "
          f"{ma:>11.2f} {ma / mb:5.2f}  {[round(x, 2) for x in b]} | "
          f"{[round(x, 2) for x in a]} {unit}")
