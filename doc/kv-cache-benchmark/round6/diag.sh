#!/usr/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Round 6: where does the one-thread insert tail go?  Runs one churn point
# (2 MiB, zipf, T=1, C = 16 GiB, 4 GiB cgroup, the round-6 configuration)
# per store and, once its warm-up is over, records 60 s of the measured
# phase with perf:
#   - sched:sched_switch with kernel call chains, filtered to the
#     benchmark's threads (switch-out stack + switch-in time = every
#     off-CPU interval and where it blocked);
#   - writeback:balance_dirty_pages (dirty throttling pauses);
#   - block:block_rq_issue for reads (synchronous read-before-write).
# The churn result of a traced run is recorded but not used in any table.
# Usage: diag.sh TAG STORE [ARGS...]   (e.g. diag.sh cy-ret cyclone
#        --wrap-retention on)
set -u
tag=$1 store=$2; shift 2
SRC=${SRC:-$HOME/cyclone-round6}
PEER_BUILD=${PEER_BUILD:-$HOME/cyclone-kv-bench/build}
R6=${R6:-$HOME/round6}
DATA=${DATA:-$HOME/kvdata/round6}
HERE=$SRC/doc/kv-cache-benchmark/round6
OUT=$R6/out/diag
mkdir -p "$OUT"
log=$OUT/churn-$tag.txt
"$HERE/idle.sh" > /dev/null
echo "$(date +%T) diag $tag load: $(cat /proc/loadavg)" >> "$OUT/progress.txt"
rm -f "$OUT/$tag.stop"
"$HERE/sampler.sh" "$OUT/$tag.stop" > "$OUT/samples-$tag.txt" &
sp=$!
CYCLONE_BUILD=$SRC/build PEER_BUILD=$PEER_BUILD DATA=$DATA/churn \
  bash "$SRC/doc/kv-cache-benchmark/churn/run-churn.sh" "$store" zipf 1 \
  17179869184 120 "$OUT/churn-$tag.jsonl" "$log" --block-size 2097152 \
  --max-warmup-seconds 1800 "$@" &
rp=$!
# Wait for the warm-up line (stderr, unbuffered), then 10 s into the
# measured phase.
until grep -q 'warm-up:' "$log" 2>/dev/null; do sleep 1; done
sleep 10
pid=$(pgrep -n -x 'kv_churn|kvchurn')
filter=""
for t in $(ls "/proc/$pid/task"); do
  filter="$filter${filter:+ || }prev_pid == $t || next_pid == $t"
done
echo "pid $pid tids $(ls "/proc/$pid/task" | tr '\n' ' ')" >> "$log"
sudo -n perf record -o "$OUT/perf-$tag.data" -g -a \
  -e sched:sched_switch --filter "$filter" \
  -e writeback:balance_dirty_pages \
  -e block:block_rq_issue --filter 'rwbs ~ "*R*"' \
  -- sleep 60 > "$OUT/perf-$tag.log" 2>&1
wait "$rp"
touch "$OUT/$tag.stop"; wait "$sp"; rm -f "$OUT/$tag.stop"
sudo -n chown "$(id -u):$(id -g)" "$OUT/perf-$tag.data"
sudo -n perf script -f -i "$OUT/perf-$tag.data" -F comm,tid,cpu,time,event,trace,ip,sym \
  > "$OUT/perf-$tag.script" 2> /dev/null
tids=$(sed -n 's/^pid [0-9]* tids //p' "$log" | tr ' ' ',' | sed 's/,$//')
python3 "$HERE/offcpu.py" "$OUT/perf-$tag.script" "$tids" > "$OUT/offcpu-$tag.txt"
echo "$(date +%T) diag $tag done load: $(cat /proc/loadavg)" >> "$OUT/progress.txt"
