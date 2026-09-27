#!/usr/bin/env python3

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Issue #35: device and page-cache activity over each churn point's
measured phase.

The window is round6/samples.py's: the last sampler.sh row that still has
cgroup columns marks the end of the run, and [end - 125 s, end - 5 s] covers
the 120 s measured phase.  From diskstats.sh (<tag>.disk) it reports, per
second of that window: reads and writes per second, MB/s, the mean request
size, the mean time a request took (r_await / w_await, ms), and the mean
number of requests in the device (queue).  From sampler.sh (<tag>.txt):
pages dirtied and written per second (global), the cgroup's reclaim
(pgsteal) and refaults, in MiB/s, and its dirty and writeback MiB.  From
wbt.sh (<tag>.wbt), when present: the mean writeback depth limit blk-wbt
allowed (wb_normal), the share of samples at 8 or less (writeback throttled
hard because reads missed their latency target) and at the full 384, and
the mean writeback requests in flight.

  diskstats_summary.py SAMPLES_DIR [TAG_REGEX]
"""
import glob
import os
import re
import sys

WINDOW = (125.0, 5.0)


def rows(path):
    out = []
    with open(path) as f:
        for line in f:
            p = line.split()
            if len(p) >= 2:
                out.append(p)
    return out


def window(sampler_rows):
    end = None
    for r in sampler_rows:
        if len(r) > 14 and r[14] != "-":
            end = int(r[0]) / 1e3
    if end is None:
        return None
    return end - WINDOW[0], end - WINDOW[1]


def delta(rs, lo, hi):
    rs = [r for r in rs if lo <= int(r[0]) / 1e3 <= hi]
    if len(rs) < 2:
        return None, None
    a, b = rs[0], rs[-1]
    dt = (int(b[0]) - int(a[0])) / 1e3
    return [float(y) - float(x) for x, y in zip(a[1:], b[1:])], dt


def disk(path, lo, hi):
    d, dt = delta(rows(path), lo, hi)
    if d is None:
        return None
    rd, _, rsec, rms, wr, _, wsec, wms, _, io_ms, wio_ms = d[:11]
    return {
        "r/s": rd / dt, "rMB/s": rsec * 512 / 1e6 / dt,
        "rKiB/req": rsec * 512 / 1024 / rd if rd else 0,
        "r_await": rms / rd if rd else 0,
        "w/s": wr / dt, "wMB/s": wsec * 512 / 1e6 / dt,
        "wKiB/req": wsec * 512 / 1024 / wr if wr else 0,
        "w_await": wms / wr if wr else 0,
        "util%": io_ms / dt / 10, "queue": wio_ms / dt / 1e3,
    }


def vm(sampler_rows, lo, hi):
    rs = [r for r in sampler_rows if lo <= int(r[0]) / 1e3 <= hi
          and len(r) > 19 and r[14] != "-"]
    if len(rs) < 2:
        return None
    a, b = rs[0], rs[-1]
    dt = (int(b[0]) - int(a[0])) / 1e3
    mib = 4096 / 2**20

    def rate(i, scale):
        return (float(b[i]) - float(a[i])) * scale / dt

    return {
        "dirtied MiB/s": rate(4, mib), "written MiB/s": rate(5, mib),
        "cg pgsteal MiB/s": rate(18, mib), "cg refault MiB/s": rate(19, mib),
        "cg dirty MiB": sum(float(r[15]) for r in rs) / len(rs) / 2**20,
        "cg wb MiB": sum(float(r[16]) for r in rs) / len(rs) / 2**20,
    }


def wbt(path, lo, hi):
    rs = [r for r in rows(path) if lo <= int(r[0]) / 1e3 <= hi and len(r) >= 4]
    if not rs:
        return None
    normal = [float(r[1]) for r in rs]
    return {
        "wbt normal mean": sum(normal) / len(normal),
        "wbt throttled%": 100 * sum(n <= 8 for n in normal) / len(normal),
        "wbt full%": 100 * sum(n >= 384 for n in normal) / len(normal),
        "wbt inflight": sum(float(r[3]) for r in rs) / len(rs),
    }


def main():
    d = sys.argv[1]
    rx = re.compile(sys.argv[2]) if len(sys.argv) > 2 else None
    for s in sorted(glob.glob(os.path.join(d, "*.txt"))):
        tag = os.path.basename(s)[:-4]
        if rx and not rx.search(tag):
            continue
        dp = os.path.join(d, tag + ".disk")
        sr = rows(s)
        w = window(sr)
        if w is None or not os.path.exists(dp):
            continue
        k = disk(dp, *w)
        v = vm(sr, *w)
        wp = os.path.join(d, tag + ".wbt")
        b = wbt(wp, *w) if os.path.exists(wp) else None
        parts = []
        for m in (k, v, b):
            if m:
                parts += [f"{n} {x:.2f}" for n, x in m.items()]
        print(f"{tag}: " + "; ".join(parts))


if __name__ == "__main__":
    main()
