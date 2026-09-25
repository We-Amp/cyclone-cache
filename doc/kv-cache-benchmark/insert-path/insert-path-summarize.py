# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

import glob
import json
import os
import re
import statistics as st
import sys

d = sys.argv[1]


def med(xs):
    return st.median(xs) if xs else float('nan')


print("== insert_bench (median of runs: p50 / p99 / mean total us, GB/s wall)")
for name in ["base", "fix", "fix-reserve"]:
    p = os.path.join(d, f"insert-{name}.jsonl")
    if not os.path.exists(p):
        continue
    rows = [json.loads(l) for l in open(p) if l.strip()]
    for bs in (2097152, 524288):
        for ph in ("fill", "steady"):
            rs = [r for r in rows if r["block_size"] == bs and r["phase"] == ph]
            if not rs:
                continue
            print(f"  {name:12s} {bs:8d} {ph:6s} n={len(rs)} "
                  f"p50 {med([r['total_p50_us'] for r in rs]):8.1f} "
                  f"p99 {med([r['total_p99_us'] for r in rs]):8.1f} "
                  f"mean {med([r['total_mean_us'] for r in rs]):8.1f} "
                  f"write {med([r['write_p50_us'] for r in rs]):7.1f} "
                  f"commit {med([r['commit_p50_us'] for r in rs]):7.1f} "
                  f"minflt {med([r['minflt_per_insert'] for r in rs]):6.1f} "
                  f"gbps {med([r['gb_per_s'] for r in rs]):5.2f}  "
                  f"[p50s {', '.join(str(round(r['total_p50_us'])) for r in rs)}]")

print("== kv_bench put 2 MiB")
for t in ("base", "fix"):
    vals = []
    for p in sorted(glob.glob(os.path.join(d, f"kv-bench-{t}-r*.jsonl"))):
        for l in open(p):
            if not l.strip():
                continue
            r = json.loads(l)
            if r.get("phase") == "put":
                vals.append(r)
    if vals:
        keys = [k for k in vals[0] if 'p50' in k or 'gb' in k or 'p99' in k]
        print(f"  {t}: " + "; ".join(
            f"{k} {med([v[k] for v in vals if isinstance(v.get(k), (int, float))]):.3f}"
            for k in keys) + f"  runs gb/s {[v.get('gb_per_s') for v in vals]}")

print("== performance_baseline sequential_write_4096B (ops/s, p50, p99)")
for t in ("base", "fix"):
    p = os.path.join(d, f"perf-baseline-{t}.txt")
    if not os.path.exists(p):
        continue
    rows = []
    for l in open(p):
        if l.startswith("sequential_write"):
            f = l.split()
            rows.append((float(f[1]), float(f[3]), float(f[4])))
    if rows:
        print(f"  {t}: ops/s {med([r[0] for r in rows]):.0f} p50 {med([r[1] for r in rows]):.2f} "
              f"p99 {med([r[2] for r in rows]):.2f}  runs {rows}")

print("== churn (zipf, 2 MiB, C=16 GiB, 4 GiB cgroup)")
for t in ("base", "fix", "fix-reserve"):
    p = os.path.join(d, f"churn-{t}.jsonl")
    if not os.path.exists(p):
        continue
    rows = [json.loads(l) for l in open(p) if l.strip()]
    for th in (1, 4):
        rs = [r for r in rows if r["threads"] == th]
        if not rs:
            continue
        print(f"  {t:12s} T={th} n={len(rs)} hit {med([r['hit_ratio'] for r in rs]):.3f} "
              f"served {med([r['served_gb_per_s'] for r in rs]):.2f} "
              f"miss p50 {med([r['miss_p50_us'] for r in rs]):7.0f} "
              f"p99 {med([r['miss_p99_us'] for r in rs]):7.0f} "
              f"hit p50 {med([r['hit_p50_us'] for r in rs]):5.0f} "
              f"hit p99 {med([r['hit_p99_us'] for r in rs]):6.0f} "
              f"ins GB/s {med([r['inserted_gb_per_s'] for r in rs]):.3f}  "
              f"[served {[r['served_gb_per_s'] for r in rs]} missp50 {[r['miss_p50_us'] for r in rs]}]")
