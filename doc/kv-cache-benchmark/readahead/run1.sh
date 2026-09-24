#!/usr/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# One benchmark invocation with the vmstat/diskstats sampler.
# usage: run1.sh TAG BINARY ARGS...   (env PERF=1: perf-record the first
# cold phase for 2.5 s, cpu-clock + sched_switch with call graphs)
set -u
tag=$1; bin=$2; shift 2
OUT=$HOME/ra512/out; DATA=$HOME/kvdata/ra512
mkdir -p "$OUT" "$DATA"
# The machine is shared: wait until no process matches BACKGROUND_JOB_PATTERN
# (if set) and the 1-minute load has settled below 1.0 before starting.
while { [ -n "${BACKGROUND_JOB_PATTERN:-}" ] &&
        pgrep -f "$BACKGROUND_JOB_PATTERN" >/dev/null; } ||
      awk '{exit !($1 >= 1.0)}' /proc/loadavg; do sleep 10; done
rm -rf "${DATA:?}"/*; rm -f "$OUT/$tag.DONE" "$OUT/$tag.stop" "$OUT/$tag.perf"
DROP="sync; echo 3 | sudo -n tee /proc/sys/vm/drop_caches >/dev/null; echo PHASE \$(date +%s%3N) >> $OUT/$tag.marks"
if [ "${PERF:-0}" = 1 ]; then
  DROP="$DROP; [ -e $OUT/$tag.perf ] || { touch $OUT/$tag.perf; sudo -n perf record -q -g -e cpu-clock -e sched:sched_switch -p \$PPID -o $OUT/$tag.perf.data -- sleep 2.5 >/dev/null 2>&1 & sleep 0.3; }"
fi
echo "$(date -Is) start load: $(cat /proc/loadavg) commit: ${COMMIT:-?}" > "$OUT/$tag.marks"
"$HOME/ra512/sampler.sh" "$OUT/$tag.stop" > "$OUT/$tag.samples" &
"$bin" --path "$DATA" --drop-caches-cmd "$DROP" --output "$OUT/$tag.jsonl" "$@" > "$OUT/$tag.log" 2>&1
echo "rc=$? end $(date +%s%3N) load: $(cat /proc/loadavg)" >> "$OUT/$tag.marks"
touch "$OUT/$tag.stop"; wait
rm -rf "${DATA:?}"/*
touch "$OUT/$tag.DONE"
