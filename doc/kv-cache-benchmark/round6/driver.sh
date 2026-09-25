#!/usr/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Round 6 driver, Linux benchmark machine.  One benchmark at a time; every
# point waits for idle.sh (no runner job, 1-minute load < 1.0) and runs with
# sampler.sh beside it.  Usage: driver.sh sweep|churn [REPS]
#
#   sweep : kv_bench full sweep (the round-5 command), the 2 MiB
#           verification-off run, the kvpeer full sweeps (LMDB, filedir,
#           RocksDB), and the 7-size cold sweep (64 KiB..32 MiB) for Cyclone
#           and LMDB; REPS interleaved repetitions.
#   churn : the round-5 churn matrix (2 MiB, C = 16 GiB, 4 GiB cgroup, 120 s)
#           plus --reserve and T=1 peers; REPS interleaved repetitions.
#   extra : driver.sh extra REP NAME:PATTERN:T ... re-runs single churn
#           points as repetition REP (the points a runner job started into).
#
# Environment: SRC (Cyclone tree, built in $SRC/build), PEER_BUILD (peer
# harness build dir: kvpeer, kvchurn), R6 (output root), DATA (store root).
set -u
mode=$1 reps=${2:-3}
SRC=${SRC:-$HOME/cyclone-round6}
B=$SRC/build
PEER_BUILD=${PEER_BUILD:-$HOME/cyclone-kv-bench/build}
R6=${R6:-$HOME/round6}
DATA=${DATA:-$HOME/kvdata/round6}
HERE=$SRC/doc/kv-cache-benchmark/round6
OUT=$R6/out
DROP='sync; echo 3 | sudo -n tee /proc/sys/vm/drop_caches >/dev/null'
mkdir -p "$OUT/samples" "$DATA"
rm -f "$OUT/DONE-$mode"

if [ ! -e "$OUT/machine.txt" ]; then
  {
    date -Is; uname -r
    cat /sys/block/nvme0n1/device/model /sys/block/nvme0n1/device/firmware_rev
    for q in read_ahead_kb max_sectors_kb scheduler; do
      echo "$q: $(cat /sys/block/nvme0n1/queue/$q)"; done
    sysctl vm.dirty_ratio vm.dirty_background_ratio vm.dirty_bytes \
      vm.dirty_background_bytes vm.dirty_expire_centisecs \
      vm.dirty_writeback_centisecs
    clang++-20 --version | head -1
    echo "cyclone: $(cat "$SRC/COMMIT" 2>/dev/null)"
    "$B/kv_bench" --print-vectors
    echo "--- kv_churn"; "$B/kv_churn" --print-vectors
    echo "--- kvchurn"; "$PEER_BUILD/kvchurn" --print-vectors
  } > "$OUT/machine.txt" 2>&1
fi

# point TAG CMD...  : wait for idle, sample, run, wipe the store.
point() {
  local tag=$1; shift
  "$HERE/idle.sh" > /dev/null
  echo "$(date +%T) $tag load: $(cat /proc/loadavg)" >> "$OUT/progress.txt"
  rm -f "$OUT/samples/$tag.stop"
  "$HERE/sampler.sh" "$OUT/samples/$tag.stop" > "$OUT/samples/$tag.txt" &
  local sp=$!
  "$@"
  local rc=$?
  touch "$OUT/samples/$tag.stop"; wait "$sp"; rm -f "$OUT/samples/$tag.stop"
  rm -rf "${DATA:?}"/*
  echo "$(date +%T) $tag rc=$rc load: $(cat /proc/loadavg)" >> "$OUT/progress.txt"
}

run_bench() {  # TAG CMD... ; JSONL and log per tag
  local tag=$1; shift
  "$@" --output "$OUT/$tag.jsonl" --path "$DATA" --drop-caches-cmd "$DROP" \
    > "$OUT/$tag.log" 2>&1
}
bench() { point "$1" run_bench "$@"; }

COLD=(--block-size 65536 --block-size 262144 --block-size 524288
      --block-size 1048576 --block-size 2097152 --block-size 8388608
      --block-size 33554432 --seconds 1 --threads 1 --skip-multiprocess)

churn() {  # TAG STORE PATTERN THREADS [ARGS...]
  local tag=$1 store=$2 pattern=$3 t=$4; shift 4
  point "$tag" env CYCLONE_BUILD="$B" PEER_BUILD="$PEER_BUILD" \
    DATA="$DATA/churn" bash "$SRC/doc/kv-cache-benchmark/churn/run-churn.sh" \
    "$store" "$pattern" "$t" 17179869184 120 "$OUT/churn-$tag.jsonl" \
    "$OUT/churn-$tag.txt" --block-size 2097152 --max-warmup-seconds 1800 "$@"
}

case $mode in
  sweep)
    for r in $(seq 1 "$reps"); do
      bench "cyclone-full-$r" "$B/kv_bench" --seconds 10
      bench "cyclone-noverify-$r" "$B/kv_bench" --block-size 2097152 \
        --no-mmap-dir --no-verify --seconds 10
      bench "lmdb-full-$r" "$PEER_BUILD/kvpeer" --store lmdb --seconds 10
      bench "filedir-full-$r" "$PEER_BUILD/kvpeer" --store filedir --seconds 10
      bench "rocksdb-full-$r" "$PEER_BUILD/kvpeer" --store rocksdb --seconds 10
      bench "cyclone-cold-$r" "$B/kv_bench" "${COLD[@]}"
      bench "lmdb-cold-$r" "$PEER_BUILD/kvpeer" --store lmdb "${COLD[@]}"
    done ;;
  churn)
    for r in $(seq 1 "$reps"); do
      for p in zipf zipf+scan; do
        for t in 1 4; do
          churn "cy-flush-$p-t$t-$r" cyclone "$p" "$t" --wrap-retention off
          churn "cy-ret-$p-t$t-$r" cyclone "$p" "$t" --wrap-retention on
          churn "cy-reserve-$p-t$t-$r" cyclone "$p" "$t" --wrap-retention on \
            --reserve
          churn "lmdb-$p-t$t-$r" lmdb "$p" "$t"
          churn "filedir-$p-t$t-$r" filedir "$p" "$t"
        done
      done
    done
    "$B/kv_churn_policy" > "$OUT/policy-replay.txt" 2>&1 ;;
  extra)
    # driver.sh extra REP NAME:PATTERN:T ...  -- re-run single churn points
    # (those a runner job started into) as repetition REP.
    shift 2
    for spec in "$@"; do
      IFS=: read -r name p t <<< "$spec"
      case $name in
        cy-flush) churn "$name-$p-t$t-$reps" cyclone "$p" "$t" --wrap-retention off ;;
        cy-ret) churn "$name-$p-t$t-$reps" cyclone "$p" "$t" --wrap-retention on ;;
        cy-reserve) churn "$name-$p-t$t-$reps" cyclone "$p" "$t" \
          --wrap-retention on --reserve ;;
        lmdb|filedir) churn "$name-$p-t$t-$reps" "$name" "$p" "$t" ;;
      esac
    done ;;
esac
touch "$OUT/DONE-$mode"
