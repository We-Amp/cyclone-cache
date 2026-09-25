#!/usr/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Issue #16: does the allocator explain round 5's unexplained 2 MiB PUT
# (1.11 GB/s in the full sweep, 0.59 in a 2 MiB-only re-run)?  main's
# kv_bench, 2 MiB alone vs after a 512 KiB size, with page-fault counts.
set -u
BASE=$HOME/insert-path-base; FIX=$HOME/insert-path-fix2
OUT=$HOME/kvresults/insert-path/ab4; DATA=$HOME/kvdata/insert-path
mkdir -p "$OUT" "$DATA"; rm -f "$OUT/DONE"
trap 'rm -rf "${DATA:?}"/*; touch "$OUT/DONE"' EXIT
DROP='sync; echo 3 | sudo -n tee /proc/sys/vm/drop_caches >/dev/null'
prog() { echo "$(date +%T) $* load: $(cat /proc/loadavg)" >> "$OUT/progress.txt"; }
idle() { "$HOME/insert-path-idle.sh" > /dev/null; }

run() {  # run TAG LOG CMD...
  local tag=$1 log=$2; shift 2
  idle
  prog "start $tag"
  { echo "=== $(date -Is) $tag: $*"; echo "load: $(cat /proc/loadavg)"; } >> "$log"
  /usr/bin/time -v "$@" >> "$log" 2>&1
  echo "rc=$? load-after: $(cat /proc/loadavg)" >> "$log"
  rm -rf "${DATA:?}"/*
}

for rep in 1 2; do
  for t in base fix; do
    tree=$BASE; [ $t = fix ] && tree=$FIX
    run "kv_bench $t 2MiB-only r$rep" "$OUT/kv-bench-order.txt" \
      "$tree/build/kv_bench" --block-size 2097152 --seconds 1 \
      --skip-multiprocess --path "$DATA" --drop-caches-cmd "$DROP" \
      --output "$OUT/kv-bench-$t-2m-r$rep.jsonl"
    run "kv_bench $t 512KiB-then-2MiB r$rep" "$OUT/kv-bench-order.txt" \
      "$tree/build/kv_bench" --block-size 524288 --block-size 2097152 \
      --seconds 1 --skip-multiprocess --path "$DATA" \
      --drop-caches-cmd "$DROP" --output "$OUT/kv-bench-$t-sweep-r$rep.jsonl"
  done
done
prog "all done"
