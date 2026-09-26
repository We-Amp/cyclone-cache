#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Issue #35: off-CPU time of the benchmark threads split by the user-space
frame on the blocking stack (write path vs read path), from the same perf
script round6/offcpu.py reads.  Stacks without a recognisable user frame are
counted as "unattributed".

  byframe.py PERF_SCRIPT TIDS
"""
import collections
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "round6"))
import offcpu  # noqa: E402

GROUPS = [
    ("write path (commit_write / write handle close)",
     r"commit_write|writer_probe|WriteHandle|close_sync|allocate_write_slot|"
     r"pwrite_parts|pwritev|__libc_pwrite"),
    ("read path (read_sync / borrow / CRC / copy)",
     r"read_sync|chunk_borrow|sse42|crc32c|PosixMappedFile|__memmove|"
     r"load_version|probe"),
    ("sleep (idle helper threads)", r"clock_nanosleep|futex"),
]


def main():
    tids = {int(t) for t in sys.argv[2].split(",")}
    out = {}
    agg = collections.defaultdict(list)
    for e in offcpu.parse(sys.argv[1]):
        if e["event"] != "sched:sched_switch":
            continue
        kv = dict(re.findall(r"(\w+)=(\S+)", e["trace"]))
        prev, nxt = int(kv["prev_pid"]), int(kv["next_pid"])
        if prev in tids:
            out[prev] = (e["t"], e["stack"], kv.get("prev_state", "?"))
        if nxt in out:
            ts, st, state = out.pop(nxt)
            if state.startswith("R"):
                continue
            d = (e["t"] - ts) * 1e3
            g = offcpu.classify(st, GROUPS, "unattributed")
            w = offcpu.classify(st, offcpu.WAIT, "other sleep")
            agg[(g, w)].append(d)
    print(f"{'user-space frame':48s} {'waiting for':44s} {'n':>7s} "
          f"{'total ms':>9s} {'>10ms':>6s} {'>20ms':>6s} {'max ms':>7s}")
    for (g, w), v in sorted(agg.items(), key=lambda kv: -sum(kv[1])):
        print(f"{g:48s} {w:44s} {len(v):7d} {sum(v):9.0f} "
              f"{sum(x > 10 for x in v):6d} {sum(x > 20 for x in v):6d} "
              f"{max(v):7.1f}")


if __name__ == "__main__":
    main()
