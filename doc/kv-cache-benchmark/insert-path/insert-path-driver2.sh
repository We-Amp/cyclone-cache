#!/usr/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Issue #16, second pass: the final tree (small-object coalescing added, and
# insert_bench --reserve generating in place) against main.
set -u
BASE=$HOME/insert-path-base; FIX=$HOME/insert-path-fix2
OUT=$HOME/kvresults/insert-path/ab2; DATA=$HOME/kvdata/insert-path
mkdir -p "$OUT" "$DATA"; rm -f "$OUT/DONE"
trap 'rm -rf "${DATA:?}"/*; touch "$OUT/DONE"' EXIT
prog() { echo "$(date +%T) $* load: $(cat /proc/loadavg)" >> "$OUT/progress.txt"; }
idle() { "$HOME/insert-path-idle.sh" > /dev/null; }

run() {  # run TAG LOG CMD...
  local tag=$1 log=$2; shift 2
  idle
  prog "start $tag"
  { echo "=== $(date -Is) $tag: $*"; echo "load: $(cat /proc/loadavg)"; } >> "$log"
  "$@" >> "$log" 2>&1
  echo "rc=$? load-after: $(cat /proc/loadavg)" >> "$log"
  rm -rf "${DATA:?}"/*
}

for rep in 1 2 3 4 5; do
  for t in base fix; do
    tree=$BASE; [ $t = fix ] && tree=$FIX
    (cd "$DATA" && run "perf_baseline $t r$rep" "$OUT/perf-baseline-$t.txt" \
      "$tree/build/performance_baseline" --cache-size 512 --entries 5000 \
      --content-size 4096)
  done
done
for rep in 1 2 3; do
  for bs in 2097152 524288; do
    run "insert fix $bs r$rep" "$OUT/insert-bench.txt" \
      "$FIX/build/insert_bench" --block-size $bs --capacity 4294967296 \
      --laps 2 --path "$DATA" --output "$OUT/insert-fix.jsonl"
    run "insert fix-reserve $bs r$rep" "$OUT/insert-bench.txt" \
      "$FIX/build/insert_bench" --block-size $bs --capacity 4294967296 \
      --laps 2 --path "$DATA" --reserve --output "$OUT/insert-fix-reserve.jsonl"
  done
done
# Where the remaining time goes: the final tree under perf.
idle
prog "perf fix"
sudo -n perf record -g -o "$OUT/perf-fix.data" "$FIX/build/insert_bench" \
  --block-size 2097152 --capacity 4294967296 --laps 2 --path "$DATA" \
  > "$OUT/perf-fix-run.txt" 2>&1
rm -rf "${DATA:?}"/*
prog "all done"
