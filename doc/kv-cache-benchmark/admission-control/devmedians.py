#!/usr/bin/env python3

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Issue #43: medians of the device and page-cache columns per cell.

  devmedians.py SAMPLES_SUMMARY CHURN_SUMMARY

SAMPLES_SUMMARY is insert-tail/diskstats_summary.py's output over the
samples directory; CHURN_SUMMARY is summarize.py's output, whose per-run
table marks the runs a job started into (`*`), which are left out here as
they are there.  One line per (threads, pattern, configuration): the number
of runs, then the median of reads/s, read await (ms), write MB/s, write
await (ms), requests in the device, utilisation %, the cgroup's dirty MiB
and the share of blk-wbt samples at its tightest writeback limit.
"""
import collections
import re
import statistics
import sys

KEYS = ["r/s", "r_await", "wMB/s", "w_await", "queue", "util%",
        "cg dirty MiB", "wbt throttled%"]
TAG = re.compile(r"(.+)-(zipf\+scan|zipf)-t(\d+)-(\d+)$")


def main():
    bad = set()
    with open(sys.argv[2]) as f:
        for line in f:
            p = line.split()
            if len(p) > 1 and p[1] == "*":
                bad.add(p[0])
            elif p and p[0].endswith("*"):
                bad.add(p[0][:-1])
    cells = collections.defaultdict(list)
    with open(sys.argv[1]) as f:
        for line in f:
            tag, rest = line.rstrip("\n").split(": ", 1)
            m = TAG.match(tag)
            if not m or tag in bad:
                continue
            d = {}
            for kv in rest.split("; "):
                k, v = kv.rsplit(" ", 1)
                d[k] = float(v)
            cells[(int(m[3]), m[2], m[1])].append(d)
    print("T pattern config n " + " | ".join(KEYS))
    for k in sorted(cells, key=lambda k: (-k[0], k[1], k[2])):
        rs = cells[k]
        vals = [statistics.median(r.get(q, 0.0) for r in rs) for q in KEYS]
        print(f"{k[0]} {k[1]} {k[2]} {len(rs)} " +
              " | ".join(f"{v:.2f}" for v in vals))


if __name__ == "__main__":
    main()
