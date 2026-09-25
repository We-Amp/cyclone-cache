#!/usr/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Issue #16 before/after driver: base (main 6e2077e + insert_bench) vs fix.
# Interleaved, one benchmark at a time, idle-gated.
set -u
BASE=$HOME/insert-path-base; FIX=$HOME/insert-path-fix
OUT=$HOME/kvresults/insert-path/ab; DATA=$HOME/kvdata/insert-path
mkdir -p "$OUT" "$DATA"; rm -f "$OUT/DONE"
trap 'rm -rf "${DATA:?}"/*; touch "$OUT/DONE"' EXIT
DROP='sync; echo 3 | sudo -n tee /proc/sys/vm/drop_caches >/dev/null'
prog() { echo "$(date +%T) $* load: $(cat /proc/loadavg)" >> "$OUT/progress.txt"; }
idle() { "$HOME/insert-path-idle.sh" > /dev/null; }
sysctl vm.dirty_ratio vm.dirty_background_ratio vm.dirty_bytes > "$OUT/sysctl.txt"

run() {  # run TAG LOG CMD...
  local tag=$1 log=$2; shift 2
  idle
  prog "start $tag"
  { echo "=== $(date -Is) $tag: $*"; echo "load: $(cat /proc/loadavg)"; } >> "$log"
  "$@" >> "$log" 2>&1
  echo "rc=$? load-after: $(cat /proc/loadavg)" >> "$log"
  rm -rf "${DATA:?}"/*
}

if [ "${SKIP_MICRO:-0}" != 1 ]; then
for rep in 1 2 3; do
  for bs in 2097152 524288; do
    run "insert base $bs r$rep" "$OUT/insert-bench.txt" \
      "$BASE/build/insert_bench" --block-size $bs --capacity 4294967296 \
      --laps 2 --path "$DATA" --output "$OUT/insert-base.jsonl"
    run "insert fix $bs r$rep" "$OUT/insert-bench.txt" \
      "$FIX/build/insert_bench" --block-size $bs --capacity 4294967296 \
      --laps 2 --path "$DATA" --output "$OUT/insert-fix.jsonl"
    run "insert fix-reserve $bs r$rep" "$OUT/insert-bench.txt" \
      "$FIX/build/insert_bench" --block-size $bs --capacity 4294967296 \
      --laps 2 --path "$DATA" --reserve --output "$OUT/insert-fix-reserve.jsonl"
  done
  for t in base fix; do
    tree=$BASE; [ $t = fix ] && tree=$FIX
    (cd "$DATA" && run "perf_baseline $t r$rep" "$OUT/perf-baseline-$t.txt" \
      "$tree/build/performance_baseline" --cache-size 512 --entries 5000 \
      --content-size 4096)
    run "kv_bench put $t r$rep" "$OUT/kv-bench-$t.txt" \
      "$tree/build/kv_bench" --block-size 2097152 --seconds 2 \
      --skip-multiprocess --path "$DATA" --drop-caches-cmd "$DROP" \
      --output "$OUT/kv-bench-$t-r$rep.jsonl"
  done
done
for rep in 1 2; do
  for t in base fix; do
    tree=$BASE; [ $t = fix ] && tree=$FIX
    (cd "$DATA" && run "read_bench $t r$rep" "$OUT/read-bench-$t.txt" \
      "$tree/build/concurrent_read_bench" 20000 512 2 0 512 ramoff)
  done
done
fi

if [ "${SKIP_CHURN:-0}" != 1 ]; then
C=17179869184
export DATA_CHURN=$DATA/churn
for rep in 1 2 3; do
  for pt in "base 1" "fix 1" "fix-reserve 1" "base 4" "fix 4"; do
    set -- $pt; t=$1; th=$2
    tree=$BASE; extra=()
    [ $t != base ] && tree=$FIX
    [ $t = fix-reserve ] && extra=(--reserve)
    idle
    prog "churn $t T=$th r$rep"
    CYCLONE_BUILD=$tree/build DATA=$DATA_CHURN \
      bash "$FIX/doc/kv-cache-benchmark/churn/run-churn.sh" cyclone zipf "$th" \
      $C 120 "$OUT/churn-$t.jsonl" "$OUT/churn-$t.txt" \
      --block-size 2097152 --max-warmup-seconds 1800 "${extra[@]}"
  done
done
fi
prog "all done"
