#!/usr/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# One churn point (doc/kv-cache-benchmark/kv-churn-spec.md) inside a memory
# cgroup, then wipe the store.
#
#   run-churn.sh STORE PATTERN THREADS CAPACITY SECONDS OUT.jsonl LOG [ARGS...]
#
# STORE is cyclone (benchmarks/kv_churn) or lmdb / filedir (the peer
# harness's kvchurn).  Environment: CYCLONE_BUILD (dir holding kv_churn),
# PEER_BUILD (dir holding kvchurn), DATA (store directory root), MEMORY_MAX
# (cgroup memory.max, default 4G).  Needs passwordless sudo for systemd-run
# and drop_caches.
set -u
store=$1 pattern=$2 threads=$3 capacity=$4 seconds=$5 out=$6 log=$7
shift 7
CYCLONE_BUILD=${CYCLONE_BUILD:-$HOME/cyclone-churn/build}
PEER_BUILD=${PEER_BUILD:-$HOME/cyclone-kv-bench/build}
DATA=${DATA:-$HOME/kvdata/churn}
MEMORY_MAX=${MEMORY_MAX:-4G}
DROP='sync; echo 3 | sudo -n tee /proc/sys/vm/drop_caches >/dev/null'

mkdir -p "$DATA"
if [ "$store" = cyclone ]; then
  cmd=("$CYCLONE_BUILD/kv_churn" --path "$DATA/cyclone")
else
  cmd=("$PEER_BUILD/kvchurn" --store "$store" --path "$DATA/$store")
fi
{
  echo "=== $(date -Is) $store $pattern T=$threads C=$capacity s=$seconds $*"
  echo "load: $(cat /proc/loadavg)"
} >> "$log"
sudo -n systemd-run --scope --quiet -p "MemoryMax=$MEMORY_MAX" \
  --uid="$(id -u)" --gid="$(id -g)" \
  "${cmd[@]}" --pattern "$pattern" --threads "$threads" \
  --capacity "$capacity" --seconds "$seconds" --output "$out" \
  --drop-caches-cmd "$DROP" "$@" >> "$log" 2>&1
rc=$?
rm -rf "${DATA:?}"/*
echo "rc=$rc load-after: $(cat /proc/loadavg)" >> "$log"
exit $rc
