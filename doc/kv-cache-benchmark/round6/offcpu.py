#!/usr/bin/env python3

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Round 6: off-CPU intervals of the benchmark's threads, from diag.sh's
`perf script` output.

Every sched_switch that takes a benchmark thread off the CPU carries the
kernel stack it blocked in; the next switch that puts it back gives the
interval.  Intervals are grouped by what the thread was waiting for (the
first matching frame, innermost first) and by the syscall or fault that got
it there.  Also sums the balance_dirty_pages pauses and counts device reads
per origin.

  offcpu.py perf-TAG.script TID[,TID...]

TIDs are the benchmark's threads (diag.sh logs them); switches of other
tasks that the filter let through (the task switched out when a benchmark
thread is switched in) are ignored.
"""
import collections
import re
import sys

WAIT = [  # (category, frame regex), first listed match wins
    ("dirty throttle (balance_dirty_pages)", r"^balance_dirty_pages"),
    ("wait for page writeback", r"wait_on_page_writeback|folio_wait_writeback"),
    ("read of a partial page before the write (write_begin)",
     r"__wait_on_buffer|ext4_read_bh|bh_read|ll_rw_block|__block_write_begin|ext4_da_write_begin"),
    ("memcg reclaim", r"try_to_free_mem_cgroup_pages|shrink_node|shrink_lruvec|mem_cgroup_handle_over_high|reclaim_throttle"),
    ("read I/O in a page fault (mmap)", r"filemap_fault|do_read_fault"),
    ("read I/O in a buffered read (pread)", r"filemap_read|filemap_get_pages"),
    ("lock", r"rwsem_down|mutex_lock|__mutex_lock|down_read|down_write"),
    ("jbd2 / journal", r"jbd2|start_this_handle|__ext4_journal_start"),
]
ORIGIN = [
    ("pwrite", r"__x64_sys_pwrite64|ksys_pwrite64"),
    ("writev/write", r"__x64_sys_writev|__x64_sys_write\b|ksys_write|do_writev"),
    ("pread/preadv", r"__x64_sys_pread64|__x64_sys_preadv"),
    ("page fault", r"asm_exc_page_fault|exc_page_fault|handle_mm_fault"),
    ("madvise", r"__x64_sys_madvise"),
    ("rename/unlink/open", r"__x64_sys_rename|__x64_sys_unlink|__x64_sys_openat|do_sys_open"),
    ("fsync/syncfs", r"sys_syncfs|sys_fsync|sys_fdatasync"),
    ("futex/sleep", r"__x64_sys_futex|__x64_sys_nanosleep|__x64_sys_clock_nanosleep"),
]

HDR = re.compile(r"^\s*(\S.*?)\s+(\d+)\s+\[(\d+)\]\s+([\d.]+):\s+([\w:]+):\s*(.*)$")


def parse(path):
    ev = None
    with open(path, errors="replace") as f:
        for line in f:
            if not line.strip():
                if ev:
                    yield ev
                ev = None
                continue
            m = HDR.match(line)
            if m and not line.startswith("\t"):
                if ev:
                    yield ev
                ev = {"comm": m.group(1), "tid": int(m.group(2)),
                      "t": float(m.group(4)), "event": m.group(5),
                      "trace": m.group(6), "stack": []}
            elif ev is not None:
                parts = line.strip().split(None, 1)
                if len(parts) == 2:
                    ev["stack"].append(re.sub(r"\+0x[0-9a-f]+$", "", parts[1]))
    if ev:
        yield ev


def classify(stack, table, default):
    for name, rx in table:
        r = re.compile(rx)
        if any(r.search(fr) for fr in stack):
            return name
    return default


def main():
    path = sys.argv[1]
    tids = {int(t) for t in sys.argv[2].split(",")}
    out = {}
    iv = collections.defaultdict(list)
    stacks = collections.defaultdict(list)
    bdp = collections.defaultdict(list)
    reads = collections.Counter()
    t0 = t1 = None
    for e in parse(path):
        t0 = e["t"] if t0 is None else t0
        t1 = e["t"]
        if e["event"] == "sched:sched_switch":
            kv = dict(re.findall(r"(\w+)=(\S+)", e["trace"]))
            prev, nxt = int(kv["prev_pid"]), int(kv["next_pid"])
            if prev in tids:
                out[prev] = (e["t"], e["stack"], kv.get("prev_state", "?"))
            if nxt in out:
                ts, st, state = out.pop(nxt)
                d = (e["t"] - ts) * 1e3
                if state.startswith("R"):
                    cat = "preempted (runnable)"
                else:
                    cat = classify(st, WAIT, "other sleep")
                org = classify(st, ORIGIN, "other")
                iv[(cat, org)].append(d)
                key = " <- ".join(f for f in st if f not in (
                    "__schedule", "schedule", "io_schedule"))[:400]
                stacks[(nxt, key)].append(d)
        elif e["event"] == "writeback:balance_dirty_pages":
            if e["tid"] not in tids:
                continue
            kv = dict(re.findall(r"(\w+)=(\S+)", e["trace"]))
            bdp[e["comm"]].append(int(kv.get("pause", 0)))
        elif e["event"] == "block:block_rq_issue":
            org = classify(e["stack"], WAIT[2:3] + WAIT[4:6] + [
                ("readahead hint (madvise)", r"__x64_sys_madvise"),
                ("readahead", r"page_cache_ra|ondemand_readahead")],
                "other")
            reads[(e["tid"], e["comm"], org)] += 1
    span = (t1 - t0) if t0 is not None else 0
    print(f"trace span {span:.1f} s")
    print(f"\n{'waiting for':44s} {'via':18s} {'n':>7s} {'total ms':>9s} "
          f"{'>5ms':>6s} {'>10ms':>6s} {'>20ms':>6s} {'max ms':>7s}")
    for (cat, org), v in sorted(iv.items(), key=lambda kv: -sum(kv[1])):
        print(f"{cat:44s} {org:18s} {len(v):7d} {sum(v):9.0f} "
              f"{sum(x > 5 for x in v):6d} {sum(x > 10 for x in v):6d} "
              f"{sum(x > 20 for x in v):6d} {max(v):7.1f}")
    print("\nstacks with the most intervals over 10 ms (tid, innermost first):")
    top = sorted(stacks.items(), key=lambda kv: (-sum(x > 10 for x in kv[1]),
                                                  -sum(kv[1])))[:10]
    for (tid, key), v in top:
        print(f"  tid {tid} n={len(v)} total={sum(v):.0f} ms "
              f">10ms={sum(x > 10 for x in v)} max={max(v):.1f} ms\n    {key}")
    print("\nbalance_dirty_pages events (pause in ms, per event):")
    for comm, v in bdp.items():
        print(f"  {comm}: n={len(v)} sum={sum(v)} ms "
              f"pause>0: {sum(x > 0 for x in v)} max={max(v) if v else 0}")
    if not bdp:
        print("  none")
    print("\ndevice reads issued (block_rq_issue, R), benchmark threads by "
          "origin, then the busiest other tasks:")
    for (tid, comm, org), n in reads.most_common():
        if tid in tids:
            print(f"  {comm:20s} {org:54s} {n}")
    others = collections.Counter()
    for (tid, comm, org), n in reads.items():
        if tid not in tids:
            others[comm] += n
    for comm, n in others.most_common(5):
        print(f"  (other) {comm:12s} {n}")


if __name__ == "__main__":
    main()
