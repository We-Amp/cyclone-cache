#!/usr/bin/env python3

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Round 6: medians over the interleaved repetitions.

  summarize.py OUT_DIR sweep   kv_bench / kvpeer JSONL (<store>-<kind>-<rep>)
  summarize.py OUT_DIR churn   churn JSONL (churn-<tag>-<rep>.jsonl)

Every cell prints the median and, in brackets, the per-run values in rep
order, so a reader can see the spread the median hides.  A churn run during
whose measured window sampler.sh saw a GitHub runner job start is marked *
and left out of the median (unless every run is marked).
"""
import glob
import json
import os
import re
import statistics
import sys


def med(v):
    v = [x for x in v if x is not None]
    return statistics.median(v) if v else None


def fmt(x, nd=2):
    if x is None:
        return "-"
    return f"{x:.{nd}f}"


def cell(vals, nd=2):
    return f"{fmt(med(vals), nd)} [{' '.join(fmt(v, nd) for v in vals)}]"


def load(path):
    rows = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line.startswith("{"):
                rows.append(json.loads(line))
    return rows


def sweep(out):
    groups = {}
    for p in sorted(glob.glob(os.path.join(out, "*.jsonl"))):
        base = os.path.basename(p)
        m = re.match(r"^(\w+)-(full|noverify|cold)-(\d+)\.jsonl$", base)
        if not m:
            continue
        run, kind, rep = m.group(1), m.group(2), int(m.group(3))
        for r in load(p):
            if r.get("phase") in (None, "machine"):
                continue
            key = (kind, run, r["block_size"], r["phase"], r.get("mode", ""),
                   r.get("threads", 1), r.get("processes", ""))
            groups.setdefault(key, {})[rep] = r
    last = None
    for key in sorted(groups, key=lambda k: (k[0], k[2], k[3], k[4], k[5],
                                             str(k[6]), k[1])):
        kind, run, bs, phase, mode, t, procs = key
        if (kind, bs) != last:
            print(f"\n== {kind}, block {bs} ==")
            last = (kind, bs)
        reps = [groups[key][r] for r in sorted(groups[key])]
        gb = [r.get("gb_per_s") for r in reps]
        ops = [r.get("ops_per_s") for r in reps]
        p50 = [r.get("p50_us") for r in reps]
        p99 = [r.get("p99_us") for r in reps]
        print(f"{phase:18s} {mode:5s} T={t!s:2s} {procs!s:2s} {run:8s} "
              f"GB/s {cell(gb)}  ops/s {cell(ops, 0)}  "
              f"p50 {cell(p50, 1)}  p99 {cell(p99, 1)}")


CHURN_FIELDS = [
    ("hit_ratio", 4), ("served_gb_per_s", 2), ("inserted_gb_per_s", 2),
    ("hit_p50_us", 0), ("hit_p99_us", 0), ("hit_p999_us", 0),
    ("miss_p50_us", 0), ("miss_p99_us", 0), ("miss_p999_us", 0),
    ("reopen_hit_ratio", 3), ("write_amp", 3), ("device_read_bytes", 0),
    ("cy_writes_dropped_by_lease", 0), ("read_errors", 0),
    ("put_failures", 0), ("cy_tag_collision_evictions_total", 0),
]


def contaminated(out, tag):
    """True if sampler.sh saw a runner job in the measured window."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import samples
    # driver layout: OUT/samples/TAG.txt; committed: OUT/round6-samples/
    for d in ("samples", "round6-samples"):
        p = os.path.join(out, d, tag + ".txt")
        if os.path.exists(p):
            break
    else:
        return False
    rows = samples.load(p)
    s = samples.summarize(rows) if rows else None
    return bool(s and s["runner jobs seen"])


def churn(out):
    groups = {}
    for p in sorted(glob.glob(os.path.join(out, "churn-*.jsonl"))):
        m = re.match(r"^churn-(.+)-(\d+)\.jsonl$", os.path.basename(p))
        if not m:
            continue
        rows = load(p)
        if rows:
            rows[-1]["_dirty"] = contaminated(out, m.group(0)[6:-6])
            groups.setdefault(m.group(1), {})[int(m.group(2))] = rows[-1]
    for tag in sorted(groups):
        reps = [groups[tag][r] for r in sorted(groups[tag])]
        failed = [r.get("failed") for r in reps]
        clean = [r for r in reps if not r["_dirty"]] or reps
        marks = "".join("*" if r["_dirty"] else "." for r in reps)
        print(f"\n== {tag} (runs {len(reps)} {marks}, median over "
              f"{len(clean)}, failed {failed}) ==")
        for f, nd in CHURN_FIELDS:
            vals = [r.get(f) for r in reps]
            if all(v is None for v in vals):
                continue
            allv = " ".join(fmt(v, nd) + ("*" if r["_dirty"] else "")
                            for v, r in zip(vals, reps))
            print(f"  {f:34s} {fmt(med([r.get(f) for r in clean]), nd)} "
                  f"[{allv}]")


if __name__ == "__main__":
    {"sweep": sweep, "churn": churn}[sys.argv[2]](sys.argv[1])
