#!/usr/bin/env python3

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Round 6: memory and writeback state during the measured phase of each
churn point, from sampler.sh output.

The sampler does not know the phase boundaries.  The benchmark's cgroup
disappears when the process exits, so the last sample with cgroup columns
marks the end; the window is [end - 125 s, end - 5 s], which covers the
120 s measured phase and excludes the reopen and the final syncfs.

  samples.py SAMPLES_DIR [TAG_REGEX]
"""
import glob
import os
import re
import statistics
import sys

COLS = ["t", "jobs", "nr_dirty", "nr_writeback", "nr_dirtied", "nr_written",
        "pgscan_direct", "pgsteal_direct", "pgscan_kswapd", "pgsteal_kswapd",
        "allocstall_normal", "allocstall_movable", "refault_g", "pgmajfault",
        "cg_file", "cg_file_dirty", "cg_file_writeback", "cg_pgscan",
        "cg_pgsteal", "cg_refault", "cg_mem_full_us", "cg_io_full_us"]
PAGE = 4096


def load(path):
    rows = []
    with open(path) as f:
        for line in f:
            v = line.split()
            if len(v) != len(COLS) or "-" in v:
                continue
            try:
                rows.append({c: int(x) for c, x in zip(COLS, v)})
            except ValueError:
                continue
    return rows


def summarize(rows):
    end = rows[-1]["t"]
    w = [r for r in rows if end - 125000 <= r["t"] <= end - 5000]
    if len(w) < 10:
        return None
    a, b = w[0], w[-1]
    s = (b["t"] - a["t"]) / 1000.0

    def rate(c, scale=1.0):
        return (b[c] - a[c]) * scale / s

    mib = PAGE / 2**20
    return {
        "dirtied MB/s": rate("nr_dirtied", PAGE / 1e6),
        "cg dirty MiB mean/max": (
            statistics.mean(r["cg_file_dirty"] for r in w) / 2**20,
            max(r["cg_file_dirty"] for r in w) / 2**20),
        "cg writeback MiB mean/max": (
            statistics.mean(r["cg_file_writeback"] for r in w) / 2**20,
            max(r["cg_file_writeback"] for r in w) / 2**20),
        "cg pgsteal MiB/s": rate("cg_pgsteal", mib),
        "cg pgscan/pgsteal": ((b["cg_pgscan"] - a["cg_pgscan"]) /
                              max(1, b["cg_pgsteal"] - a["cg_pgsteal"])),
        "cg refault MiB/s": rate("cg_refault", mib),
        "cg memory full ms/s": rate("cg_mem_full_us", 1e-3),
        "cg io full ms/s": rate("cg_io_full_us", 1e-3),
        "pgmajfault/s": rate("pgmajfault"),
        "runner jobs seen": max(r["jobs"] for r in w),
    }


def main():
    d = sys.argv[1]
    rx = re.compile(sys.argv[2]) if len(sys.argv) > 2 else None
    for p in sorted(glob.glob(os.path.join(d, "*.txt"))):
        tag = os.path.basename(p)[:-4]
        if rx and not rx.search(tag):
            continue
        rows = load(p)
        s = summarize(rows) if rows else None
        if not s:
            continue
        parts = []
        for k, v in s.items():
            if isinstance(v, tuple):
                parts.append(f"{k} {v[0]:.0f}/{v[1]:.0f}")
            elif isinstance(v, float):
                parts.append(f"{k} {v:.2f}")
            else:
                parts.append(f"{k} {v}")
        print(f"{tag}: " + "; ".join(parts))


if __name__ == "__main__":
    main()
