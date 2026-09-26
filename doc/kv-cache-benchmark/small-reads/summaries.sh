#!/usr/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Small cold reads (issue #29): regenerate the small-reads-*-summary.txt
# files from the raw Linux data in linux/.  Usage: summaries.sh (from this
# directory).
set -eu
cd "$(dirname "$0")"
S=summarize.py
note() {  # FILE TEXT...
  local f=$1; shift
  printf '%s\n' "$@" > "$f"
}
note small-reads-cold-summary.txt \
  "# Cold sweep: kv_bench --seconds 1 --threads 1 --skip-multiprocess, eight" \
  "# block sizes, page cache dropped before first touch and restart; main" \
  "# (base), the change (after) and LMDB interleaved.  Median [per run], GB/s" \
  "# (put, first touch, restart, copy) or gets/s (view)." ""
{ echo "## after / lmdb"; python3 $S linux cold after lmdb; echo
  echo "## after / base"; python3 $S linux cold after base; } \
  >> small-reads-cold-summary.txt
note small-reads-cold-earlier-summary.txt \
  "# The same cold sweep on an earlier build of the change (171a48f: before" \
  "# the write-cursor clamp, the resident-run probe and the unchecked hint)," \
  "# run first; linux/earlier-build/." ""
{ echo "## after / lmdb"; python3 $S linux/earlier-build cold after lmdb; echo
  echo "## after / base"; python3 $S linux/earlier-build cold after base; } \
  >> small-reads-cold-earlier-summary.txt
note small-reads-warm-summary.txt \
  "# Warm check: kv_bench --block-size 65536 --block-size 262144" \
  "# --block-size 524288 --seconds 5 --threads 1,4 (all phases, including the" \
  "# 4-process phase), main (base) and the change (after) interleaved." \
  "# Build 171a48f of the change (linux/earlier-build/): its validated warm" \
  "# path is the final one.  The final build's warm set" \
  "# (linux/disturbed-warm/) ran into a container job and is not used." ""
python3 $S linux/earlier-build warm after base >> small-reads-warm-summary.txt
note small-reads-resident-summary.txt \
  "# Resident CRC-pending reads: the cold command WITHOUT dropping caches," \
  "# 64 KiB..1 MiB, so first touch and restart CRC-verify documents that are" \
  "# in the page cache.  afterw0 = the change with the window off (every read" \
  "# takes the per-document hint, as a non-sequential first read does)." ""
python3 $S linux resident after base >> small-reads-resident-summary.txt
note small-reads-ablate-summary.txt \
  "# Window size: the change with --sequential-readahead-bytes W (w0 = off:" \
  "# the per-document hint alone), 64 KiB..1 MiB cold.  Earlier build of the" \
  "# change (6c060e9: before the write-cursor clamp, the resident-run probe" \
  "# and the unchecked hint, which do not change what a cold run advises)." ""
python3 $S linux ablate >> small-reads-ablate-summary.txt
note small-reads-normal-summary.txt \
  "# Experiment, not proposed: main with the open-time MADV_RANDOM removed" \
  "# (mapping left at MADV_NORMAL), 64 KiB..1 MiB cold (normal-cold) and" \
  "# 64 KiB warm (normal-warm).  Compare with main in the cold summary." ""
python3 $S linux normal >> small-reads-normal-summary.txt
note small-reads-baseline-summary.txt \
  "# performance_baseline --cache-size 512 --entries 5000 --content-size 4096" \
  "# (4 KB objects, below cold_readahead_min_bytes), base / after interleaved." ""
python3 summarize_tools.py linux >> small-reads-baseline-summary.txt
