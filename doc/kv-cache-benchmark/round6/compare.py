#!/usr/bin/env python3

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Round 6: every kv_bench cell of the round-6 Cyclone sweep (median of the
repetitions) against one earlier kv_bench JSONL, flagging moves over 10 %.

  compare.py OUT_DIR EARLIER.jsonl [GLOB]   (GLOB default cyclone-full-*)
"""
import glob
import json
import os
import statistics
import sys


def rows(p):
    with open(p) as f:
        return [json.loads(line) for line in f
                if line.startswith("{") and '"machine"' not in line]


def key(r):
    return (r["block_size"], r["phase"], r.get("mode", ""),
            r.get("threads", 1))


def main():
    out, earlier = sys.argv[1], sys.argv[2]
    pat = sys.argv[3] if len(sys.argv) > 3 else "cyclone-full-*.jsonl"
    old = {key(r): r for r in rows(earlier)}
    new = {}
    for p in sorted(glob.glob(os.path.join(out, pat))):
        for r in rows(p):
            new.setdefault(key(r), []).append(r)
    for k in sorted(old):
        if k not in new or not old[k]["ops_per_s"]:
            continue
        a = old[k]["ops_per_s"]
        b = statistics.median(x["ops_per_s"] for x in new[k])
        ratio = b / a
        flag = "  <<" if ratio < 0.9 else ("  >>" if ratio > 1.1 else "")
        bs, phase, mode, t = k
        print(f"{bs:>9} {phase:18s} {mode:5s} T={t}  earlier {a:12.1f}  "
              f"now {b:12.1f}  ratio {ratio:.2f}{flag}")


if __name__ == "__main__":
    main()
