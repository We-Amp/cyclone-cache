#!/usr/bin/env python3

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Round 7: per-run tables, medians and the churn criteria for driver.sh.

  summarize.py OUT_DIR churn
  summarize.py OUT_DIR sweep

churn: reads OUT_DIR/churn-<name>-<pattern>-t<T>-<rep>.jsonl.  A run is
marked `*` and left out of the medians when a runner or container job was
seen during its measured phase (the admission-control summariser's window:
[end - 125 s, end], end = the last round6/sampler.sh row with cgroup
columns; samples/<tag>.jobs is the driver's 5 s job watcher).  Latencies in
ms.  "held/ins" is the time an insert spent starting write-back
(cy_write_behind_us), ms per insert.  The criteria (kv-churn-spec.md) pair
each Cyclone median with the same-day LMDB median; brackets give the range
over every pairing of clean runs, and "worst" is the pairing least
favourable pairing: each pairing is checked against A or B on its own, and
the count of pairings that pass is printed.

sweep: reads OUT_DIR/<run>-<kind>-<rep>.jsonl (kv_bench / kvpeer).  A run
during which the job watcher saw a job at any time is marked `*` and left
out of the medians; n=clean/total is printed per cell, and a cell with no
clean run prints n/a.

The committed directory keeps the sampler files for a subset of runs only;
there the marks come from round7-marks.txt (one marked tag per line, written
by `summarize.py OUT_DIR marks` over the full output), which takes precedence
when it exists.
"""
import glob
import json
import os
import re
import statistics
import sys

CHURN = re.compile(r"churn-(.+)-(zipf\+scan|zipf)-t(\d+)-(\d+)\.jsonl$")
ORDER = ["ret", "fill", "wb", "fillwb", "flush", "lmdb", "filedir"]


def saved_marks(out):
    try:
        with open(os.path.join(out, "round7-marks.txt")) as f:
            return {x.strip() for x in f if x.strip()
                    and not x.startswith("#")}
    except OSError:
        return None


def jobs_in(out, tag, lo=None, hi=None):
    try:
        with open(os.path.join(out, "samples", tag + ".jobs")) as f:
            ts = [float(x) for x in f.read().split()]
    except OSError:
        ts = []
    if lo is None:
        return bool(ts)
    return any(lo <= t <= hi for t in ts)


def measured_window(out, tag):
    end = None
    runner = []
    try:
        with open(os.path.join(out, "samples", tag + ".txt")) as f:
            for line in f:
                p = line.split()
                if len(p) > 14 and p[14] != "-":
                    end = int(p[0]) / 1e3
                    if int(p[1]) > 0:
                        runner.append(end)
    except OSError:
        pass
    return end, runner


def churn_affected(out, tag):
    marks = saved_marks(out)
    if marks is not None:
        return "churn-" + tag in marks
    end, runner = measured_window(out, tag)
    if end is None:
        return jobs_in(out, tag)
    lo = end - 125.0
    return any(lo <= t <= end for t in runner) or jobs_in(out, tag, lo, end)


def load_churn(out):
    runs = []
    for path in sorted(glob.glob(os.path.join(out, "churn-*.jsonl"))):
        m = CHURN.search(os.path.basename(path))
        if not m:
            continue
        with open(path) as f:
            lines = [x for x in f if x.strip().startswith("{")]
        if not lines:
            continue
        d = json.loads(lines[-1])
        tag = os.path.basename(path)[len("churn-"):-len(".jsonl")]
        d.update(name=m[1], pat=m[2], t=int(m[3]), rep=int(m[4]), tag=tag,
                 bad=churn_affected(out, tag))
        runs.append(d)
    return runs


def row(d):
    ins = d.get("inserts", 0) or 1
    return (d["hit_ratio"], d["served_gb_per_s"], d["hit_p50_us"] / 1e3,
            d["hit_p99_us"] / 1e3, d["hit_p999_us"] / 1e3,
            d["miss_p50_us"] / 1e3, d["miss_p99_us"] / 1e3,
            d["miss_p999_us"] / 1e3,
            d.get("cy_write_behind_us", 0) / ins / 1e3,
            d.get("write_amp") or 0.0, d.get("reopen_hit_ratio") or 0.0)


HDR = ("hit ratio", "served", "hit p50", "hit p99", "hit p99.9", "ins p50",
       "ins p99", "ins p99.9", "held/ins", "write amp", "reopen")


def fmt(vals):
    return " ".join(f"{v:9.3f}" if i in (0, 2, 10) else f"{v:9.2f}"
                    for i, v in enumerate(vals))


def churn(out):
    runs = load_churn(out)
    names = sorted({r["name"] for r in runs},
                   key=lambda n: (ORDER.index(n) if n in ORDER else 9, n))
    print("per run (* = a job ran during the measured phase; not in medians)")
    print(f"{'run':34s} " + " ".join(f"{h:>9s}" for h in HDR) + "  failed")
    for r in sorted(runs, key=lambda r: (-r["t"], r["pat"], r["rep"],
                                         names.index(r["name"]))):
        flag = "*" if r["bad"] else " "
        print(f"{r['tag']:33s}{flag} " + fmt(row(r)) +
              f"  {r.get('failed')} err={r.get('read_errors', 0)}"
              f" drop={r.get('cy_writes_dropped_by_lease', '-')}")

    print("\nmedians over clean runs (n)")
    print(f"{'cell':34s} " + " ".join(f"{h:>9s}" for h in HDR))
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
                tot = len([r for r in runs if r["t"] == t and r["pat"] == pat
                           and r["name"] == n])
                print(f"{pat + ' T=' + str(t) + ' ' + n:28s}({len(sel)}/{tot}) "
                      + fmt(m))

    print("\ncriteria against same-day LMDB: A served >= 1.5x; "
          "B hit p99 <= 0.5x at served >= 0.9x")
    print("[served range] [hit p99 range] over every clean pairing; worst = "
          "least favourable pairing")
    verdict = {}
    for t in (4, 1):
        for pat in ("zipf", "zipf+scan"):
            if (t, pat, "lmdb") not in med:
                continue
            lm, lsel = med[(t, pat, "lmdb")]
            for n in names:
                if n == "lmdb" or (t, pat, n) not in med:
                    continue
                cm, csel = med[(t, pat, n)]
                served = cm[1] / lm[1]
                p99 = cm[3] / lm[3]
                pairs = [(row(c)[1] / row(l)[1], row(c)[3] / row(l)[3])
                         for c in csel for l in lsel]
                a = served >= 1.5
                b = p99 <= 0.5 and served >= 0.9
                ok = [x[0] >= 1.5 or (x[1] <= 0.5 and x[0] >= 0.9)
                      for x in pairs]
                worst_ok = all(ok)
                wp = max(x[1] for x in pairs)
                ws = min(x[0] for x in pairs)
                wtxt = (f"pairings passing {sum(ok)}/{len(ok)}; worst p99 "
                        f"{wp:.2f}x, worst served {ws:.2f}x")
                verdict[(t, pat, n)] = (a or b, worst_ok)
                print(f"T={t} {pat:10s} {n:8s} served {served:.2f}x "
                      f"[{min(x[0] for x in pairs):.2f}-"
                      f"{max(x[0] for x in pairs):.2f}]  hit p99 {p99:.2f}x "
                      f"[{min(x[1] for x in pairs):.2f}-"
                      f"{max(x[1] for x in pairs):.2f}]  "
                      f"A {'yes' if a else 'no '}  B {'yes' if b else 'no '}"
                      f"  {wtxt} ({'holds' if worst_ok else 'fails'})")

    print("\nverdict per configuration (WIN: both patterns pass at T=4; "
          "PARTIAL: a pass at T=1 only or on one pattern only; LOSS: none)")
    for n in names:
        if n in ("lmdb", "filedir"):
            continue
        t4 = [verdict.get((4, p, n), (False, False))[0]
              for p in ("zipf", "zipf+scan")]
        t1 = [verdict.get((1, p, n), (False, False))[0]
              for p in ("zipf", "zipf+scan")]
        t4w = [verdict.get((4, p, n), (False, False))[1]
               for p in ("zipf", "zipf+scan")]
        if all(t4):
            v = "WIN"
        elif any(t4) or any(t1):
            v = "PARTIAL"
        else:
            v = "LOSS"
        print(f"{n:8s} {v:8s} T=4 zipf/zipf+scan {t4}  T=1 {t1}  "
              f"T=4 worst pairing holds {t4w}")


SWEEP = re.compile(r"^(\w+)-(full|noverify|cold|put)-(\d+)\.jsonl$")


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
    bad = {}
    for p in sorted(glob.glob(os.path.join(out, "*.jsonl"))):
        base = os.path.basename(p)
        m = SWEEP.match(base)
        if not m:
            continue
        run, kind, rep = m.group(1), m.group(2), int(m.group(3))
        tag = base[:-len(".jsonl")]
        marks = saved_marks(out)
        bad[(run, kind, rep)] = (tag in marks if marks is not None
                                 else jobs_in(out, tag))
        for r in load(p):
            if r.get("phase") in (None, "machine"):
                continue
            key = (kind, run, r["block_size"], r["phase"], r.get("mode", ""),
                   r.get("threads", 1), r.get("processes", ""))
            groups.setdefault(key, {})[rep] = r
    marked = sorted(f"{k[0]}-{k[1]}-{k[2]}" for k, v in bad.items() if v)
    print("runs a job was seen in (left out of the medians): " +
          (", ".join(marked) if marked else "none"))
    last = None
    for key in sorted(groups, key=lambda k: (k[0], k[2], k[3], k[4], k[5],
                                             str(k[6]), k[1])):
        kind, run, bs, phase, mode, t, procs = key
        if (kind, bs) != last:
            print(f"\n== {kind}, block {bs} ==")
            last = (kind, bs)
        reps = sorted(groups[key])
        clean = [r for r in reps if not bad[(run, kind, r)]]

        def cell(field, nd):
            allv = " ".join(
                (f"{groups[key][r].get(field):.{nd}f}"
                 if groups[key][r].get(field) is not None else "-") +
                ("*" if bad[(run, kind, r)] else "") for r in reps)
            vals = [groups[key][r].get(field) for r in clean
                    if groups[key][r].get(field) is not None]
            m = f"{statistics.median(vals):.{nd}f}" if vals else "n/a"
            return f"{m} [{allv}]"

        print(f"{phase:18s} {mode:5s} T={t!s:2s} {procs!s:2s} {run:9s} "
              f"n={len(clean)}/{len(reps)} "
              f"GB/s {cell('gb_per_s', 2)}  ops/s {cell('ops_per_s', 0)}  "
              f"p50 {cell('p50_us', 1)}  p99 {cell('p99_us', 1)}")


def marks(out):
    print("# runs a runner or container job was seen in (see summarize.py)")
    for p in sorted(glob.glob(os.path.join(out, "*.jsonl"))):
        base = os.path.basename(p)[:-len(".jsonl")]
        if CHURN.search(os.path.basename(p)):
            if churn_affected(out, base[len("churn-"):]):
                print(base)
        elif SWEEP.match(os.path.basename(p)) and jobs_in(out, base):
            print(base)


if __name__ == "__main__":
    {"churn": churn, "sweep": sweep, "marks": marks}[sys.argv[2]](sys.argv[1])
