#!/usr/bin/env python3

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Issue #43: per-run table, medians and the churn criterion for driver.sh.

  summarize.py OUT_DIR

Reads OUT_DIR/churn-<name>-<pattern>-t<T>-<rep>.jsonl and, beside them,
samples/<tag>.txt (round6/sampler.sh) and samples/<tag>.jobs (driver.sh's
job watcher).  A run is marked `*` and left out of the medians when a
runner or container job was seen during its measured phase: the window is
round6/samples.py's, [end - 125 s, end] where end is the last sampler row
with cgroup columns.  Latencies in ms.  "held/ins" is the time an insert
was held back, in ms per insert: the pilot's sleep (cy_admission_delay_us)
or the time spent starting write-back (cy_write_behind_us).  "write amp" is
device bytes written per inserted byte.  The criterion (kv-churn-spec.md) pairs each Cyclone median with
the same-day LMDB median at T=4; brackets give the range over every pairing
of clean runs.
"""
import glob
import json
import os
import re
import statistics
import sys

TAG = re.compile(r"churn-(.+)-(zipf\+scan|zipf)-t(\d+)-(\d+)\.jsonl$")


def window_end(sampler_path):
    end = None
    try:
        with open(sampler_path) as f:
            for line in f:
                p = line.split()
                if len(p) > 14 and p[14] != "-":
                    end = int(p[0]) / 1e3
                    runner = int(p[1])
                    if runner > 0:
                        yield ("runner", end)
    except OSError:
        return
    if end is not None:
        yield ("end", end)


def job_affected(out, tag):
    events = list(window_end(os.path.join(out, "samples", tag + ".txt")))
    ends = [t for k, t in events if k == "end"]
    if not ends:
        return False
    end = ends[-1]
    lo = end - 125.0
    if any(k == "runner" and lo <= t <= end for k, t in events):
        return True
    try:
        with open(os.path.join(out, "samples", tag + ".jobs")) as f:
            return any(lo <= float(x) <= end for x in f.read().split())
    except OSError:
        return False


def load(out):
    runs = []
    for path in sorted(glob.glob(os.path.join(out, "churn-*.jsonl"))):
        m = TAG.search(os.path.basename(path))
        if not m:
            continue
        with open(path) as f:
            line = f.readline()
        if not line.strip():
            continue
        d = json.loads(line)
        tag = os.path.basename(path)[len("churn-"):-len(".jsonl")]
        d.update(name=m[1], pat=m[2], t=int(m[3]), rep=int(m[4]), tag=tag,
                 bad=job_affected(out, tag))
        runs.append(d)
    return runs


def ms(v):
    return v / 1e3


def row(d):
    ins = d.get("inserts", 0) or 1
    # Time an insert was held back, per insert: the pilot's sleep, or the
    # time spent starting write-back (write_behind_us).
    held_us = (d.get("cy_admission_delay_us", 0) +
               d.get("cy_write_behind_us", 0))
    return (d["served_gb_per_s"], ms(d["hit_p50_us"]), ms(d["hit_p99_us"]),
            ms(d["hit_p999_us"]), ms(d["miss_p50_us"]), ms(d["miss_p99_us"]),
            ms(d["miss_p999_us"]), held_us / ins / 1e3,
            d.get("write_amp") or 0.0)


HDR = ("served", "hit p50", "hit p99", "hit p99.9", "ins p50", "ins p99",
       "ins p99.9", "held/ins", "write amp")


def fmt(vals):
    return " ".join(f"{v:9.2f}" for v in vals)


def main():
    out = sys.argv[1]
    runs = load(out)
    order = ["main", "fill", "fillwa", "lmdb"]
    names = sorted({r["name"] for r in runs},
                   key=lambda n: (order.index(n) if n in order else 9, n))
    print("per run (* = a job ran during the measured phase; not in medians)")
    print(f"{'run':34s} " + " ".join(f"{h:>9s}" for h in HDR))
    for r in sorted(runs, key=lambda r: (r["t"], r["pat"], r["rep"],
                                         names.index(r["name"]))):
        flag = "*" if r["bad"] else " "
        print(f"{r['tag']:33s}{flag} " + fmt(row(r)))

    print("\nmedians over clean runs (n)")
    med = {}
    for t in (4, 1):
        for pat in ("zipf", "zipf+scan"):
            for n in names:
                sel = [r for r in runs if r["t"] == t and r["pat"] == pat
                       and r["name"] == n and not r["bad"]]
                if not sel:
                    continue
                cols = list(zip(*(row(r) for r in sel)))
                m = [statistics.median(c) for c in cols]
                med[(t, pat, n)] = (m, sel)
                label = f"{pat} T={t} {n} ({len(sel)})"
                print(f"{label:34s} " + fmt(m))

    print("\ncriterion at T=4 against same-day LMDB (served >= 1.5x, or "
          "hit p99 <= 0.5x at served >= 0.9x)")
    for pat in ("zipf", "zipf+scan"):
        if (4, pat, "lmdb") not in med:
            continue
        lm, lsel = med[(4, pat, "lmdb")]
        for n in names:
            if n == "lmdb" or (4, pat, n) not in med:
                continue
            cm, csel = med[(4, pat, n)]
            served = cm[0] / lm[0]
            p99 = cm[2] / lm[2]
            pr = [row(c)[2] / row(l)[2] for c in csel for l in lsel]
            a = served >= 1.5
            b = p99 <= 0.5 and served >= 0.9
            print(f"{pat:10s} {n:8s} served {served:.2f}x  hit p99 "
                  f"{p99:.2f}x [{min(pr):.2f}-{max(pr):.2f}]  "
                  f"A {'yes' if a else 'no '}  B {'yes' if b else 'no'}")


if __name__ == "__main__":
    main()
