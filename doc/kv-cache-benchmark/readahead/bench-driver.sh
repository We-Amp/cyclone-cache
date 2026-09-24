#!/usr/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Before/after driver for the 512 KiB readahead fix (issue #18), on the Linux benchmark machine.
# Ran from ~/ra512 with run1.sh, sampler.sh and phase.sh from this directory.
#   base  = origin/main b94540d          (~/cyclone-ra512-base)
#   after = readahead-512k dd487fb       (~/cyclone-ra512-after)
#   lmdb  = peer harness kvpeer, same day
# One benchmark at a time; each invocation waits for background jobs and load.
set -u
R=$HOME/ra512; OUT=$R/out/final2; mkdir -p "$OUT"; rm -f "$OUT/DONE"
BASE=$HOME/cyclone-ra512-base/build; AFTER=$HOME/cyclone-ra512-after/build
PEER=$HOME/cyclone-kv-bench/build/kvpeer
{
  date -Is; uname -a
  cat /sys/block/nvme0n1/device/model /sys/block/nvme0n1/device/firmware_rev
  for q in read_ahead_kb max_sectors_kb max_hw_sectors_kb scheduler; do
    echo "$q: $(cat /sys/block/nvme0n1/queue/$q)"; done
  sysctl vm.dirty_ratio vm.dirty_background_ratio vm.dirty_bytes
  clang++-20 --version | head -1
} > "$OUT/machine.txt" 2>&1
run() {  # tag bin args...
  local tag=$1; shift
  echo "$(date +%T) $tag load: $(cat /proc/loadavg)" >> "$OUT/progress.txt"
  "$R/run1.sh" "final2/$tag" "$@"
}
# Full unit/integration suite of the after-tree first.
(cd "$HOME/cyclone-ra512-after/build" && ./cyclone-tests > "$OUT/tests.txt" 2>&1;
 echo "rc=$?" >> "$OUT/tests.txt")
# Warm-path check at 512 KiB (re-advise cost), incl. the 4-process phase.
for rep in 1 2 3; do
  COMMIT=b94540d run "warm-base-$rep" "$BASE/kv_bench" --block-size 524288 \
    --seconds 5 --threads 1,4
  COMMIT=dd487fb run "warm-after-$rep" "$AFTER/kv_bench" --block-size 524288 \
    --seconds 5 --threads 1,4
done
COLD=(--block-size 65536 --block-size 262144 --block-size 524288
      --block-size 1048576 --block-size 2097152 --block-size 8388608
      --block-size 33554432 --seconds 1 --threads 1 --skip-multiprocess)
for rep in 1 2 3; do
  COMMIT=b94540d run "cold-base-$rep" "$BASE/kv_bench" "${COLD[@]}"
  COMMIT=dd487fb run "cold-after-$rep" "$AFTER/kv_bench" "${COLD[@]}"
  run "cold-lmdb-$rep" "$PEER" --store lmdb "${COLD[@]}"
done
# Full round-5 sweep (all phases, 4 sizes, 10 s points, 4-process phase).
COMMIT=b94540d run full-base "$BASE/kv_bench" --seconds 10
COMMIT=dd487fb run full-after "$AFTER/kv_bench" --seconds 10
# 4 KB HTTP-object baseline (below the readahead threshold; must not move).
for rep in 1 2 3; do
  for t in base after; do
    b=$BASE; [ $t = after ] && b=$AFTER
    while { [ -n "${BACKGROUND_JOB_PATTERN:-}" ] &&
            pgrep -f "$BACKGROUND_JOB_PATTERN" >/dev/null; } ||
          awk '{exit !($1 >= 1.0)}' /proc/loadavg; do sleep 10; done
    echo "$(date +%T) perfbase-$t-$rep load: $(cat /proc/loadavg)" >> "$OUT/progress.txt"
    "$b/performance_baseline" --cache-size 512 --entries 5000 \
      --content-size 4096 > "$OUT/perfbase-$t-$rep.txt" 2>&1
  done
done
# Side experiment (not part of the fix): what the hint would buy below the
# default 256 KiB threshold.  After-tree, default threshold vs 64 KiB.
SMALL=(--block-size 65536 --block-size 131072 --seconds 3 --threads 1,4
       --skip-multiprocess)
COMMIT=dd487fb run thr-default "$AFTER/kv_bench" "${SMALL[@]}"
COMMIT=dd487fb run thr-64k "$AFTER/kv_bench" "${SMALL[@]}" \
  --readahead-min-bytes 65536
touch "$OUT/DONE"
