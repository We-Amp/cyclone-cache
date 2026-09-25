#!/usr/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Issue #16, same-day peers for the acceptance criteria: file-per-block (and
# LMDB) at T=1 on the churn workload, and file-per-block PUT beside the
# final tree's kv_bench PUT.
set -u
FIX=$HOME/insert-path-fix2; PEER=$HOME/cyclone-kv-bench/build
OUT=$HOME/kvresults/insert-path/ab3; DATA=$HOME/kvdata/insert-path
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
  "$@" >> "$log" 2>&1
  echo "rc=$? load-after: $(cat /proc/loadavg)" >> "$log"
  rm -rf "${DATA:?}"/*
}

for rep in 1 2 3; do
  run "kv_bench put fix r$rep" "$OUT/kv-bench-fix.txt" \
    "$FIX/build/kv_bench" --block-size 2097152 --seconds 2 \
    --skip-multiprocess --path "$DATA" --drop-caches-cmd "$DROP" \
    --output "$OUT/kv-bench-fix-r$rep.jsonl"
  run "kvpeer filedir put r$rep" "$OUT/kv-bench-filedir.txt" \
    "$PEER/kvpeer" --store filedir --block-size 2097152 --seconds 2 \
    --threads 1 --skip-multiprocess --path "$DATA" --drop-caches-cmd "$DROP" \
    --output "$OUT/kv-bench-filedir-r$rep.jsonl"
done

C=17179869184
for rep in 1 2; do
  for st in filedir lmdb; do
    idle
    prog "churn $st T=1 r$rep"
    PEER_BUILD=$PEER DATA=$DATA/churn \
      bash "$FIX/doc/kv-cache-benchmark/churn/run-churn.sh" "$st" zipf 1 \
      $C 120 "$OUT/churn-$st.jsonl" "$OUT/churn-$st.txt" \
      --block-size 2097152 --max-warmup-seconds 1800
  done
done
prog "all done"
