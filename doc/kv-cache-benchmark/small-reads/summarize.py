#!/usr/bin/env python3

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Small cold reads (issue #29): medians over interleaved repetitions.

  summarize.py OUT_DIR PREFIX [RATIO_A RATIO_B]

Groups OUT_DIR/PREFIX-<variant>-<rep>.jsonl by variant and prints, per
(block size, phase, mode, threads), the median and the per-run values of
GB/s (cold, copy, PUT) or gets/s (warm view, 4-process view).  With
RATIO_A and RATIO_B it adds the ratio of their medians, A / B.
"""
import glob
import json
import os
import re
import statistics
import sys
from collections import defaultdict

out, prefix = sys.argv[1], sys.argv[2]
ra = sys.argv[3] if len(sys.argv) > 3 else None
rb = sys.argv[4] if len(sys.argv) > 4 else None

vals = defaultdict(lambda: defaultdict(list))  # key -> variant -> [v]
variants = []
for p in sorted(glob.glob(os.path.join(out, f"{prefix}-*.jsonl"))):
    m = re.match(rf"^{re.escape(prefix)}-(.+)-(\d+)\.jsonl$",
                 os.path.basename(p))
    if not m:
        continue
    var = m.group(1)
    if var not in variants:
        variants.append(var)
    for line in open(p):
        line = line.strip()
        if not line.startswith("{"):
            continue
        r = json.loads(line)
        ph = r.get("phase")
        if ph in (None, "machine"):
            continue
        view = r.get("mode") == "view" and ph in ("get_warm",
                                                  "multiprocess_read")
        v = r.get("ops_per_s") if view else r.get("gb_per_s")
        key = (r["block_size"], ph, r.get("mode") or "-", r.get("threads", 1))
        vals[key][var].append(v)

order = {"put": 0, "get_first_touch": 1, "get_warm": 2, "restart": 3,
         "multiprocess_read": 4}


def fmt(x):
    if x is None:
        return "-"
    return f"{x:,.0f}" if x >= 1000 else f"{x:.2f}"


for key in sorted(vals, key=lambda k: (k[0], order.get(k[1], 9), k[2], k[3])):
    bs, ph, mode, t = key
    cells = []
    med = {}
    for var in variants:
        v = [x for x in vals[key].get(var, []) if x is not None]
        if not v:
            continue
        med[var] = statistics.median(v)
        cells.append(f"{var} {fmt(med[var])} [{' '.join(fmt(x) for x in v)}]")
    line = f"{bs // 1024:>6}K {ph:<17} {mode:<4} T={t:<2} " + "  ".join(cells)
    if ra and rb and med.get(rb):
        line += f"  {ra}/{rb} {med.get(ra, float('nan')) / med[rb]:.2f}"
    print(line)
