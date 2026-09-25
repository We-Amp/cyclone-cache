#!/usr/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Round 6: 1 Hz memory / writeback sampler, run beside every benchmark
# point until the file $1 exists.  Reads /proc and cgroup files only.
#
# Columns (counters are cumulative; the summariser takes deltas):
#   t_ms runner_jobs
#   global /proc/vmstat: nr_dirty nr_writeback nr_dirtied nr_written
#     pgscan_direct pgsteal_direct pgscan_kswapd pgsteal_kswapd
#     allocstall_normal allocstall_movable workingset_refault_file pgmajfault
#   benchmark's cgroup memory.stat: file file_dirty file_writeback pgscan
#     pgsteal workingset_refault_file   ("-" before the process starts)
#   cgroup memory.pressure / io.pressure "full" total (µs stalled)
stop=$1
vm() {
  awk '/^(nr_dirty|nr_writeback|nr_dirtied|nr_written|pgscan_direct|pgsteal_direct|pgscan_kswapd|pgsteal_kswapd|allocstall_normal|allocstall_movable|workingset_refault_file|pgmajfault) /{v[$1]=$2}
       END{printf "%s %s %s %s %s %s %s %s %s %s %s %s", v["nr_dirty"], v["nr_writeback"], v["nr_dirtied"], v["nr_written"], v["pgscan_direct"], v["pgsteal_direct"], v["pgscan_kswapd"], v["pgsteal_kswapd"], v["allocstall_normal"], v["allocstall_movable"], v["workingset_refault_file"], v["pgmajfault"]}' /proc/vmstat
}
cg=""
while [ ! -e "$stop" ]; do
  if [ -z "$cg" ]; then
    pid=$(pgrep -n -x 'kv_churn|kvchurn|kv_bench|kvpeer')
    if [ -n "$pid" ]; then
      rel=$(sed -n 's/^0:://p' "/proc/$pid/cgroup" 2>/dev/null)
      [ -n "$rel" ] && [ "$rel" != / ] && cg=/sys/fs/cgroup$rel
    fi
  fi
  cgs="- - - - - - - -"
  if [ -n "$cg" ] && [ -r "$cg/memory.stat" ]; then
    cgs=$(awk '/^(file|file_dirty|file_writeback|pgscan|pgsteal|workingset_refault_file) /{v[$1]=$2}
      END{printf "%s %s %s %s %s %s", v["file"], v["file_dirty"], v["file_writeback"], v["pgscan"], v["pgsteal"], v["workingset_refault_file"]}' "$cg/memory.stat")
    mp=$(awk '/^full/{sub("total=","",$5); print $5}' "$cg/memory.pressure" 2>/dev/null)
    ip=$(awk '/^full/{sub("total=","",$5); print $5}' "$cg/io.pressure" 2>/dev/null)
    cgs="$cgs ${mp:--} ${ip:--}"
  fi
  echo "$(date +%s%3N) $(pgrep -c -x Runner.Worker) $(vm) $cgs"
  sleep 1
done
