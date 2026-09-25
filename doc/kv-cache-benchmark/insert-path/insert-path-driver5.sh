#!/usr/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Issue #16, review follow-up: 4 KiB performance_baseline after the head is
# built with room for a coalesced small object.  main vs the PR head before
# the change (c9942cf) vs after, interleaved, five rounds.
set -u
MAIN=$HOME/insert-path-base; PREV=$HOME/insert-path-fix2
NEW=$HOME/insert-path-fix3
OUT=$HOME/kvresults/insert-path/ab5; DATA=$HOME/kvdata/insert-path
mkdir -p "$OUT" "$DATA"; rm -f "$OUT/DONE"
trap 'rm -rf "${DATA:?}"/*; touch "$OUT/DONE"' EXIT
prog() { echo "$(date +%T) $* load: $(cat /proc/loadavg)" >> "$OUT/progress.txt"; }
for rep in 1 2 3 4 5; do
  for t in main prev new; do
    case $t in main) tree=$MAIN ;; prev) tree=$PREV ;; new) tree=$NEW ;; esac
    "$HOME/insert-path-idle.sh" > /dev/null
    prog "perf_baseline $t r$rep"
    { echo "=== $(date -Is) $t r$rep"; echo "load: $(cat /proc/loadavg)"; } \
      >> "$OUT/perf-baseline-$t.txt"
    (cd "$DATA" && "$tree/build/performance_baseline" --cache-size 512 \
      --entries 5000 --content-size 4096 >> "$OUT/perf-baseline-$t.txt" 2>&1)
    rm -rf "${DATA:?}"/*
  done
done
prog "all done"
