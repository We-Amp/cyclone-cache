#!/usr/bin/env python3

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Compare two kv_bench full-sweep jsonl files cell by cell.
import json
import sys


def load(p):
    out = {}
    for line in open(p):
        r = json.loads(line)
        if "block_size" not in r:
            continue
        k = (r["block_size"], r["phase"], r.get("mode", "-"), r.get("threads", 1))
        out[k] = r
    return out


a, b = load(sys.argv[1]), load(sys.argv[2])
print(f"{'block':>7} {'phase':18} {'mode':5} {'T':>2} {'base':>12} {'after':>12} ratio")
for k in sorted(a):
    if k not in b:
        continue
    ra, rb = a[k], b[k]
    metric = "gb_per_s" if k[1] != "get_warm" and k[1] != "multiprocess_read" or k[2] == "copy" else "ops_per_s"
    va, vb = ra[metric], rb[metric]
    unit = "GB/s" if metric == "gb_per_s" else "gets/s"
    print(f"{k[0] // 1024:>6}K {k[1]:18} {k[2]:5} {k[3]:>2} {va:>12.2f} {vb:>12.2f} {vb / va:5.2f} {unit}")
